// Execution Manager decision logic: start order, heartbeat, restart budget, state transitions (project.md §8).
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include "sil_test.h"
#include "supervisor.hpp"

using namespace sil::em;

namespace {

constexpr std::int64_t MS = 1'000'000;
constexpr std::int64_t S = 1'000 * MS;

struct Harness {
	std::vector<AuditEvent> log;
	Supervisor sup;
	int next_pid = 1000;
	std::int64_t now = 0;

	Harness()
		: sup({"engine", "brake", "steering", "gateway", "consumer"},
		      {{}, {}, {}, {"engine", "brake", "steering"}, {"gateway"}}, Policy{},
		      [this](const AuditEvent &e) { log.push_back(e); })
	{
	}

	// apply Start actions like the real Execution Manager would; returns the actions
	std::vector<Action> step(std::int64_t t)
	{
		now = t;
		auto acts = sup.tick(now);
		for (auto &a : acts)
			if (a.kind == Action::Kind::Start)
				sup.on_started(a.proc, next_pid++, now);
		return acts;
	}
	std::size_t idx(const std::string &n) const
	{
		for (std::size_t i = 0; i < sup.size(); i++)
			if (sup.name(i) == n)
				return i;
		return 99;
	}
	void hb(const std::string &n) { sup.on_heartbeat(sup.pid(idx(n)), n, now); }
	void hb_all()
	{
		for (std::size_t i = 0; i < sup.size(); i++)
			if (sup.pid(i))
				sup.on_heartbeat(sup.pid(i), sup.name(i), now);
	}
	// bring everything to Running, heartbeating every 100 ms
	void boot()
	{
		for (int k = 0; k < 30; k++) {
			step(now + 100 * MS);
			hb_all();
		}
	}
	bool has(const std::string &type, const std::string &proc) const
	{
		return std::any_of(log.begin(), log.end(), [&](const AuditEvent &e) { return e.type == type && e.process == proc; });
	}
};

bool is_kill_of(const std::vector<Action> &acts, std::size_t i)
{
	return std::any_of(acts.begin(), acts.end(), [&](const Action &a) { return a.kind == Action::Kind::Kill && a.proc == i; });
}

// CAN ECUs start first, Gateway only after all three are ready, consumer after Gateway
void test_start_order()
{
	Harness h;
	auto a = h.step(0);
	CHECK_EQ_INT(a.size(), 3);
	CHECK(h.sup.proc_state(h.idx("gateway")) == ProcState::NotStarted);
	CHECK(h.sup.state() == MachineState::Startup);

	h.hb("engine");
	h.hb("brake");
	CHECK(h.step(10 * MS).empty()); // steering not ready yet
	h.hb("steering");
	a = h.step(20 * MS);
	CHECK_EQ_INT(a.size(), 1);
	CHECK_EQ_INT(a[0].proc, h.idx("gateway"));
	CHECK(h.step(30 * MS).empty());
	h.hb("gateway");
	a = h.step(40 * MS);
	CHECK_EQ_INT(a.size(), 1);
	CHECK_EQ_INT(a[0].proc, h.idx("consumer"));
	h.hb("consumer");
	CHECK(h.sup.state() == MachineState::Running);
}

// process exits -> Degraded at once -> restart attempt within 3 s -> Running after first heartbeat
void test_exit_and_restart()
{
	Harness h;
	h.boot();
	const std::size_t brk = h.idx("brake");
	const int old_pid = h.sup.pid(brk);
	const std::int64_t t_fail = h.now + 10 * MS;

	h.now = t_fail;
	h.sup.on_exit(old_pid, "killed by signal 9", t_fail);
	CHECK(h.sup.state() == MachineState::Degraded);
	CHECK(h.sup.proc_state(brk) == ProcState::Restarting);

	std::int64_t restarted_at = -1;
	for (std::int64_t t = t_fail; t <= t_fail + 3 * S && restarted_at < 0; t += 50 * MS) {
		h.step(t);
		if (h.sup.pid(brk) != 0)
			restarted_at = t;
	}
	CHECK(restarted_at >= 0 && restarted_at - t_fail <= 3 * S);
	CHECK(h.sup.pid(brk) != old_pid);
	CHECK_EQ_INT(h.sup.restarts(brk), 1);
	h.hb_all();
	CHECK(h.sup.state() == MachineState::Running);
}

// process stops responding (alive, no heartbeat) -> failure at 3 missed heartbeats -> killed -> restarted
void test_hang_detected_by_heartbeat()
{
	Harness h;
	h.boot();
	const std::size_t st = h.idx("steering");
	const std::int64_t last_hb = h.now;

	std::int64_t detected = -1;
	for (std::int64_t t = last_hb + 100 * MS; t <= last_hb + 3 * S && detected < 0; t += 100 * MS) {
		h.now = t;
		auto acts = h.sup.tick(t);
		for (std::size_t i = 0; i < h.sup.size(); i++)
			if (i != st && h.sup.pid(i))
				h.sup.on_heartbeat(h.sup.pid(i), h.sup.name(i), t);
		if (is_kill_of(acts, st))
			detected = t;
	}
	// §8.2: 3 x 500 ms = 1.5 s (one tick of 100 ms resolution)
	CHECK(detected - last_hb >= 1500 * MS && detected - last_hb <= 1600 * MS);
	CHECK(h.sup.state() == MachineState::Degraded);
	CHECK(h.has("failure", "steering"));

	// not restarted while the hung process still exists
	const int hung_pid = h.sup.pid(st);
	h.step(detected + 1 * S);
	CHECK_EQ_INT(h.sup.pid(st), hung_pid);
	h.sup.on_exit(hung_pid, "killed by signal 9", detected + 1 * S);
	h.step(detected + 1500 * MS);
	CHECK(h.sup.pid(st) != 0 && h.sup.pid(st) != hung_pid);
}

// 3 failures within 60 s -> no more restarts, stays Degraded
void test_restart_budget()
{
	Harness h;
	h.boot();
	const std::size_t eng = h.idx("engine");

	for (int k = 1; k <= 3; k++) {
		h.now += 2 * S;
		h.hb_all(); // the other processes kept heartbeating during the time jump
		h.sup.on_exit(h.sup.pid(eng), "exit status 1", h.now);
		for (int j = 0; j < 20; j++) {
			h.step(h.now + 100 * MS);
			h.hb_all();
		}
	}
	CHECK(h.sup.proc_state(eng) == ProcState::Failed);
	CHECK_EQ_INT(h.sup.restarts(h.idx("brake")), 0);
	CHECK_EQ_INT(h.sup.pid(eng), 0);
	CHECK_EQ_INT(h.sup.restarts(eng), 2);
	CHECK(h.has("budget_exhausted", "engine"));
	for (int j = 0; j < 50; j++)
		h.step(h.now + 1 * S);
	CHECK_EQ_INT(h.sup.pid(eng), 0);
	CHECK(h.sup.state() == MachineState::Degraded);
}

// failures spread over more than 60 s do not exhaust the budget
void test_budget_window()
{
	Harness h;
	h.boot();
	const std::size_t eng = h.idx("engine");

	for (int k = 0; k < 5; k++) {
		h.now += 31 * S;
		h.hb_all(); // the other processes kept heartbeating during the time jump
		h.sup.on_exit(h.sup.pid(eng), "exit status 1", h.now);
		for (int j = 0; j < 20; j++) {
			h.step(h.now + 100 * MS);
			h.hb_all();
		}
		CHECK(h.sup.proc_state(eng) == ProcState::Running);
	}
	CHECK_EQ_INT(h.sup.restarts(eng), 5);
	CHECK(h.sup.state() == MachineState::Running);
}

// no first heartbeat within 5 s of start -> startup failure
void test_startup_timeout()
{
	Harness h;
	h.step(0);
	h.hb("engine");
	h.hb("brake");
	std::vector<Action> acts;
	for (std::int64_t t = 100 * MS; t <= 5 * S; t += 100 * MS) {
		h.now = t;
		h.hb("engine");
		h.hb("brake");
		auto a = h.sup.tick(t);
		acts.insert(acts.end(), a.begin(), a.end());
	}
	CHECK(is_kill_of(acts, h.idx("steering")));
	CHECK(h.sup.state() == MachineState::Degraded);
}

// heartbeats are accepted only from the supervised pid with the matching name
void test_spoofed_heartbeat_ignored()
{
	Harness h;
	h.boot();
	CHECK(!h.sup.on_heartbeat(424242, "engine", h.now));
	CHECK(!h.sup.on_heartbeat(h.sup.pid(h.idx("brake")), "engine", h.now));
	CHECK(h.has("security", "engine"));
	CHECK(h.sup.on_heartbeat(h.sup.pid(h.idx("engine")), "engine", h.now));
}

// shutdown: consumer, gateway, then ECUs (reverse order), SIGKILL after the stop timeout
void test_shutdown_reverse_order()
{
	Harness h;
	h.boot();
	std::vector<std::string> stopped;
	h.sup.begin_shutdown(h.now);
	CHECK(h.sup.state() == MachineState::Shutdown);

	for (int k = 0; k < 20 && !h.sup.shutdown_complete(); k++) {
		h.now += 100 * MS;
		auto acts = h.sup.tick(h.now);
		CHECK(acts.size() <= 1);
		for (auto &a : acts) {
			CHECK(a.kind == Action::Kind::Terminate);
			stopped.push_back(h.sup.name(a.proc));
			h.sup.on_exit(h.sup.pid(a.proc), "exit status 0", h.now);
		}
	}
	CHECK(h.sup.shutdown_complete());
	CHECK(stopped == (std::vector<std::string>{"consumer", "gateway", "steering", "brake", "engine"}));

	Harness k;
	k.boot();
	k.sup.begin_shutdown(k.now);
	auto a = k.sup.tick(k.now);
	CHECK(a.size() == 1 && a[0].kind == Action::Kind::Terminate);
	CHECK(k.sup.tick(k.now + 1 * S).empty());
	a = k.sup.tick(k.now + 2 * S);
	CHECK(a.size() == 1 && a[0].kind == Action::Kind::Kill && a[0].proc == k.idx("consumer"));
}

void test_manifest_errors()
{
	bool threw = false;
	try {
		Supervisor s({"a", "b"}, {{"b"}, {"a"}}, Policy{}, nullptr);
	} catch (const std::invalid_argument &) {
		threw = true;
	}
	CHECK(threw);
	threw = false;
	try {
		Supervisor s({"a"}, {{"x"}}, Policy{}, nullptr);
	} catch (const std::invalid_argument &) {
		threw = true;
	}
	CHECK(threw);
}

} // namespace

int main()
{
	test_start_order();
	test_exit_and_restart();
	test_hang_detected_by_heartbeat();
	test_restart_budget();
	test_budget_window();
	test_startup_timeout();
	test_spoofed_heartbeat_ignored();
	test_shutdown_reverse_order();
	test_manifest_errors();
	return TEST_REPORT();
}
