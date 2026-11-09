// Vehicle Gateway process: CAN (vxcan) -> validation -> state cache -> SOME/IP VehicleState provider,
// and with --lin-iface the LIN master and SOME/IP BodyControl provider.
// project.md §4.1, §5.4, §6, §7.1~7.3. Runs in ns_gateway (practice: hj_gw) as a non-root user.
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/can/raw.h>

#include <vsomeip/vsomeip.hpp>

#include "body_control.hpp"
#include "can_input_validator.hpp"
#include "lin_master.hpp"
#include "sil_diag_gen.h"
#include "sil/heartbeat.h"
#include "sil_services_gen.hpp"
#include "vehicle_state_cache.hpp"
#include "vehicle_state_payload.hpp"

namespace svc = sil::services::VehicleState;
namespace bsvc = sil::services::BodyControl;
using namespace sil::gateway;
using Clock = std::chrono::steady_clock; // CLOCK_MONOTONIC on Linux

namespace {

std::int64_t now_ns()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

int open_can(const char *ifname)
{
	sockaddr_can addr{};
	unsigned idx = if_nametoindex(ifname);
	int fd;

	if (idx == 0) {
		std::fprintf(stderr, "[gateway] interface %s: %s\n", ifname, std::strerror(errno));
		return -1;
	}
	fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
	if (fd < 0) {
		std::perror("[gateway] socket");
		return -1;
	}
	addr.can_family = AF_CAN;
	addr.can_ifindex = static_cast<int>(idx);
	if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		std::perror("[gateway] bind");
		close(fd);
		return -1;
	}
	return fd;
}

// LIN master + BodyControl (only with --lin-iface)
struct BodyNode {
	std::mutex mtx;
	sil::lin::LinMaster master;
	sil::body::BodyControl control;
	std::map<std::uint64_t, std::shared_ptr<vsomeip::message>> pending;
	std::uint64_t next_token = 1;
	int fd = -1;
	std::uint64_t events_sent = 0, security_events = 0, requests = 0;

	// §7.3 allowlist: the AP BodyControl client (practice selection); 5 requests/s per method; 500 ms to confirm
	BodyNode(int wire, std::int64_t now)
		: master(now), control({0x2002}, 5, 500'000'000), fd(wire)
	{
	}
};

struct Gateway {
	std::mutex mtx;
	CanInputValidator validator;
	VehicleStateCache cache;
	std::atomic<bool> stop{false};
	std::atomic<std::uint64_t> events_sent{0};
	std::atomic<std::uint64_t> snapshot_requests{0};
	std::atomic<std::uint64_t> malformed_requests{0};
	std::atomic<std::uint64_t> frames_read{0};
	std::atomic<std::uint64_t> diag_frames{0};
	std::shared_ptr<vsomeip::application> app;
	std::unique_ptr<BodyNode> body;

	std::array<std::uint8_t, sil::services::VehicleStateData::kSize> payload_bytes(std::uint32_t seq)
	{
		std::lock_guard<std::mutex> lk(mtx);
		return sil::services::serialize(to_payload(cache.snapshot(now_ns()), seq));
	}
};

bool is_diag_id(canid_t id)
{
	if (id & (CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_ERR_FLAG))
		return false;
	for (unsigned i = 0; i < SIL_DIAG_ECU_COUNT; i++)
		if (id == sil_diag_ecus[i].request_id || id == sil_diag_ecus[i].response_id)
			return true;
	return false;
}

void can_rx_loop(Gateway &gw, int fd)
{
	pollfd pfd{fd, POLLIN, 0};

	while (!gw.stop) {
		if (poll(&pfd, 1, 100) <= 0)
			continue;
		can_frame fr{};
		ssize_t n = read(fd, &fr, sizeof(fr));
		if (n != static_cast<ssize_t>(sizeof(fr)))
			continue;
		const std::int64_t t = now_ns();
		if (is_diag_id(fr.can_id)) { // ISO-TP diagnostic traffic is handled by doip_gateway, not a PDU
			gw.diag_frames++;
			continue;
		}
		sil_pdu_frame_t out{};
		std::lock_guard<std::mutex> lk(gw.mtx);
		gw.cache.on_frame(gw.validator.validate(fr, out), out, t);
		gw.frames_read++;
	}
}

void notify_loop(Gateway &gw, std::int64_t end_ns)
{
	auto rt = vsomeip::runtime::get();
	const auto period = std::chrono::milliseconds(svc::kVehicleStateChangedCycleMs);
	auto next = Clock::now();
	std::uint32_t seq = 0;
	sil_heartbeat_t hb;

	sil_heartbeat_init(&hb);
	while (!gw.stop) {
		next += period;
		std::this_thread::sleep_until(next);
		if (end_ns && now_ns() >= end_ns)
			break;
		// heartbeat from the event loop: if publishing stalls, heartbeats stop too
		sil_heartbeat_tick(&hb, now_ns());
		auto bytes = gw.payload_bytes(seq++);
		auto pl = rt->create_payload();
		pl->set_data(bytes.data(), static_cast<vsomeip::length_t>(bytes.size()));
		gw.app->notify(svc::kServiceId, svc::kInstanceId, svc::kVehicleStateChangedEventId, pl, true);
		gw.events_sent++;
	}
	sil_heartbeat_close(&hb);
	gw.stop = true;
	gw.app->stop();
}

void on_get_snapshot(Gateway &gw, const std::shared_ptr<vsomeip::message> &req)
{
	auto rt = vsomeip::runtime::get();
	auto resp = rt->create_response(req);

	gw.snapshot_requests++;
	if (req->get_payload()->get_length() != svc::kGetSnapshotRequestSize) {
		// §7.2: non-empty GetSnapshot request is rejected
		gw.malformed_requests++;
		resp->set_message_type(vsomeip::message_type_e::MT_ERROR);
		resp->set_return_code(vsomeip::return_code_e::E_MALFORMED_MESSAGE);
		std::fprintf(stderr, "{\"security_event\": \"someip_malformed_request\", \"method\": \"0x%04X\", \"length\": %u}\n",
			     svc::kGetSnapshotMethodId, req->get_payload()->get_length());
	} else {
		auto bytes = gw.payload_bytes(0);
		auto pl = rt->create_payload();
		pl->set_data(bytes.data(), static_cast<vsomeip::length_t>(bytes.size()));
		resp->set_payload(pl);
	}
	gw.app->send(resp);
}

// §7.3 vehicle state check (practice selection): window commands need ignition ON from a valid, fresh 0x100
bool ignition_ok(Gateway &gw)
{
	std::lock_guard<std::mutex> lk(gw.mtx);
	const std::int64_t t = now_ns();
	const PduHealth &h = gw.cache.health(SIL_PDU_PT_ENGINESTATUS);
	return gw.cache.freshness(SIL_PDU_PT_ENGINESTATUS, t) == Freshness::Fresh && !gw.cache.pdu_degraded(SIL_PDU_PT_ENGINESTATUS, t) &&
	       h.last.flags.valid && h.last.raw[SIL_PT_ENGINESTATUS_IGNITION_ON] == 1;
}

void security_event(const std::string &what)
{
	std::fprintf(stderr, "{\"security_event\": \"%s\"}\n", what.c_str());
}

void send_body_result(Gateway &gw, const std::shared_ptr<vsomeip::message> &req, std::uint8_t code)
{
	auto resp = vsomeip::runtime::get()->create_response(req);
	auto pl = vsomeip::runtime::get()->create_payload();
	auto bytes = sil::services::serialize(sil::services::BodyControlResult{code});
	pl->set_data(bytes.data(), static_cast<vsomeip::length_t>(bytes.size()));
	resp->set_payload(pl);
	gw.app->send(resp);
}

void on_body_request(Gateway &gw, const std::shared_ptr<vsomeip::message> &req)
{
	const bool ignition = ignition_ok(gw); // gw.mtx before body->mtx, never the other way round
	auto pl = req->get_payload();
	std::vector<std::uint8_t> data(pl->get_data(), pl->get_data() + pl->get_length());
	sil::body::Decision d;
	{
		std::lock_guard<std::mutex> lk(gw.body->mtx);
		const std::uint64_t token = gw.body->next_token++;
		gw.body->requests++;
		d = gw.body->control.request(req->get_method(), req->get_client(), data, now_ns(), ignition, gw.body->master, token);
		if (d.pending)
			gw.body->pending[token] = req;
		if (!d.security_event.empty())
			gw.body->security_events++;
	}
	if (!d.security_event.empty())
		security_event(d.security_event);
	if (!d.pending)
		send_body_result(gw, req, d.code);
}

sil::services::BodyStatusData body_status(const BodyNode &b, std::uint32_t seq, std::int64_t t)
{
	using sil::lin::Slave;
	const auto &st = b.master.status();
	sil::services::BodyStatusData d;
	d.sequence = seq;
	d.timestamp_us = static_cast<std::uint64_t>(b.master.newest_status_ns() / 1000);
	d.door_bits = static_cast<std::uint8_t>(st.doors | (st.central_lock << 4) | (st.child_lock << 5));
	d.window_position = st.window_position;
	d.light_state = st.light_active;
	d.status_flags = static_cast<std::uint8_t>(st.obstruction | (st.window_fault << 1) | (st.light_fault << 2) |
						   (b.master.health(Slave::Door).degraded << 3) |
						   (b.master.health(Slave::Window).degraded << 4) |
						   (b.master.health(Slave::Light).degraded << 5) |
						   ((b.master.state() != sil::lin::BusState::Running) << 6) | ((st.window_motion != 0) << 7));
	d.freshness = static_cast<std::uint8_t>(b.master.fresh(Slave::Door, t) | (b.master.fresh(Slave::Window, t) << 1) |
						(b.master.fresh(Slave::Light, t) << 2));
	return d;
}

// LIN master: 1 ms loop drives the 10 ms schedule; BodyControl answers and the 100 ms BodyStatusChanged event
void lin_loop(Gateway &gw)
{
	auto rt = vsomeip::runtime::get();
	BodyNode &b = *gw.body;
	auto next = Clock::now();
	auto next_event = next;
	std::uint32_t seq = 0;

	while (!gw.stop) {
		next += std::chrono::milliseconds(1);
		std::this_thread::sleep_until(next);
		const std::int64_t t = now_ns();
		std::vector<std::pair<std::shared_ptr<vsomeip::message>, std::uint8_t>> answers;
		std::shared_ptr<vsomeip::payload> event;
		{
			std::lock_guard<std::mutex> lk(b.mtx);
			can_frame fr{};
			while (recv(b.fd, &fr, sizeof(fr), MSG_DONTWAIT) == static_cast<ssize_t>(sizeof(fr)))
				b.master.on_frame({fr.can_id, std::vector<std::uint8_t>(fr.data, fr.data + fr.len)}, t);
			for (const auto &w : b.master.tick(t)) {
				can_frame out{};
				out.can_id = w.can_id;
				out.len = static_cast<std::uint8_t>(w.data.size());
				std::memcpy(out.data, w.data.data(), w.data.size());
				if (write(b.fd, &out, sizeof(out)) != static_cast<ssize_t>(sizeof(out)))
					std::perror("[gateway] LIN write");
			}
			for (const auto &c : b.control.poll(t, b.master)) {
				auto it = b.pending.find(c.token);
				if (it != b.pending.end()) {
					answers.push_back({it->second, c.code});
					b.pending.erase(it);
				}
			}
			if (Clock::now() >= next_event) {
				next_event += std::chrono::milliseconds(bsvc::kBodyStatusChangedCycleMs);
				auto bytes = sil::services::serialize(body_status(b, seq++, t));
				event = rt->create_payload();
				event->set_data(bytes.data(), static_cast<vsomeip::length_t>(bytes.size()));
				b.events_sent++;
			}
		}
		for (auto &a : answers)
			send_body_result(gw, a.first, a.second);
		if (event)
			gw.app->notify(bsvc::kServiceId, bsvc::kInstanceId, bsvc::kBodyStatusChangedEventId, event, true);
	}
}

void print_report(Gateway &gw)
{
	std::lock_guard<std::mutex> lk(gw.mtx);
	const std::int64_t t = now_ns();

	std::printf("{\"component\": \"gateway\", \"frames_read\": %llu, \"diag_frames\": %llu, \"lost_frames\": %llu, \"unattributed_errors\": %llu,"
		    " \"events_sent\": %llu, \"snapshot_requests\": %llu, \"malformed_requests\": %llu,"
		    " \"final_state\": %u, \"pdus\": [",
		    static_cast<unsigned long long>(gw.frames_read), static_cast<unsigned long long>(gw.diag_frames),
		    static_cast<unsigned long long>(gw.validator.lost_frames()),
		    static_cast<unsigned long long>(gw.cache.unattributed_errors()),
		    static_cast<unsigned long long>(gw.events_sent), static_cast<unsigned long long>(gw.snapshot_requests),
		    static_cast<unsigned long long>(gw.malformed_requests), static_cast<unsigned>(gw.cache.state(t)));
	for (unsigned i = 0; i < SIL_PDU_COUNT; i++) {
		const PduHealth &h = gw.cache.health(i);
		std::printf("%s{\"id\": \"0x%03X\", \"name\": \"%s\", \"accepted\": %llu, \"rejected\": %llu}", i ? ", " : "",
			    sil_pdus[i].can_id, sil_pdus[i].name, static_cast<unsigned long long>(h.accepted),
			    static_cast<unsigned long long>(h.rejected));
	}
	std::printf("]");
	if (gw.body) {
		using sil::lin::Slave;
		std::lock_guard<std::mutex> bl(gw.body->mtx);
		const auto &m = gw.body->master;
		std::printf(", \"lin\": {\"sleeps\": %llu, \"slot_overruns\": %llu, \"unexpected_frames\": %llu,"
			    " \"requests\": %llu, \"security_events\": %llu, \"events_sent\": %llu",
			    static_cast<unsigned long long>(m.sleeps()), static_cast<unsigned long long>(m.slot_overruns()),
			    static_cast<unsigned long long>(m.unexpected_frames()), static_cast<unsigned long long>(gw.body->requests),
			    static_cast<unsigned long long>(gw.body->security_events), static_cast<unsigned long long>(gw.body->events_sent));
		const char *names[3] = {"door", "window", "light"};
		for (int i = 0; i < 3; i++) {
			const auto &h = m.health(static_cast<Slave>(i));
			std::printf(", \"%s\": {\"ok\": %llu, \"missed\": %llu, \"bad_checksum\": %llu, \"recoveries\": %llu, \"degraded\": %s}",
				    names[i], static_cast<unsigned long long>(h.ok_total), static_cast<unsigned long long>(h.missed_total),
				    static_cast<unsigned long long>(h.bad_checksum), static_cast<unsigned long long>(h.recoveries),
				    h.degraded ? "true" : "false");
		}
		std::printf("}");
	}
	std::printf("}\n");
	std::fflush(stdout);
}

} // namespace

int main(int argc, char **argv)
{
	const char *ifname = nullptr, *lin_ifname = nullptr;
	double duration_s = 0.0;

	bool bad_args = false;

	for (int i = 1; i < argc && !bad_args; i++) {
		if (std::strcmp(argv[i], "--iface") == 0 && i + 1 < argc)
			ifname = argv[++i];
		else if (std::strcmp(argv[i], "--duration-s") == 0 && i + 1 < argc)
			duration_s = std::strtod(argv[++i], nullptr);
		else if (std::strcmp(argv[i], "--lin-iface") == 0 && i + 1 < argc)
			lin_ifname = argv[++i];
		else
			bad_args = true;
	}
	if (bad_args || ifname == nullptr || duration_s < 0.0) {
		std::fprintf(stderr, "usage: %s --iface <can-if> [--lin-iface <lin-if>] [--duration-s <sec>]\n", argv[0]);
		return 2;
	}

	// SIGINT/SIGTERM are handled by a dedicated thread
	sigset_t sigs;
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGINT);
	sigaddset(&sigs, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

	int fd = open_can(ifname);
	if (fd < 0)
		return 1;

	Gateway gw;
	if (lin_ifname) {
		int lfd = open_can(lin_ifname);
		if (lfd < 0) {
			close(fd);
			return 1;
		}
		gw.body = std::make_unique<BodyNode>(lfd, now_ns());
	}
	gw.app = vsomeip::runtime::get()->create_application("vehicle_gateway");
	if (!gw.app->init()) {
		std::fprintf(stderr, "[gateway] vsomeip init failed (check VSOMEIP_CONFIGURATION)\n");
		close(fd);
		return 1;
	}
	gw.app->register_state_handler([&gw](vsomeip::state_type_e st) {
		if (st != vsomeip::state_type_e::ST_REGISTERED)
			return;
		std::set<vsomeip::eventgroup_t> groups{svc::kVehicleStateChangedEventgroupId};
		gw.app->offer_event(svc::kServiceId, svc::kInstanceId, svc::kVehicleStateChangedEventId, groups,
				    vsomeip::event_type_e::ET_EVENT, std::chrono::milliseconds::zero(), false, true, nullptr,
				    vsomeip::reliability_type_e::RT_UNRELIABLE);
		gw.app->offer_service(svc::kServiceId, svc::kInstanceId, svc::kMajorVersion, svc::kMinorVersion);
		std::fprintf(stderr, "[gateway] offering VehicleState 0x%04X.0x%04X\n", svc::kServiceId, svc::kInstanceId);
		if (gw.body) {
			std::set<vsomeip::eventgroup_t> bgroups{bsvc::kBodyStatusChangedEventgroupId};
			gw.app->offer_event(bsvc::kServiceId, bsvc::kInstanceId, bsvc::kBodyStatusChangedEventId, bgroups,
					    vsomeip::event_type_e::ET_EVENT, std::chrono::milliseconds::zero(), false, true, nullptr,
					    vsomeip::reliability_type_e::RT_UNRELIABLE);
			gw.app->offer_service(bsvc::kServiceId, bsvc::kInstanceId, bsvc::kMajorVersion, bsvc::kMinorVersion);
			std::fprintf(stderr, "[gateway] offering BodyControl 0x%04X.0x%04X\n", bsvc::kServiceId, bsvc::kInstanceId);
		}
	});
	gw.app->register_message_handler(svc::kServiceId, svc::kInstanceId, svc::kGetSnapshotMethodId,
					 [&gw](const std::shared_ptr<vsomeip::message> &m) { on_get_snapshot(gw, m); });

	const std::int64_t end_ns = duration_s > 0.0 ? now_ns() + static_cast<std::int64_t>(duration_s * 1e9) : 0;
	std::thread rx(can_rx_loop, std::ref(gw), fd);
	std::thread tx(notify_loop, std::ref(gw), end_ns);
	std::thread lin;
	if (gw.body) {
		for (auto m : {bsvc::kSetWindowPositionMethodId, bsvc::kSetInteriorLightMethodId})
			gw.app->register_message_handler(bsvc::kServiceId, bsvc::kInstanceId, m,
							 [&gw](const std::shared_ptr<vsomeip::message> &msg) { on_body_request(gw, msg); });
		lin = std::thread(lin_loop, std::ref(gw));
	}
	std::thread sig([&gw, sigs]() {
		int s = 0;
		while (!gw.stop) {
			timespec ts{0, 200000000};
			s = sigtimedwait(&sigs, nullptr, &ts);
			if (s == SIGINT || s == SIGTERM) {
				gw.stop = true;
				gw.app->stop();
			}
		}
	});

	std::fprintf(stderr, "[gateway] start iface=%s lin=%s\n", ifname, lin_ifname ? lin_ifname : "off");
	gw.app->start(); // returns after app->stop()

	gw.stop = true;
	tx.join();
	rx.join();
	if (lin.joinable())
		lin.join();
	sig.join();
	gw.cache.shutdown();
	close(fd);
	print_report(gw);
	if (gw.body)
		close(gw.body->fd);
	return 0;
}
