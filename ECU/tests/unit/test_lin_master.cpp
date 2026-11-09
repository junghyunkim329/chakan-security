// LIN master + BodyControl tests with the real slave logic on a simulated bus (project.md §6, §7.3).
#include <cstring>
#include <vector>

extern "C" {
#include "lin_slave.h"
}
#include "body_control.hpp"
#include "lin_master.hpp"
#include "sil_test.h"

using namespace sil;

namespace {

constexpr std::int64_t MS = 1'000'000;

struct Bus {
	lin::LinMaster master;
	sil_lin_slave_t door{}, window{}, light{};
	std::int64_t now = 0;
	std::vector<std::pair<std::int64_t, std::uint8_t>> headers; // (time, pid)
	unsigned wakeups = 0;

	Bus() : master(0)
	{
		sil_lin_slave_init(&door, SIL_LIN_ROLE_DOOR, 0);
		sil_lin_slave_init(&window, SIL_LIN_ROLE_WINDOW, 0);
		sil_lin_slave_init(&light, SIL_LIN_ROLE_LIGHT, 0);
	}

	// one millisecond of bus time: master frames go to every slave, slave answers go to the master
	void step()
	{
		now += MS;
		for (const lin::WireFrame &f : master.tick(now)) {
			if (f.can_id == SIL_LIN_WAKEUP_CAN_ID) {
				wakeups++;
				for (auto *s : {&door, &window, &light})
					sil_lin_slave_on_wakeup(s, now);
			} else if (f.can_id == SIL_LIN_HEADER_CAN_ID) {
				headers.push_back({now, f.data[0]});
				for (auto *s : {&door, &window, &light}) {
					std::uint8_t out[8];
					std::size_t n = sil_lin_slave_on_header(s, f.data[0], now, out);
					if (n)
						master.on_frame({SIL_LIN_RESPONSE_CAN_ID, std::vector<std::uint8_t>(out, out + n)}, now);
				}
			} else {
				for (auto *s : {&door, &window, &light})
					sil_lin_slave_on_response(s, f.data.data(), f.data.size());
			}
		}
	}
	void run(std::int64_t ms)
	{
		for (std::int64_t i = 0; i < ms; i++)
			step();
	}
};

void test_schedule()
{
	Bus b;
	b.run(1000);
	static const std::uint8_t order[7] = {0x11, 0x92, 0xD3, 0x14, 0x50, 0x11, 0x92};
	CHECK(b.headers.size() >= 69 && b.headers.size() <= 71); // 7 headers per 100 ms
	for (std::size_t i = 0; i < b.headers.size(); i++)
		CHECK_EQ_INT(b.headers[i].second, order[i % 7]);
	// 10 ms between slots, 40 ms from the 60 ms slot to the next 0 ms slot (3 idle slots).
	// Measured from the second major cycle: the very first header is stamped at the first 1 ms step.
	CHECK_EQ_INT(b.headers[8].first - b.headers[7].first, 10 * MS);
	CHECK_EQ_INT(b.headers[14].first - b.headers[13].first, 40 * MS);
	CHECK_EQ_INT(b.headers[14].first - b.headers[7].first, 100 * MS);
	// initial state from the slaves
	CHECK(b.master.status().central_lock && b.master.status().door_valid && b.master.status().window_valid);
	for (auto s : {lin::Slave::Door, lin::Slave::Window, lin::Slave::Light})
		CHECK(!b.master.health(s).degraded && b.master.fresh(s, b.now));
}

void test_window_command_and_status()
{
	Bus b;
	b.run(200);
	b.master.command_window(50, b.now);
	b.run(700);
	CHECK_EQ_INT(b.window.position, 50);
	CHECK_EQ_INT(b.master.status().window_position, 50);
	CHECK(b.master.window_cmd_tx_ns() > 0 && b.master.status_ns(lin::Slave::Window) > b.master.window_cmd_tx_ns());
	b.master.command_light(true, b.now);
	b.run(1200);
	CHECK(b.light.brightness == 100 && b.master.status().light_active);
	sil_lin_slave_control(&b.door, "door RR 1");
	b.run(150);
	CHECK_EQ_INT(b.master.status().doors, 0x08);
}

void test_missing_responses_and_recovery()
{
	Bus b;
	b.run(300);
	sil_lin_slave_control(&b.window, "mute 1");
	b.run(160); // window answers twice per 100 ms
	CHECK(b.master.health(lin::Slave::Window).degraded);
	CHECK(!b.master.fresh(lin::Slave::Window, b.now));
	CHECK(!b.master.health(lin::Slave::Door).degraded);
	sil_lin_slave_control(&b.window, "mute 0");
	b.run(110); // two good responses are not enough
	CHECK(b.master.health(lin::Slave::Window).degraded);
	b.run(60);
	CHECK(!b.master.health(lin::Slave::Window).degraded);
	CHECK_EQ_INT(b.master.health(lin::Slave::Window).recoveries, 1);

	sil_lin_slave_control(&b.light, "corrupt 1"); // checksum errors count as missing responses
	b.run(310);
	CHECK(b.master.health(lin::Slave::Light).degraded && b.master.health(lin::Slave::Light).bad_checksum >= 3);
	sil_lin_slave_control(&b.light, "corrupt 0");
	b.run(310);
	CHECK(!b.master.health(lin::Slave::Light).degraded);
}

void test_sleep_and_wakeup()
{
	Bus b;
	b.run(5200);
	CHECK(b.master.state() == lin::BusState::Sleep);
	const std::size_t n = b.headers.size();
	b.run(1000);
	CHECK_EQ_INT(b.headers.size(), n); // silent bus
	b.master.command_window(20, b.now);
	const std::int64_t cmd = b.now;
	b.run(300);
	CHECK_EQ_INT(b.wakeups, 1);
	CHECK(b.headers.size() > n);
	CHECK_EQ_INT(b.headers[n].second, 0x11); // restart at the 0 ms slot
	CHECK(b.headers[n].first - cmd >= SIL_LIN_WAKEUP_DELAY_MS * MS);
	CHECK(b.master.state() == lin::BusState::Running);
	CHECK(b.window.target == 20);
	// a moving window and status changes keep it awake past 5 s of no commands only while things change
	b.run(5300);
	CHECK(b.master.state() == lin::BusState::Sleep);
}

void test_body_control_policy()
{
	Bus b;
	body::BodyControl bc({0x2002}, 5, 500 * MS);
	b.run(200);
	std::uint64_t token = 1;

	auto d = bc.request(body::kSetWindowPosition, 0x2FFF, {10}, b.now, true, b.master, token++);
	CHECK(!d.pending && d.code == body::kStateRejected && !d.security_event.empty());
	d = bc.request(body::kSetWindowPosition, 0x2002, {101}, b.now, true, b.master, token++);
	CHECK(!d.pending && d.code == body::kInvalidArgument);
	d = bc.request(body::kSetWindowPosition, 0x2002, {10, 0}, b.now, true, b.master, token++);
	CHECK(!d.pending && d.code == body::kInvalidArgument);
	d = bc.request(body::kSetInteriorLight, 0x2002, {2}, b.now, true, b.master, token++);
	CHECK(d.code == body::kInvalidArgument);
	d = bc.request(body::kSetWindowPosition, 0x2002, {10}, b.now, false, b.master, token++);
	CHECK(!d.pending && d.code == body::kStateRejected); // ignition off / unknown

	// accepted after the slave confirms the command
	const std::uint64_t t = token++;
	d = bc.request(body::kSetWindowPosition, 0x2002, {30}, b.now, true, b.master, t);
	CHECK(d.pending);
	std::vector<body::Completion> done;
	for (int i = 0; i < 200 && done.empty(); i++) {
		b.step();
		done = bc.poll(b.now, b.master);
	}
	CHECK(done.size() == 1 && done[0].token == t && done[0].code == body::kAccepted);

	// rate limit: 5 window requests per second (one used above, one rejected for ignition counts too)
	int limited = 0;
	for (int i = 0; i < 6; i++)
		limited += bc.request(body::kSetWindowPosition, 0x2002, {40}, b.now, true, b.master, token++).code == body::kStateRejected;
	CHECK(limited >= 1);
	b.run(1100);
	bc.poll(b.now, b.master);
	// the light method has its own budget
	CHECK(bc.request(body::kSetInteriorLight, 0x2002, {1}, b.now, true, b.master, token++).pending);

	// slave goes silent after the request: no confirmation -> LIN timeout
	b.run(1100);
	bc.poll(b.now, b.master);
	sil_lin_slave_control(&b.window, "mute 1");
	const std::uint64_t lost = token++;
	CHECK(bc.request(body::kSetWindowPosition, 0x2002, {60}, b.now, true, b.master, lost).pending);
	done.clear();
	for (int i = 0; i < 600 && done.empty(); i++) {
		b.step();
		done = bc.poll(b.now, b.master);
	}
	CHECK(done.size() == 1 && done[0].code == body::kLinTimeout);
	// once in communication error the request is refused at once
	d = bc.request(body::kSetWindowPosition, 0x2002, {60}, b.now, true, b.master, token++);
	CHECK(!d.pending && d.code == body::kLinTimeout);
}

} // namespace

int main()
{
	test_schedule();
	test_window_command_and_status();
	test_missing_responses_and_recovery();
	test_sleep_and_wakeup();
	test_body_control_policy();
	return TEST_REPORT();
}
