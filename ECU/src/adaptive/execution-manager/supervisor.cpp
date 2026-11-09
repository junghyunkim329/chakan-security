#include "supervisor.hpp"

#include <algorithm>
#include <stdexcept>

namespace sil::em {

const char *to_string(MachineState s)
{
	switch (s) {
	case MachineState::Startup: return "Startup";
	case MachineState::Running: return "Running";
	case MachineState::Degraded: return "Degraded";
	case MachineState::Shutdown: return "Shutdown";
	}
	return "?";
}

const char *to_string(ProcState s)
{
	switch (s) {
	case ProcState::NotStarted: return "NotStarted";
	case ProcState::Starting: return "Starting";
	case ProcState::Running: return "Running";
	case ProcState::Restarting: return "Restarting";
	case ProcState::Failed: return "Failed";
	case ProcState::Stopping: return "Stopping";
	case ProcState::Stopped: return "Stopped";
	}
	return "?";
}

Supervisor::Supervisor(std::vector<std::string> names, std::vector<std::vector<std::string>> depends_on, Policy policy,
		       AuditSink sink)
	: policy_(policy), sink_(std::move(sink))
{
	if (names.size() != depends_on.size() || names.empty())
		throw std::invalid_argument("manifest: empty or inconsistent process list");
	for (auto &n : names) {
		if (std::count(names.begin(), names.end(), n) != 1)
			throw std::invalid_argument("manifest: duplicate process " + n);
		procs_.push_back(Proc{});
		procs_.back().name = n;
	}
	for (std::size_t i = 0; i < names.size(); i++) {
		for (auto &d : depends_on[i]) {
			auto it = std::find(names.begin(), names.end(), d);
			if (it == names.end())
				throw std::invalid_argument("manifest: " + names[i] + " depends on unknown " + d);
			procs_[i].deps.push_back(static_cast<std::size_t>(it - names.begin()));
		}
	}
	// Kahn topological sort, stable by manifest order
	std::vector<bool> placed(procs_.size(), false);
	while (order_.size() < procs_.size()) {
		bool progress = false;
		for (std::size_t i = 0; i < procs_.size(); i++) {
			if (placed[i])
				continue;
			bool ready = std::all_of(procs_[i].deps.begin(), procs_[i].deps.end(), [&](std::size_t d) { return placed[d]; });
			if (ready) {
				placed[i] = true;
				order_.push_back(i);
				progress = true;
			}
		}
		if (!progress)
			throw std::invalid_argument("manifest: dependency cycle");
	}
}

void Supervisor::audit(std::int64_t t, const std::string &type, std::size_t i, const std::string &detail)
{
	if (sink_)
		sink_(AuditEvent{t, type, i < procs_.size() ? procs_[i].name : "", i < procs_.size() ? procs_[i].pid : 0, detail});
}

int Supervisor::find_pid(int pid) const
{
	if (pid <= 0)
		return -1;
	for (std::size_t i = 0; i < procs_.size(); i++)
		if (procs_[i].pid == pid)
			return static_cast<int>(i);
	return -1;
}

void Supervisor::on_started(std::size_t i, int pid, std::int64_t now_ns)
{
	Proc &p = procs_[i];
	p.pid = pid;
	p.state = ProcState::Starting;
	p.started_ns = now_ns;
	p.last_hb_ns = 0;
	audit(now_ns, "start", i, p.restarts ? "restart " + std::to_string(p.restarts) : "initial");
	update_state(now_ns);
}

void Supervisor::on_start_failed(std::size_t i, const std::string &why, std::int64_t now_ns)
{
	procs_[i].pid = 0;
	fail(i, "start failed: " + why, now_ns, nullptr);
}

bool Supervisor::on_heartbeat(int pid, const std::string &name, std::int64_t now_ns)
{
	int idx = find_pid(pid);
	if (idx < 0 || procs_[static_cast<std::size_t>(idx)].name != name) {
		if (sink_)
			sink_(AuditEvent{now_ns, "security", name, pid, "heartbeat from unsupervised pid or wrong name ignored"});
		return false;
	}
	Proc &p = procs_[static_cast<std::size_t>(idx)];
	if (p.state != ProcState::Starting && p.state != ProcState::Running)
		return true; // late heartbeat from a process being killed or stopped
	p.last_hb_ns = now_ns;
	if (p.state == ProcState::Starting) {
		p.state = ProcState::Running;
		audit(now_ns, "ready", static_cast<std::size_t>(idx), "first heartbeat");
		update_state(now_ns);
	}
	return true;
}

void Supervisor::on_exit(int pid, const std::string &how, std::int64_t now_ns)
{
	int idx = find_pid(pid);
	if (idx < 0)
		return;
	const std::size_t i = static_cast<std::size_t>(idx);
	Proc &p = procs_[i];
	audit(now_ns, "exit", i, how);
	p.pid = 0;
	if (p.state == ProcState::Stopping) {
		p.state = ProcState::Stopped;
	} else if (p.state == ProcState::Starting || p.state == ProcState::Running) {
		fail(i, "unexpected exit: " + how, now_ns, nullptr);
	}
	// Restarting/Failed: the process was killed by us after a failure; restart waits for this exit
	update_state(now_ns);
}

void Supervisor::fail(std::size_t i, const std::string &why, std::int64_t now_ns, std::vector<Action> *actions)
{
	Proc &p = procs_[i];
	audit(now_ns, "failure", i, why);
	if (p.pid != 0 && actions)
		actions->push_back(Action{Action::Kind::Kill, i, why});

	p.failures.push_back(now_ns);
	while (!p.failures.empty() && now_ns - p.failures.front() > policy_.failure_window_ns)
		p.failures.pop_front();

	if (shutting_down_) {
		p.state = ProcState::Stopping;
	} else if (static_cast<int>(p.failures.size()) >= policy_.max_failures) {
		p.state = ProcState::Failed;
		audit(now_ns, "budget_exhausted", i,
		      std::to_string(p.failures.size()) + " failures within " + std::to_string(policy_.failure_window_ns / 1'000'000'000) +
			      " s: automatic restart stopped");
	} else {
		p.state = ProcState::Restarting;
		p.restart_at_ns = now_ns + policy_.restart_delay_ns;
		audit(now_ns, "restart_scheduled", i, "in " + std::to_string(policy_.restart_delay_ns / 1'000'000) + " ms");
	}
	update_state(now_ns);
}

std::vector<Action> Supervisor::tick(std::int64_t now_ns)
{
	std::vector<Action> actions;

	if (shutting_down_) {
		// stop in reverse dependency order, one process at a time
		for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
			Proc &p = procs_[*it];
			if (p.pid == 0) {
				if (p.state != ProcState::Stopped && p.state != ProcState::Failed && p.state != ProcState::NotStarted)
					p.state = ProcState::Stopped;
				continue;
			}
			p.state = ProcState::Stopping;
			if (p.term_sent_ns < 0) {
				p.term_sent_ns = now_ns;
				audit(now_ns, "stop", *it, "SIGTERM");
				actions.push_back(Action{Action::Kind::Terminate, *it, "shutdown"});
			} else if (!p.kill_sent && now_ns - p.term_sent_ns >= policy_.stop_timeout_ns) {
				p.kill_sent = true;
				audit(now_ns, "kill", *it, "no exit within stop timeout");
				actions.push_back(Action{Action::Kind::Kill, *it, "stop timeout"});
			}
			break;
		}
		return actions;
	}

	const std::int64_t hb_limit = policy_.heartbeat_period_ns * policy_.missed_heartbeats;
	for (std::size_t i = 0; i < procs_.size(); i++) {
		Proc &p = procs_[i];
		if (p.state == ProcState::Running && now_ns - p.last_hb_ns >= hb_limit)
			fail(i, "heartbeat timeout (" + std::to_string(policy_.missed_heartbeats) + " missed)", now_ns, &actions);
		else if (p.state == ProcState::Starting && now_ns - p.started_ns >= policy_.startup_timeout_ns)
			fail(i, "no heartbeat within startup timeout", now_ns, &actions);
	}

	for (std::size_t i : order_) {
		Proc &p = procs_[i];
		const bool deps_ready = std::all_of(p.deps.begin(), p.deps.end(),
						    [&](std::size_t d) { return procs_[d].state == ProcState::Running; });
		if (p.state == ProcState::NotStarted && deps_ready)
			actions.push_back(Action{Action::Kind::Start, i, "dependencies ready"});
		else if (p.state == ProcState::Restarting && p.pid == 0 && now_ns >= p.restart_at_ns) {
			p.restarts++;
			actions.push_back(Action{Action::Kind::Start, i, "restart"});
		}
	}
	return actions;
}

void Supervisor::begin_shutdown(std::int64_t now_ns)
{
	if (shutting_down_)
		return;
	shutting_down_ = true;
	update_state(now_ns);
}

bool Supervisor::shutdown_complete() const
{
	return shutting_down_ && std::all_of(procs_.begin(), procs_.end(), [](const Proc &p) { return p.pid == 0; });
}

void Supervisor::update_state(std::int64_t now_ns)
{
	MachineState next;
	const bool all_running =
		std::all_of(procs_.begin(), procs_.end(), [](const Proc &p) { return p.state == ProcState::Running; });

	if (all_running)
		startup_complete_ = true;
	if (shutting_down_)
		next = MachineState::Shutdown;
	else if (all_running)
		next = MachineState::Running;
	else if (!startup_complete_ &&
		 std::none_of(procs_.begin(), procs_.end(), [](const Proc &p) {
			 return p.state == ProcState::Failed || p.state == ProcState::Restarting;
		 }))
		next = MachineState::Startup;
	else
		next = MachineState::Degraded;

	if (next != state_) {
		if (sink_)
			sink_(AuditEvent{now_ns, "state", "", 0, std::string(to_string(state_)) + " -> " + to_string(next)});
		state_ = next;
	}
}

} // namespace sil::em
