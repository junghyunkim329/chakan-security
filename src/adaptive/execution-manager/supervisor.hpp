// Execution Manager decision logic (project.md §8.1, §8.2). Pure: no OS calls, time is passed in,
// so start order, heartbeat timeout, restart budget and state transitions are unit-testable.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace sil::em {

enum class MachineState { Startup, Running, Degraded, Shutdown };
enum class ProcState { NotStarted, Starting, Running, Restarting, Failed, Stopping, Stopped };

const char *to_string(MachineState s);
const char *to_string(ProcState s);

struct Policy {
	std::int64_t heartbeat_period_ns = 500'000'000;  // §8.2 500 ms
	int missed_heartbeats = 3;                        // §8.2 3 missed -> failure (~1.5 s)
	std::int64_t restart_delay_ns = 500'000'000;      // restart attempt well inside §8.2 3 s
	int max_failures = 3;                             // §8.2 3 failures ...
	std::int64_t failure_window_ns = 60'000'000'000;  // ... within 60 s -> stop restarting
	std::int64_t startup_timeout_ns = 5'000'000'000;  // first heartbeat must arrive within 5 s
	std::int64_t stop_timeout_ns = 2'000'000'000;     // SIGTERM -> SIGKILL on shutdown
};

struct Action {
	enum class Kind { Start, Terminate, Kill } kind;
	std::size_t proc;
	std::string reason;
};

struct AuditEvent {
	std::int64_t t_ns;
	std::string type;   // state, start, ready, exit, failure, restart_scheduled, budget_exhausted, kill, stop, security
	std::string process;
	int pid;
	std::string detail;
};

class Supervisor {
public:
	using AuditSink = std::function<void(const AuditEvent &)>;

	// names[i] depends on every name in depends_on[i]. Throws std::invalid_argument on unknown name or cycle.
	Supervisor(std::vector<std::string> names, std::vector<std::vector<std::string>> depends_on, Policy policy,
		   AuditSink sink);

	// Decide what to do now: start processes whose dependencies are ready, declare heartbeat/startup
	// timeouts, schedule restarts, and run the shutdown sequence.
	std::vector<Action> tick(std::int64_t now_ns);

	void on_started(std::size_t proc, int pid, std::int64_t now_ns);
	void on_start_failed(std::size_t proc, const std::string &why, std::int64_t now_ns);
	// returns false if the pid is not a supervised process or the name does not match (spoofing)
	bool on_heartbeat(int pid, const std::string &name, std::int64_t now_ns);
	void on_exit(int pid, const std::string &how, std::int64_t now_ns);
	void begin_shutdown(std::int64_t now_ns);

	MachineState state() const { return state_; }
	bool shutdown_complete() const;
	std::size_t size() const { return procs_.size(); }
	const std::string &name(std::size_t i) const { return procs_[i].name; }
	ProcState proc_state(std::size_t i) const { return procs_[i].state; }
	int pid(std::size_t i) const { return procs_[i].pid; }
	unsigned restarts(std::size_t i) const { return procs_[i].restarts; }
	const std::vector<std::size_t> &start_order() const { return order_; }

private:
	struct Proc {
		std::string name;
		std::vector<std::size_t> deps;
		ProcState state = ProcState::NotStarted;
		int pid = 0;              // nonzero while the OS process exists
		std::int64_t started_ns = 0;
		std::int64_t last_hb_ns = 0;
		std::int64_t restart_at_ns = 0;
		std::int64_t term_sent_ns = -1;
		bool kill_sent = false;
		unsigned restarts = 0;
		std::deque<std::int64_t> failures;
	};

	void fail(std::size_t i, const std::string &why, std::int64_t now_ns, std::vector<Action> *actions);
	void update_state(std::int64_t now_ns);
	void audit(std::int64_t t, const std::string &type, std::size_t i, const std::string &detail);
	int find_pid(int pid) const;

	std::vector<Proc> procs_;
	std::vector<std::size_t> order_; // dependency (topological) order
	Policy policy_;
	AuditSink sink_;
	MachineState state_ = MachineState::Startup;
	bool startup_complete_ = false;
	bool shutting_down_ = false;
};

} // namespace sil::em
