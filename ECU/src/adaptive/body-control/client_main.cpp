// AP BodyControl client for tests (project.md §7.3): reads commands on stdin, prints JSON lines on stdout.
//   window <0..255>      SetWindowPosition with one byte
//   light <0..255>       SetInteriorLight with one byte
//   raw <method> <hex>   any payload (malformed-request tests)
//   quit
// Output: {"type": "available"}, {"type": "response", ...}, {"type": "event", ...}. No CAN or LIN access.
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <vsomeip/vsomeip.hpp>

#include "sil_services_gen.hpp"

namespace bsvc = sil::services::BodyControl;
using Clock = std::chrono::steady_clock;

namespace {

std::mutex g_out;
std::map<vsomeip::session_t, std::pair<std::uint16_t, std::int64_t>> g_sent; // session -> (method, t_us)

std::int64_t now_us()
{
	return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}

void emit(const std::string &line)
{
	std::lock_guard<std::mutex> lk(g_out);
	std::cout << line << std::endl;
}

void send(const std::shared_ptr<vsomeip::application> &app, std::uint16_t method, const std::vector<vsomeip::byte_t> &data)
{
	auto rt = vsomeip::runtime::get();
	auto req = rt->create_request(true);
	req->set_service(bsvc::kServiceId);
	req->set_instance(bsvc::kInstanceId);
	req->set_method(method);
	req->set_interface_version(bsvc::kMajorVersion);
	auto pl = rt->create_payload();
	pl->set_data(data);
	req->set_payload(pl);
	app->send(req);
	std::lock_guard<std::mutex> lk(g_out);
	g_sent[req->get_session()] = {method, now_us()};
}

} // namespace

int main()
{
	sigset_t sigs;
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGINT);
	sigaddset(&sigs, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

	auto app = vsomeip::runtime::get()->create_application("body_control_client");
	if (!app->init()) {
		std::fprintf(stderr, "[body_client] vsomeip init failed (check VSOMEIP_CONFIGURATION)\n");
		return 1;
	}
	app->register_availability_handler(
		bsvc::kServiceId, bsvc::kInstanceId,
		[](vsomeip::service_t, vsomeip::instance_t, bool a) {
			if (a)
				emit("{\"type\": \"available\", \"t_us\": " + std::to_string(now_us()) + "}");
		},
		bsvc::kMajorVersion);
	for (auto m : {bsvc::kSetWindowPositionMethodId, bsvc::kSetInteriorLightMethodId, std::uint16_t(0x0042)})
		app->register_message_handler(bsvc::kServiceId, bsvc::kInstanceId, m, [](const std::shared_ptr<vsomeip::message> &msg) {
			const std::int64_t t = now_us();
			sil::services::BodyControlResult r;
			auto pl = msg->get_payload();
			std::int64_t sent = t;
			{
				std::lock_guard<std::mutex> lk(g_out);
				auto it = g_sent.find(msg->get_session());
				if (it != g_sent.end()) {
					sent = it->second.second;
					g_sent.erase(it);
				}
			}
			std::ostringstream o;
			o << "{\"type\": \"response\", \"method\": " << msg->get_method() << ", \"return_code\": "
			  << static_cast<int>(msg->get_return_code()) << ", \"code\": "
			  << (sil::services::deserialize(pl->get_data(), pl->get_length(), r) ? std::to_string(r.result) : std::string("null"))
			  << ", \"rtt_ms\": " << (t - sent) / 1000.0 << ", \"t_us\": " << t << "}";
			emit(o.str());
		});
	app->register_message_handler(bsvc::kServiceId, bsvc::kInstanceId, bsvc::kBodyStatusChangedEventId,
				      [](const std::shared_ptr<vsomeip::message> &msg) {
					      sil::services::BodyStatusData d;
					      auto pl = msg->get_payload();
					      std::ostringstream o;
					      if (!sil::services::deserialize(pl->get_data(), pl->get_length(), d)) {
						      emit("{\"type\": \"malformed_event\", \"length\": " + std::to_string(pl->get_length()) + "}");
						      return;
					      }
					      o << "{\"type\": \"event\", \"seq\": " << d.sequence << ", \"t_us\": " << now_us()
						<< ", \"door_bits\": " << int(d.door_bits) << ", \"window\": " << int(d.window_position)
						<< ", \"light\": " << int(d.light_state) << ", \"flags\": " << int(d.status_flags)
						<< ", \"fresh\": " << int(d.freshness) << "}";
					      emit(o.str());
				      });
	app->register_state_handler([&app](vsomeip::state_type_e st) {
		if (st != vsomeip::state_type_e::ST_REGISTERED)
			return;
		std::set<vsomeip::eventgroup_t> groups{bsvc::kBodyStatusChangedEventgroupId};
		app->request_service(bsvc::kServiceId, bsvc::kInstanceId, bsvc::kMajorVersion, bsvc::kMinorVersion);
		app->request_event(bsvc::kServiceId, bsvc::kInstanceId, bsvc::kBodyStatusChangedEventId, groups,
				   vsomeip::event_type_e::ET_EVENT, vsomeip::reliability_type_e::RT_UNRELIABLE);
		app->subscribe(bsvc::kServiceId, bsvc::kInstanceId, bsvc::kBodyStatusChangedEventgroupId, bsvc::kMajorVersion);
	});

	std::thread input([&app]() {
		std::string line;
		while (std::getline(std::cin, line)) {
			std::istringstream in(line);
			std::string cmd;
			in >> cmd;
			if (cmd == "quit")
				break;
			if (cmd == "window" || cmd == "light") {
				int v = 0;
				in >> v;
				send(app, cmd == "window" ? bsvc::kSetWindowPositionMethodId : bsvc::kSetInteriorLightMethodId,
				     {static_cast<vsomeip::byte_t>(v)});
			} else if (cmd == "raw") {
				unsigned method = 0;
				std::string hex;
				in >> std::hex >> method >> hex;
				std::vector<vsomeip::byte_t> data;
				for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
					data.push_back(static_cast<vsomeip::byte_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
				send(app, static_cast<std::uint16_t>(method), data);
			}
		}
		app->stop();
	});
	std::thread sig([&app, sigs]() {
		timespec ts{0, 200000000};
		for (;;) {
			int s = sigtimedwait(&sigs, nullptr, &ts);
			if (s == SIGINT || s == SIGTERM) {
				app->stop();
				return;
			}
		}
	});
	sig.detach();
	app->start();
	input.detach(); // getline may still block on stdin; the process exits anyway
	return 0;
}
