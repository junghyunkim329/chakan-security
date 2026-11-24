// LIN master + BodyControl tests with the real slave logic on a simulated bus (project.md §6, §7.3).
#include <cstring>
#include <functional>
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
	// rewrites a slave response payload on the wire; the checksum is recomputed, so only the meaning is wrong
	std::function<void(std::uint8_t pid, std::uint8_t *payload)> tamper;

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
					if (n && tamper) {
						tamper(f.data[0], out);
						out[n - 1] = sil_lin_checksum(f.data[0], out, n - 1);
					}
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
	body::BodyControl bc(5, 500 * MS);
	b.run(200);
	std::uint64_t token = 1;

	auto d = bc.request(body::kSetWindowPosition, false, {10}, b.now, true, b.master, token++);
	CHECK(!d.pending && d.code == body::kStateRejected && !d.security_event.empty());
	d = bc.request(body::kSetWindowPosition, true, {101}, b.now, true, b.master, token++);
	CHECK(!d.pending && d.code == body::kInvalidArgument);
	d = bc.request(body::kSetWindowPosition, true, {10, 0}, b.now, true, b.master, token++);
	CHECK(!d.pending && d.code == body::kInvalidArgument);
	d = bc.request(body::kSetInteriorLight, true, {2}, b.now, true, b.master, token++);
	CHECK(d.code == body::kInvalidArgument);
	d = bc.request(body::kSetWindowPosition, true, {10}, b.now, false, b.master, token++);
	CHECK(!d.pending && d.code == body::kStateRejected); // ignition off / unknown

	// accepted after the slave confirms the command
	const std::uint64_t t = token++;
	d = bc.request(body::kSetWindowPosition, true, {30}, b.now, true, b.master, t);
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
		limited += bc.request(body::kSetWindowPosition, true, {40}, b.now, true, b.master, token++).code == body::kStateRejected;
	CHECK(limited >= 1);
	b.run(1100);
	bc.poll(b.now, b.master);
	// the light method has its own budget
	CHECK(bc.request(body::kSetInteriorLight, true, {1}, b.now, true, b.master, token++).pending);

	// slave goes silent after the request: no confirmation -> LIN timeout
	b.run(1100);
	bc.poll(b.now, b.master);
	sil_lin_slave_control(&b.window, "mute 1");
	const std::uint64_t lost = token++;
	CHECK(bc.request(body::kSetWindowPosition, true, {60}, b.now, true, b.master, lost).pending);
	done.clear();
	for (int i = 0; i < 600 && done.empty(); i++) {
		b.step();
		done = bc.poll(b.now, b.master);
	}
	CHECK(done.size() == 1 && done[0].code == body::kLinTimeout);
	// once in communication error the request is refused at once
	d = bc.request(body::kSetWindowPosition, true, {60}, b.now, true, b.master, token++);
	CHECK(!d.pending && d.code == body::kLinTimeout);
}

// #2: checksum-valid responses with a wrong meaning never update status, health, freshness or recovery
void test_semantic_errors()
{
	struct Case {
		const char *what;
		lin::Slave slave;
		std::uint8_t pid;
		unsigned byte;
		std::uint8_t set, clear; // payload[byte] = (payload[byte] & ~clear) | set
		std::uint64_t lin::SlaveHealth::*counter;
	};
	const Case cases[] = {
		{"window position 101", lin::Slave::Window, SIL_LIN_BODY_WINDOWSTATUS_PID, 0, 101, 0xFF, &lin::SlaveHealth::sem_range},
		{"window motion enum 3", lin::Slave::Window, SIL_LIN_BODY_WINDOWSTATUS_PID, 1, 3, 0xFF, &lin::SlaveHealth::sem_range},
		{"window reserved bit", lin::Slave::Window, SIL_LIN_BODY_WINDOWSTATUS_PID, 2, 0x04, 0, &lin::SlaveHealth::sem_reserved},
		{"window valid 0", lin::Slave::Window, SIL_LIN_BODY_WINDOWSTATUS_PID, 2, 0, 0x80, &lin::SlaveHealth::sem_not_valid},
		{"door reserved bit", lin::Slave::Door, SIL_LIN_BODY_DOORSTATUS_PID, 0, 0x10, 0, &lin::SlaveHealth::sem_reserved},
		{"door valid 0", lin::Slave::Door, SIL_LIN_BODY_DOORSTATUS_PID, 1, 0, 0x80, &lin::SlaveHealth::sem_not_valid},
		{"light brightness 101", lin::Slave::Light, SIL_LIN_BODY_LIGHTSTATUS_PID, 0, 101, 0xFF, &lin::SlaveHealth::sem_range},
		{"light reserved bit", lin::Slave::Light, SIL_LIN_BODY_LIGHTSTATUS_PID, 1, 0x04, 0, &lin::SlaveHealth::sem_reserved},
		{"light valid 0", lin::Slave::Light, SIL_LIN_BODY_LIGHTSTATUS_PID, 1, 0, 0x80, &lin::SlaveHealth::sem_not_valid},
	};
	for (const Case &c : cases) {
		Bus b;
		b.run(300);
		const lin::BodyStatus before = b.master.status();
		const std::int64_t last_ok = b.master.status_ns(c.slave);
		const std::uint64_t ok_total = b.master.health(c.slave).ok_total;
		b.tamper = [&c](std::uint8_t pid, std::uint8_t *p) {
			if (pid == c.pid)
				p[c.byte] = static_cast<std::uint8_t>((p[c.byte] & ~c.clear) | c.set);
		};
		b.run(400);
		const lin::SlaveHealth &h = b.master.health(c.slave);
		if (!(h.*c.counter >= 3 && h.semantic_errors() == h.*c.counter))
			std::printf("case: %s\n", c.what);
		CHECK(h.*c.counter >= 3 && h.semantic_errors() == h.*c.counter);
		CHECK(b.master.status() == before);          // nothing published from a wrong response
		CHECK_EQ_INT(b.master.status_ns(c.slave), last_ok); // freshness timestamp untouched
		CHECK_EQ_INT(h.ok_total, ok_total);
		CHECK(!b.master.fresh(c.slave, b.now));       // the status goes stale
		CHECK(!h.degraded && h.missed == 0);          // the slave did answer: not a missing response
		CHECK_EQ_INT(h.bad_checksum, 0);
	}

	// a wrong response never completes a BodyControl command
	{
		Bus b;
		body::BodyControl bc({0x2002}, 5, 500 * MS);
		b.run(300);
		b.tamper = [](std::uint8_t pid, std::uint8_t *p) {
			if (pid == SIL_LIN_BODY_WINDOWSTATUS_PID)
				p[2] = static_cast<std::uint8_t>(p[2] & ~0x80); // valid 0
		};
		CHECK(bc.request(body::kSetWindowPosition, 0x2002, {30}, b.now, true, b.master, 1).pending);
		std::vector<body::Completion> done;
		for (int i = 0; i < 700 && done.empty(); i++) {
			b.step();
			done = bc.poll(b.now, b.master);
		}
		CHECK(done.size() == 1 && done[0].code == body::kLinTimeout);
	}

	// a wrong response is not used to recover a degraded slave
	{
		Bus b;
		b.run(300);
		sil_lin_slave_control(&b.window, "mute 1");
		b.run(160);
		CHECK(b.master.health(lin::Slave::Window).degraded);
		sil_lin_slave_control(&b.window, "mute 0");
		b.tamper = [](std::uint8_t pid, std::uint8_t *p) {
			if (pid == SIL_LIN_BODY_WINDOWSTATUS_PID)
				p[0] = 200;
		};
		b.run(500);
		CHECK(b.master.health(lin::Slave::Window).degraded);
		CHECK_EQ_INT(b.master.health(lin::Slave::Window).recoveries, 0);
		b.tamper = nullptr;
		b.run(170);
		CHECK(!b.master.health(lin::Slave::Window).degraded);
	}
}

// #3: one LIN command frame never confirms two requests; a replaced request is answered kStateRejected
void test_body_control_latest_wins()
{
	auto collect = [](Bus &b, body::BodyControl &bc, std::size_t want) {
		std::vector<body::Completion> all;
		for (int i = 0; i < 700 && all.size() < want; i++) {
			b.step();
			for (const auto &c : bc.poll(b.now, b.master))
				all.push_back(c);
		}
		return all;
	};
	auto code_of = [](const std::vector<body::Completion> &v, std::uint64_t token) {
		for (const auto &c : v)
			if (c.token == token)
				return static_cast<int>(c.code);
		return -1;
	};

	// two window targets before one command slot: only the second reaches the slave
	{
		Bus b;
		body::BodyControl bc({0x2002}, 5, 500 * MS);
		b.run(205); // just after a command slot, so both requests wait for the same next slot
		CHECK(bc.request(body::kSetWindowPosition, 0x2002, {30}, b.now, true, b.master, 1).pending);
		CHECK(bc.request(body::kSetWindowPosition, 0x2002, {70}, b.now, true, b.master, 2).pending);
		const auto done = collect(b, bc, 2);
		CHECK_EQ_INT(code_of(done, 1), body::kStateRejected);
		CHECK_EQ_INT(code_of(done, 2), body::kAccepted);
		CHECK_EQ_INT(bc.rejected(body::kStateRejected), 1);
		b.run(1000);
		CHECK_EQ_INT(b.window.target, 70);
	}

	// the same for InteriorLight
	{
		Bus b;
		body::BodyControl bc({0x2002}, 5, 500 * MS);
		b.run(225);
		CHECK(bc.request(body::kSetInteriorLight, 0x2002, {1}, b.now, true, b.master, 1).pending);
		CHECK(bc.request(body::kSetInteriorLight, 0x2002, {0}, b.now, true, b.master, 2).pending);
		const auto done = collect(b, bc, 2);
		CHECK_EQ_INT(code_of(done, 1), body::kStateRejected);
		CHECK_EQ_INT(code_of(done, 2), body::kAccepted);
		CHECK_EQ_INT(b.light.target, 0);
	}

	// a burst over several slots: each request is either confirmed by its own frame or replaced, never both
	{
		Bus b;
		body::BodyControl bc({0x2002}, 5, 500 * MS);
		b.run(205);
		const std::uint8_t targets[4] = {10, 20, 30, 40};
		std::vector<body::Completion> all;
		for (std::uint64_t i = 0; i < 4; i++) {
			CHECK(bc.request(body::kSetWindowPosition, 0x2002, {targets[i]}, b.now, true, b.master, i + 1).pending);
			for (int k = 0; k < 30; k++) { // 30 ms: sometimes a command slot passes, sometimes not
				b.step();
				for (const auto &c : bc.poll(b.now, b.master))
					all.push_back(c);
			}
		}
		for (const auto &c : collect(b, bc, 4 - all.size()))
			all.push_back(c);
		CHECK_EQ_INT(all.size(), 4);
		int accepted = 0;
		for (const auto &c : all) {
			CHECK(c.code == body::kAccepted || c.code == body::kStateRejected);
			accepted += c.code == body::kAccepted;
		}
		CHECK(accepted >= 1);
		CHECK_EQ_INT(code_of(all, 4), body::kAccepted); // the latest request always wins
		b.run(1000);
		CHECK_EQ_INT(b.window.target, 40);
	}

	// requests of different methods do not replace each other
	{
		Bus b;
		body::BodyControl bc({0x2002}, 5, 500 * MS);
		b.run(205);
		CHECK(bc.request(body::kSetWindowPosition, 0x2002, {50}, b.now, true, b.master, 1).pending);
		CHECK(bc.request(body::kSetInteriorLight, 0x2002, {1}, b.now, true, b.master, 2).pending);
		const auto done = collect(b, bc, 2);
		CHECK_EQ_INT(code_of(done, 1), body::kAccepted);
		CHECK_EQ_INT(code_of(done, 2), body::kAccepted);
	}
}

// generated schema check on its own (sil_lin_check_semantics)
void test_semantic_check_table()
{
	const sil_lin_frame_t *ws = sil_lin_frame(SIL_LIN_BODY_WINDOWSTATUS_ID);
	std::uint8_t p[3] = {100, 2, 0x80};
	CHECK(sil_lin_check_semantics(ws, p) == SIL_LIN_SEM_OK);
	p[2] = 0x83; // obstruction + fault + valid: all defined bits
	CHECK(sil_lin_check_semantics(ws, p) == SIL_LIN_SEM_OK);
	p[0] = 101;
	CHECK(sil_lin_check_semantics(ws, p) == SIL_LIN_SEM_RANGE);
	p[0] = 0;
	p[2] = 0x00;
	CHECK(sil_lin_check_semantics(ws, p) == SIL_LIN_SEM_NOT_VALID);
	p[2] = 0x88;
	CHECK(sil_lin_check_semantics(ws, p) == SIL_LIN_SEM_RESERVED);
	const sil_lin_frame_t *wc = sil_lin_frame(SIL_LIN_BODY_WINDOWCOMMAND_ID); // command frame: no valid flag
	std::uint8_t c[2] = {100, 0x03};
	CHECK(sil_lin_check_semantics(wc, c) == SIL_LIN_SEM_OK);
	c[1] = 0x04;
	CHECK(sil_lin_check_semantics(wc, c) == SIL_LIN_SEM_RESERVED);
}

} // namespace

int main()
{
	test_schedule();
	test_window_command_and_status();
	test_missing_responses_and_recovery();
	test_sleep_and_wakeup();
	test_body_control_policy();
	test_semantic_errors();
	test_semantic_check_table();
	test_body_control_latest_wins();
	return TEST_REPORT();
}
