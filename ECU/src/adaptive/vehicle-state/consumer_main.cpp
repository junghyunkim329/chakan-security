// Adaptive Central Node: VehicleState consumer (project.md §7.2, §15.2).
// Discovers the service, subscribes to VehicleStateChanged, calls GetSnapshot, and reports
// discovery time and CAN-rx -> event-rx latency. Has no CAN access (runs in ns_ap / hj_ap).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <vsomeip/vsomeip.hpp>

#include "sil/heartbeat.h"
#include "sil_services_gen.hpp"

namespace svc = sil::services::VehicleState;
using sil::services::VehicleStateData;
using Clock = std::chrono::steady_clock; // same CLOCK_MONOTONIC as the Gateway on this VM

namespace {

std::int64_t now_us()
{
	return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}

struct Consumer {
	std::mutex mtx;
	std::shared_ptr<vsomeip::application> app;
	std::int64_t start_us = 0;
	std::int64_t warmup_end_us = 0;
	std::int64_t discovered_us = -1;
	std::vector<std::int64_t> latencies_us;
	std::uint64_t events = 0;
	std::uint64_t malformed_events = 0;
	std::uint64_t sequence_gaps = 0;
	std::uint64_t not_fresh_events = 0;
	std::uint64_t not_valid_events = 0;
	std::uint64_t running_events = 0;
	bool have_seq = false;
	std::uint32_t last_seq = 0;
	std::uint16_t rpm_min = 0xFFFF, rpm_max = 0, speed_max = 0;
	bool snapshot_ok = false;
	bool malformed_request_rejected = false;
	bool requested = false;
	VehicleStateData last{};
	std::atomic<bool> stop{false};
};

void send_get_snapshot(Consumer &c, std::size_t payload_len)
{
	auto rt = vsomeip::runtime::get();
	auto req = rt->create_request(true);
	std::vector<vsomeip::byte_t> data(payload_len, 0xAA);
	auto pl = rt->create_payload();

	req->set_service(svc::kServiceId);
	req->set_instance(svc::kInstanceId);
	req->set_method(svc::kGetSnapshotMethodId);
	req->set_interface_version(svc::kMajorVersion);
	pl->set_data(data);
	req->set_payload(pl);
	c.app->send(req);
}

void on_availability(Consumer &c, vsomeip::service_t, vsomeip::instance_t, bool available)
{
	std::lock_guard<std::mutex> lk(c.mtx);
	if (!available || c.requested)
		return;
	c.discovered_us = now_us() - c.start_us;
	c.requested = true;
	std::fprintf(stderr, "[consumer] VehicleState available after %.1f ms\n", c.discovered_us / 1000.0);
	send_get_snapshot(c, 0); // valid request
	send_get_snapshot(c, 1); // malformed: must be rejected by the Gateway
}

void on_event(Consumer &c, const std::shared_ptr<vsomeip::message> &m)
{
	const std::int64_t t = now_us();
	auto pl = m->get_payload();
	VehicleStateData d;
	std::lock_guard<std::mutex> lk(c.mtx);

	// §7.2: wrong version, length or reserved byte -> do not update state, record a security event
	if (m->get_interface_version() != svc::kMajorVersion || !sil::services::deserialize(pl->get_data(), pl->get_length(), d)) {
		c.malformed_events++;
		std::fprintf(stderr, "{\"security_event\": \"someip_malformed_event\", \"length\": %u}\n", pl->get_length());
		return;
	}
	c.last = d;
	c.events++;
	if (c.have_seq && d.sequence != c.last_seq + 1)
		c.sequence_gaps++;
	c.have_seq = true;
	c.last_seq = d.sequence;
	if (t < c.warmup_end_us)
		return;

	if (d.gateway_state == 1)
		c.running_events++;
	if (d.freshness != 0x3F)
		c.not_fresh_events++;
	if (d.validity != 0x3F)
		c.not_valid_events++;
	// invalid or stale values are not used as normal values (§7.2)
	if ((d.validity & 0x01) && (d.freshness & 0x01)) {
		c.rpm_min = std::min(c.rpm_min, d.rpm);
		c.rpm_max = std::max(c.rpm_max, d.rpm);
	}
	if ((d.validity & 0x02) && (d.freshness & 0x02))
		c.speed_max = std::max(c.speed_max, d.speed);
	if (d.timestamp_us != 0)
		c.latencies_us.push_back(t - static_cast<std::int64_t>(d.timestamp_us));
}

void on_response(Consumer &c, const std::shared_ptr<vsomeip::message> &m)
{
	auto pl = m->get_payload();
	VehicleStateData d;
	std::lock_guard<std::mutex> lk(c.mtx);

	if (m->get_message_type() == vsomeip::message_type_e::MT_RESPONSE && m->get_return_code() == vsomeip::return_code_e::E_OK
	    && sil::services::deserialize(pl->get_data(), pl->get_length(), d))
		c.snapshot_ok = true;
	else if (m->get_return_code() == vsomeip::return_code_e::E_MALFORMED_MESSAGE)
		c.malformed_request_rejected = true;
}

std::int64_t percentile(std::vector<std::int64_t> v, double p)
{
	if (v.empty())
		return -1;
	std::sort(v.begin(), v.end());
	std::size_t i = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1) + 0.5);
	return v[std::min(i, v.size() - 1)];
}

void print_report(Consumer &c)
{
	std::lock_guard<std::mutex> lk(c.mtx);

	std::printf("{\"component\": \"consumer\", \"discovery_ms\": %.1f, \"events\": %llu, \"measured\": %zu,"
		    " \"latency_p50_ms\": %.3f, \"latency_p99_ms\": %.3f, \"latency_max_ms\": %.3f,"
		    " \"sequence_gaps\": %llu, \"malformed_events\": %llu, \"not_fresh_events\": %llu, \"not_valid_events\": %llu,"
		    " \"running_events\": %llu, \"rpm_min\": %u, \"rpm_max\": %u, \"speed_max\": %u,"
		    " \"snapshot_ok\": %s, \"malformed_request_rejected\": %s}\n",
		    c.discovered_us < 0 ? -1.0 : c.discovered_us / 1000.0, static_cast<unsigned long long>(c.events),
		    c.latencies_us.size(), percentile(c.latencies_us, 0.50) / 1000.0, percentile(c.latencies_us, 0.99) / 1000.0,
		    percentile(c.latencies_us, 1.0) / 1000.0, static_cast<unsigned long long>(c.sequence_gaps),
		    static_cast<unsigned long long>(c.malformed_events), static_cast<unsigned long long>(c.not_fresh_events),
		    static_cast<unsigned long long>(c.not_valid_events), static_cast<unsigned long long>(c.running_events),
		    c.rpm_min == 0xFFFF ? 0u : c.rpm_min, c.rpm_max, c.speed_max, c.snapshot_ok ? "true" : "false",
		    c.malformed_request_rejected ? "true" : "false");
	std::fflush(stdout);
}

} // namespace

int main(int argc, char **argv)
{
	double duration_s = 30.0, warmup_s = 3.0;
	bool bad_args = false;

	for (int i = 1; i < argc && !bad_args; i++) {
		if (std::strcmp(argv[i], "--duration-s") == 0 && i + 1 < argc)
			duration_s = std::strtod(argv[++i], nullptr);
		else if (std::strcmp(argv[i], "--warmup-s") == 0 && i + 1 < argc)
			warmup_s = std::strtod(argv[++i], nullptr);
		else
			bad_args = true;
	}
	if (bad_args || duration_s < 0.0 || warmup_s < 0.0) {
		std::fprintf(stderr, "usage: %s [--duration-s <sec, 0 = until SIGTERM>] [--warmup-s <sec>]\n", argv[0]);
		return 2;
	}

	sigset_t sigs;
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGINT);
	sigaddset(&sigs, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

	Consumer c;
	c.start_us = now_us();
	c.warmup_end_us = c.start_us + static_cast<std::int64_t>(warmup_s * 1e6);
	c.app = vsomeip::runtime::get()->create_application("vehicle_state_consumer");
	if (!c.app->init()) {
		std::fprintf(stderr, "[consumer] vsomeip init failed (check VSOMEIP_CONFIGURATION)\n");
		return 1;
	}
	c.app->register_availability_handler(
		svc::kServiceId, svc::kInstanceId,
		[&c](vsomeip::service_t s, vsomeip::instance_t i, bool a) { on_availability(c, s, i, a); }, svc::kMajorVersion);
	c.app->register_message_handler(svc::kServiceId, svc::kInstanceId, svc::kVehicleStateChangedEventId,
					[&c](const std::shared_ptr<vsomeip::message> &m) { on_event(c, m); });
	c.app->register_message_handler(svc::kServiceId, svc::kInstanceId, svc::kGetSnapshotMethodId,
					[&c](const std::shared_ptr<vsomeip::message> &m) { on_response(c, m); });
	c.app->register_state_handler([&c](vsomeip::state_type_e st) {
		if (st != vsomeip::state_type_e::ST_REGISTERED)
			return;
		std::set<vsomeip::eventgroup_t> groups{svc::kVehicleStateChangedEventgroupId};
		c.app->request_service(svc::kServiceId, svc::kInstanceId, svc::kMajorVersion, svc::kMinorVersion);
		c.app->request_event(svc::kServiceId, svc::kInstanceId, svc::kVehicleStateChangedEventId, groups,
				     vsomeip::event_type_e::ET_EVENT, vsomeip::reliability_type_e::RT_UNRELIABLE);
		c.app->subscribe(svc::kServiceId, svc::kInstanceId, svc::kVehicleStateChangedEventgroupId, svc::kMajorVersion);
	});

	std::thread timer([&c, duration_s, sigs]() {
		const auto end = Clock::now() + std::chrono::duration<double>(duration_s);
		sil_heartbeat_t hb;

		sil_heartbeat_init(&hb);
		while (duration_s == 0.0 || Clock::now() < end) {
			timespec ts{0, 100000000};
			int s = sigtimedwait(&sigs, nullptr, &ts);
			if (s == SIGINT || s == SIGTERM)
				break;
			sil_heartbeat_tick(&hb, now_us() * 1000);
		}
		sil_heartbeat_close(&hb);
		c.stop = true;
		c.app->stop();
	});

	c.app->start(); // returns after app->stop()
	timer.join();
	print_report(c);
	return 0;
}
