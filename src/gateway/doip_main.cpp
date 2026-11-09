// Vehicle Gateway DoIP entity and UDS router: DoIP (TCP/UDP 13400) <-> kernel ISO-TP <-> ECU UDS servers.
// project.md §9.1. Runs in ns_gateway (practice: hj_gw) as a non-root user. One tester connection at a time.
//
// usage: doip_gateway --eth-addr <ipv4> --can-iface <if> [--duration-s <sec>]
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/can.h>
#include <linux/can/isotp.h>

#include "doip.hpp"

using namespace sil::doip;
using Clock = std::chrono::steady_clock;

namespace {

volatile std::sig_atomic_t g_stop;

void on_signal(int)
{
	g_stop = 1;
}

void event_log(const std::string &what)
{
	std::fprintf(stderr, "{\"component\": \"doip_gateway\", \"event\": \"%s\"}\n", what.c_str());
}

std::int64_t ms_since(Clock::time_point t)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

int open_isotp(const char *ifname, const sil_diag_ecu_t &ecu)
{
	sockaddr_can addr{};
	can_isotp_options opts{};
	int fd = socket(PF_CAN, SOCK_DGRAM | SOCK_CLOEXEC, CAN_ISOTP);
	if (fd < 0)
		return -1;
	// WAIT_TX_DONE: send() returns only after the whole PDU went out, so a missing Flow Control
	// (ECU gone) is reported as ECOMM instead of being acknowledged to the tester
	opts.flags = CAN_ISOTP_TX_PADDING | CAN_ISOTP_WAIT_TX_DONE;
	opts.txpad_content = SIL_ISOTP_TX_PADDING;
	if (setsockopt(fd, SOL_CAN_ISOTP, CAN_ISOTP_OPTS, &opts, sizeof(opts)) < 0) {
		close(fd);
		return -1;
	}
	addr.can_family = AF_CAN;
	addr.can_ifindex = static_cast<int>(if_nametoindex(ifname));
	addr.can_addr.tp.rx_id = ecu.response_id; // the gateway is the tester side of each ECU link
	addr.can_addr.tp.tx_id = ecu.request_id;
	if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

bool send_all(int fd, const Bytes &b)
{
	std::size_t off = 0;
	while (off < b.size()) {
		ssize_t n = send(fd, b.data() + off, b.size() - off, MSG_NOSIGNAL);
		if (n <= 0)
			return false;
		off += static_cast<std::size_t>(n);
	}
	return true;
}

struct EcuLink {
	const sil_diag_ecu_t *ecu;
	int fd;
	Clock::time_point pending_since{}; // last forwarded request without a response yet
	bool pending = false;
};

// Forward one UDS request, then acknowledge. Responses are relayed asynchronously by the main loop,
// so a request without a response (e.g. TesterPresent with suppressPosRsp) never blocks the next one.
bool forward(int tcp, Session &session, EcuLink &link, const Bytes &uds)
{
	// a multi-frame request blocks here until the ECU's Flow Control or the N_Bs timeout
	if (send(link.fd, uds.data(), uds.size(), 0) != static_cast<ssize_t>(uds.size())) {
		event_log(std::string("isotp_send_failed ecu=") + link.ecu->name + " errno=" + std::strerror(errno));
		return send_all(tcp, session.diagnostic_nack(link.ecu->logical_address,
							    errno == ECOMM ? kDiagNackTargetUnreachable : kDiagNackTransportProtocolError));
	}
	link.pending = true;
	link.pending_since = Clock::now();
	return send_all(tcp, session.diagnostic_ack(link.ecu->logical_address));
}

// Relay every UDS response waiting on the ECU link. Returns false if the tester connection broke.
bool relay(int tcp, const Session &session, EcuLink &link)
{
	std::uint8_t buf[SIL_DOIP_MAX_UDS_BYTES];
	ssize_t n;

	while ((n = recv(link.fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
		const bool pending_rsp = n == 3 && buf[0] == 0x7F && buf[2] == 0x78;
		link.pending = pending_rsp;
		link.pending_since = Clock::now();
		if (tcp < 0) {
			event_log(std::string("uds_response_without_tester ecu=") + link.ecu->name);
			continue;
		}
		if (!send_all(tcp, session.diagnostic_response(link.ecu->logical_address, buf, static_cast<std::size_t>(n))))
			return false;
	}
	return true;
}

} // namespace

int main(int argc, char **argv)
{
	const char *addr_s = nullptr, *ifname = nullptr;
	double duration_s = 0.0;
	bool bad = false;

	for (int i = 1; i < argc && !bad; i++) {
		if (!std::strcmp(argv[i], "--eth-addr") && i + 1 < argc)
			addr_s = argv[++i];
		else if (!std::strcmp(argv[i], "--can-iface") && i + 1 < argc)
			ifname = argv[++i];
		else if (!std::strcmp(argv[i], "--duration-s") && i + 1 < argc)
			duration_s = std::strtod(argv[++i], nullptr);
		else
			bad = true;
	}
	in_addr ip{};
	if (bad || !addr_s || !ifname || inet_pton(AF_INET, addr_s, &ip) != 1 || duration_s < 0) {
		std::fprintf(stderr, "usage: %s --eth-addr <ipv4> --can-iface <if> [--duration-s <sec>]\n", argv[0]);
		return 2;
	}

	struct sigaction sa{};
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, nullptr);
	sigaction(SIGTERM, &sa, nullptr);

	std::map<std::uint16_t, EcuLink> links;
	std::set<std::uint16_t> addresses;
	for (unsigned i = 0; i < SIL_DIAG_ECU_COUNT; i++) {
		const sil_diag_ecu_t &e = sil_diag_ecus[i];
		int fd = open_isotp(ifname, e);
		if (fd < 0) {
			std::fprintf(stderr, "[doip_gateway] ISO-TP %s on %s: %s\n", e.name, ifname, std::strerror(errno));
			return 1;
		}
		links[e.logical_address] = EcuLink{&e, fd};
		addresses.insert(e.logical_address);
	}

	sockaddr_in sin{};
	sin.sin_family = AF_INET;
	sin.sin_port = htons(SIL_DOIP_PORT);
	sin.sin_addr = ip;
	int one = 1;
	int lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	int ufd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	setsockopt(ufd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in uany = sin;
	uany.sin_addr.s_addr = htonl(INADDR_ANY); // vehicle identification may arrive as a broadcast
	if (bind(lfd, reinterpret_cast<sockaddr *>(&sin), sizeof(sin)) < 0 || listen(lfd, 1) < 0 ||
	    bind(ufd, reinterpret_cast<sockaddr *>(&uany), sizeof(uany)) < 0) {
		std::fprintf(stderr, "[doip_gateway] bind %s:%u: %s\n", addr_s, SIL_DOIP_PORT, std::strerror(errno));
		return 1;
	}
	std::fprintf(stderr, "[doip_gateway] listening %s:%u, logical address 0x%04X, CAN %s\n", addr_s, SIL_DOIP_PORT,
		     SIL_DOIP_GATEWAY_ADDRESS, ifname);

	const auto start = Clock::now();
	int tcp = -1;
	Session session(addresses);
	Clock::time_point connected{}, last_rx{};
	unsigned long forwarded = 0, connections = 0;

	while (!g_stop && (duration_s == 0 || ms_since(start) < duration_s * 1000)) {
		std::vector<pollfd> fds = {{lfd, POLLIN, 0}, {ufd, POLLIN, 0}, {tcp, POLLIN, 0}};
		for (auto &kv : links)
			fds.push_back({kv.second.fd, POLLIN, 0});
		poll(fds.data(), fds.size(), 100);

		for (auto &kv : links) {
			EcuLink &l = kv.second;
			if (!relay(tcp, session, l)) {
				close(tcp);
				tcp = -1;
			}
			if (l.pending && ms_since(l.pending_since) > SIL_UDS_P2_STAR_SERVER_MS) {
				// no response within P2*: the tester's own P2 client timer reports it (suppressed responses also end here)
				event_log(std::string("uds_no_response ecu=") + l.ecu->name);
				l.pending = false;
			}
		}

		if (fds[1].revents & POLLIN) {
			std::uint8_t buf[512];
			sockaddr_in from{};
			socklen_t fl = sizeof(from);
			ssize_t n = recvfrom(ufd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &fl);
			if (n > 0)
				if (auto r = handle_udp(buf, static_cast<std::size_t>(n)))
					sendto(ufd, r->data(), r->size(), 0, reinterpret_cast<sockaddr *>(&from), fl);
		}
		if (fds[0].revents & POLLIN) {
			int c = accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC);
			if (c >= 0 && tcp >= 0) {
				event_log("doip_second_connection_refused"); // one tester at a time
				close(c);
			} else if (c >= 0) {
				tcp = c;
				session = Session(addresses);
				connected = last_rx = Clock::now();
				connections++;
			}
		}
		if (tcp >= 0 && (fds[2].revents & (POLLIN | POLLHUP | POLLERR))) {
			std::uint8_t buf[8192];
			ssize_t n = recv(tcp, buf, sizeof(buf), 0);
			bool keep = n > 0;
			if (keep) {
				last_rx = Clock::now();
				for (const Event &e : session.feed(buf, static_cast<std::size_t>(n))) {
					if (!keep)
						break;
					switch (e.kind) {
					case Event::Kind::SendToTester:
						keep = send_all(tcp, e.data);
						break;
					case Event::Kind::Forward:
						forwarded++;
						keep = forward(tcp, session, links.at(e.target), e.data);
						break;
					case Event::Kind::Log:
						event_log(std::string(e.data.begin(), e.data.end()));
						break;
					case Event::Kind::Close:
						keep = false;
						break;
					}
				}
			}
			if (!keep) {
				close(tcp);
				tcp = -1;
			}
		}
		// ISO 13400-2 T_TCP_Initial_Inactivity / T_TCP_General_Inactivity
		if (tcp >= 0 && ((!session.routing_active() && ms_since(connected) > SIL_DOIP_INITIAL_INACTIVITY_MS) ||
				 ms_since(last_rx) > SIL_DOIP_GENERAL_INACTIVITY_MS)) {
			event_log("doip_inactivity_close");
			close(tcp);
			tcp = -1;
		}
	}

	if (tcp >= 0)
		close(tcp);
	close(lfd);
	close(ufd);
	for (auto &kv : links)
		close(kv.second.fd);
	std::printf("{\"component\": \"doip_gateway\", \"connections\": %lu, \"forwarded\": %lu}\n", connections, forwarded);
	return 0;
}
