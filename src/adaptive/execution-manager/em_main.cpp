// Educational Execution Manager (project.md §8.1, §8.2). Not an AUTOSAR Execution Management implementation.
//
// Runs as root only to enter network namespaces and drop privileges; every managed process runs as the
// manifest user with no capabilities, no_new_privs, its own process group, rlimits and a CPU set.
//
// usage: execution_manager --manifest <json> --runtime-dir <dir> --audit-log <jsonl> --status-file <json>
//                          [--var NAME=VALUE]...
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "manifest.hpp"
#include "supervisor.hpp"

using namespace sil::em;

namespace {

std::int64_t mono_ns()
{
	timespec ts{};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

std::string wall_time()
{
	timespec ts{};
	clock_gettime(CLOCK_REALTIME, &ts);
	tm local{};
	localtime_r(&ts.tv_sec, &local);
	char buf[64], tz[8];
	std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &local);
	std::strftime(tz, sizeof(tz), "%z", &local);
	char out[96];
	std::snprintf(out, sizeof(out), "%s.%03ld%.3s:%.2s", buf, ts.tv_nsec / 1'000'000, tz, tz + 3);
	return out;
}

std::string json_escape(const std::string &s)
{
	std::string o;
	for (char c : s) {
		if (c == '"' || c == '\\')
			o += '\\', o += c;
		else if (static_cast<unsigned char>(c) < 0x20)
			o += ' ';
		else
			o += c;
	}
	return o;
}

[[noreturn]] void die(const std::string &msg)
{
	std::fprintf(stderr, "[em] %s\n", msg.c_str());
	std::exit(1);
}

struct Ctx {
	Manifest manifest;
	std::vector<uid_t> uid;
	std::vector<gid_t> gid;
	std::vector<std::string> home;
	std::string runtime_dir, hb_path, log_dir;
	std::FILE *audit = nullptr;
	std::string status_file;
};

void write_audit(Ctx &c, const AuditEvent &e)
{
	std::fprintf(c.audit, "{\"time\": \"%s\", \"mono_ns\": %lld, \"type\": \"%s\", \"process\": \"%s\", \"pid\": %d, \"detail\": \"%s\"}\n",
		     wall_time().c_str(), static_cast<long long>(e.t_ns), e.type.c_str(), json_escape(e.process).c_str(), e.pid,
		     json_escape(e.detail).c_str());
	std::fflush(c.audit);
	std::fprintf(stderr, "[em] %s %s %s %s\n", e.type.c_str(), e.process.c_str(), e.pid ? std::to_string(e.pid).c_str() : "",
		     e.detail.c_str());
}

void write_status(Ctx &c, const Supervisor &sup)
{
	std::ostringstream o;
	o << "{\"time\": \"" << wall_time() << "\", \"mono_ns\": " << mono_ns() << ", \"state\": \"" << to_string(sup.state())
	  << "\", \"processes\": [";
	for (std::size_t i = 0; i < sup.size(); i++)
		o << (i ? ", " : "") << "{\"name\": \"" << sup.name(i) << "\", \"state\": \"" << to_string(sup.proc_state(i))
		  << "\", \"pid\": " << sup.pid(i) << ", \"restarts\": " << sup.restarts(i) << "}";
	o << "]}\n";
	const std::string tmp = c.status_file + ".tmp";
	std::ofstream(tmp) << o.str();
	std::rename(tmp.c_str(), c.status_file.c_str());
}

// §8.1 "실행 파일과 인자 확인": absolute, regular, executable, not writable by others,
// owned by root or the run user, directory not writable by others
void check_executable(const ProcessManifest &p, uid_t run_uid)
{
	struct stat st{};
	if (stat(p.executable.c_str(), &st) != 0)
		throw std::runtime_error(p.name + ": executable " + p.executable + ": " + std::strerror(errno));
	if (!S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR))
		throw std::runtime_error(p.name + ": " + p.executable + " is not an executable regular file");
	if (st.st_mode & (S_IWGRP | S_IWOTH))
		throw std::runtime_error(p.name + ": " + p.executable + " is writable by group/others");
	if (st.st_uid != 0 && st.st_uid != run_uid)
		throw std::runtime_error(p.name + ": " + p.executable + " is owned by another user");
	std::string dir = p.executable.substr(0, p.executable.rfind('/'));
	if (stat(dir.c_str(), &st) == 0 && (st.st_mode & S_IWOTH) && !(st.st_mode & S_ISVTX))
		throw std::runtime_error(p.name + ": directory " + dir + " is world-writable");
	for (auto &a : p.args)
		if (a.size() > 256 || a.find('\0') != std::string::npos)
			throw std::runtime_error(p.name + ": invalid argument");
}

// Child side, between fork and exec. Any failure is reported through errfd and the child exits.
[[noreturn]] void child_exec(const Ctx &c, std::size_t i, int errfd, int logfd)
{
	const ProcessManifest &p = c.manifest.processes[i];
	auto fail = [&](const std::string &what) {
		std::string m = what + ": " + std::strerror(errno);
		ssize_t r = write(errfd, m.data(), m.size());
		(void)r;
		_exit(127);
	};

	sigset_t none;
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, nullptr);
	if (setsid() < 0)
		fail("setsid");

	int nsfd = open(("/run/netns/" + p.netns).c_str(), O_RDONLY | O_CLOEXEC);
	if (nsfd < 0 || setns(nsfd, CLONE_NEWNET) != 0)
		fail("setns " + p.netns);
	close(nsfd);

	rlimit mem{static_cast<rlim_t>(p.memory_mb) * 1024 * 1024, static_cast<rlim_t>(p.memory_mb) * 1024 * 1024};
	rlimit files{p.open_files, p.open_files};
	rlimit core{0, 0};
	if (setrlimit(RLIMIT_AS, &mem) || setrlimit(RLIMIT_NOFILE, &files) || setrlimit(RLIMIT_CORE, &core))
		fail("setrlimit");
	cpu_set_t set;
	CPU_ZERO(&set);
	for (int cpu : parse_cpu_list(p.cpus))
		CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set) != 0)
		fail("sched_setaffinity " + p.cpus);
	if (setpriority(PRIO_PROCESS, 0, p.nice) != 0)
		fail("setpriority");

	// drop privileges: no supplementary groups, then gid, then uid; root must be unrecoverable
	if (setgroups(0, nullptr) != 0 || setresgid(c.gid[i], c.gid[i], c.gid[i]) != 0 ||
	    setresuid(c.uid[i], c.uid[i], c.uid[i]) != 0)
		fail("drop privileges");
	if (setuid(0) == 0)
		fail("privilege drop not permanent");
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
		fail("no_new_privs");

	if (dup2(logfd, STDOUT_FILENO) < 0 || dup2(logfd, STDERR_FILENO) < 0)
		fail("dup2 log");
	int devnull = open("/dev/null", O_RDONLY);
	if (devnull < 0 || dup2(devnull, STDIN_FILENO) < 0)
		fail("stdin");
	if (chdir("/") != 0)
		fail("chdir");

	std::vector<std::string> env = {"PATH=/usr/bin:/bin", "HOME=" + c.home[i], "USER=" + p.user, "LANG=C.UTF-8",
					"SIL_PROCESS_NAME=" + p.name, "SIL_HEARTBEAT_SOCKET=" + c.hb_path,
					"SIL_HEARTBEAT_PERIOD_MS=" + std::to_string(c.manifest.policy.heartbeat_period_ns / 1'000'000)};
	for (auto &kv : p.env)
		env.push_back(kv.first + "=" + kv.second);
	std::vector<char *> envp, argv;
	for (auto &e : env)
		envp.push_back(const_cast<char *>(e.c_str()));
	envp.push_back(nullptr);
	argv.push_back(const_cast<char *>(p.executable.c_str()));
	for (auto &a : p.args)
		argv.push_back(const_cast<char *>(a.c_str()));
	argv.push_back(nullptr);
	execve(p.executable.c_str(), argv.data(), envp.data());
	fail("execve " + p.executable);
	_exit(127); // not reached: fail() exits
}

// returns pid, or -1 with why filled
int launch(Ctx &c, std::size_t i, std::string &why)
{
	const ProcessManifest &p = c.manifest.processes[i];
	try {
		check_executable(p, c.uid[i]);
	} catch (const std::exception &e) {
		why = e.what();
		return -1;
	}
	int errpipe[2];
	if (pipe2(errpipe, O_CLOEXEC) != 0) {
		why = "pipe";
		return -1;
	}
	const std::string log = c.log_dir + "/" + p.name + ".log";
	int logfd = open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
	if (logfd < 0 || fchown(logfd, c.uid[i], c.gid[i]) != 0) {
		why = "log file " + log;
		close(errpipe[0]);
		close(errpipe[1]);
		if (logfd >= 0)
			close(logfd);
		return -1;
	}
	pid_t pid = fork();
	if (pid == 0) {
		close(errpipe[0]);
		child_exec(c, i, errpipe[1], logfd);
	}
	close(errpipe[1]);
	close(logfd);
	if (pid < 0) {
		close(errpipe[0]);
		why = "fork";
		return -1;
	}
	char buf[256];
	ssize_t n = read(errpipe[0], buf, sizeof(buf) - 1); // 0 = exec succeeded (pipe closed on exec)
	close(errpipe[0]);
	if (n > 0) {
		buf[n] = '\0';
		why = buf;
		waitpid(pid, nullptr, 0);
		return -1;
	}
	return pid;
}

std::string describe(int status)
{
	if (WIFEXITED(status))
		return "exit status " + std::to_string(WEXITSTATUS(status));
	if (WIFSIGNALED(status))
		return std::string("killed by signal ") + std::to_string(WTERMSIG(status)) + " (" + strsignal(WTERMSIG(status)) + ")";
	return "status " + std::to_string(status);
}

// §8.1 "프로세스별 사용자와 capability 확인": read back what the kernel actually applied
std::string verify_process(const Ctx &c, std::size_t i, int pid, bool &ok)
{
	std::ifstream st("/proc/" + std::to_string(pid) + "/status");
	std::string line, uid, capeff, capprm, nnp, cpus;
	while (std::getline(st, line)) {
		auto val = [&](const char *k) { return line.rfind(k, 0) == 0 ? line.substr(std::strlen(k)) : std::string(); };
		if (line.rfind("Uid:", 0) == 0) uid = val("Uid:");
		if (line.rfind("CapEff:", 0) == 0) capeff = val("CapEff:");
		if (line.rfind("CapPrm:", 0) == 0) capprm = val("CapPrm:");
		if (line.rfind("NoNewPrivs:", 0) == 0) nnp = val("NoNewPrivs:");
		if (line.rfind("Cpus_allowed_list:", 0) == 0) cpus = val("Cpus_allowed_list:");
	}
	auto trim = [](std::string s) {
		s.erase(0, s.find_first_not_of(" \t"));
		return s;
	};
	std::istringstream us(uid);
	unsigned ruid = 1, euid = 1, suid = 1;
	us >> ruid >> euid >> suid;
	struct stat ns_proc{}, ns_want{};
	const bool ns_ok = stat(("/proc/" + std::to_string(pid) + "/ns/net").c_str(), &ns_proc) == 0 &&
			   stat(("/run/netns/" + c.manifest.processes[i].netns).c_str(), &ns_want) == 0 &&
			   ns_proc.st_ino == ns_want.st_ino;
	ok = euid != 0 && ruid != 0 && suid != 0 && trim(capeff) == "0000000000000000" && trim(capprm) == "0000000000000000" &&
	     trim(nnp) == "1" && trim(cpus) == c.manifest.processes[i].cpus && ns_ok;
	return "uid=" + std::to_string(euid) + " CapEff=" + trim(capeff) + " CapPrm=" + trim(capprm) + " NoNewPrivs=" + trim(nnp) +
	       " cpus=" + trim(cpus) + " netns=" + (ns_ok ? c.manifest.processes[i].netns : std::string("MISMATCH")) +
	       " memory_mb=" + std::to_string(c.manifest.processes[i].memory_mb);
}

int open_heartbeat_socket(const Ctx &c, gid_t group)
{
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		die("heartbeat socket");
	int on = 1;
	setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on));
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	if (c.hb_path.size() >= sizeof(addr.sun_path))
		die("heartbeat socket path too long");
	std::memcpy(addr.sun_path, c.hb_path.c_str(), c.hb_path.size() + 1);
	unlink(c.hb_path.c_str());
	if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
		die("bind " + c.hb_path + ": " + std::strerror(errno));
	if (chown(c.hb_path.c_str(), 0, group) != 0 || chmod(c.hb_path.c_str(), 0620) != 0)
		die("heartbeat socket permissions");
	return fd;
}

} // namespace

int main(int argc, char **argv)
{
	std::string manifest_path, audit_path;
	std::map<std::string, std::string> vars;
	Ctx c;

	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&]() -> std::string {
			if (i + 1 >= argc)
				die("missing value for " + a);
			return argv[++i];
		};
		if (a == "--manifest") manifest_path = next();
		else if (a == "--runtime-dir") c.runtime_dir = next();
		else if (a == "--audit-log") audit_path = next();
		else if (a == "--status-file") c.status_file = next();
		else if (a == "--var") {
			std::string kv = next();
			auto eq = kv.find('=');
			if (eq == std::string::npos)
				die("--var needs NAME=VALUE");
			vars[kv.substr(0, eq)] = kv.substr(eq + 1);
		} else
			die("unknown argument " + a);
	}
	if (manifest_path.empty() || c.runtime_dir.empty() || audit_path.empty() || c.status_file.empty())
		die("usage: execution_manager --manifest <json> --runtime-dir <dir> --audit-log <jsonl> --status-file <json> [--var K=V]...");
	if (geteuid() != 0)
		die("must run as root (enters network namespaces and drops privileges for each process)");

	try {
		c.manifest = load_manifest(manifest_path, vars);
	} catch (const std::exception &e) {
		die(e.what());
	}
	std::vector<std::string> names;
	std::vector<std::vector<std::string>> deps;
	gid_t group = 0;
	for (auto &p : c.manifest.processes) {
		passwd *pw = getpwnam(p.user.c_str());
		if (pw == nullptr || pw->pw_uid == 0)
			die(p.name + ": user '" + p.user + "' does not exist or is root");
		c.uid.push_back(pw->pw_uid);
		c.gid.push_back(pw->pw_gid);
		c.home.push_back(pw->pw_dir);
		group = pw->pw_gid;
		names.push_back(p.name);
		deps.push_back(p.depends_on);
		struct stat st{};
		if (stat(("/run/netns/" + p.netns).c_str(), &st) != 0)
			die(p.name + ": network namespace " + p.netns + " does not exist");
	}

	// runtime dir: root-owned, group = run user's group, no access for others
	mkdir(c.runtime_dir.c_str(), 0750);
	if (chown(c.runtime_dir.c_str(), 0, group) != 0 || chmod(c.runtime_dir.c_str(), 0750) != 0)
		die("runtime dir " + c.runtime_dir);
	c.hb_path = c.runtime_dir + "/heartbeat.sock";
	c.log_dir = c.runtime_dir + "/logs";
	mkdir(c.log_dir.c_str(), 0750);
	if (chown(c.log_dir.c_str(), 0, group) != 0)
		die("log dir");
	c.audit = std::fopen(audit_path.c_str(), "a");
	if (c.audit == nullptr)
		die("audit log " + audit_path);

	sigset_t sigs;
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGCHLD);
	sigaddset(&sigs, SIGTERM);
	sigaddset(&sigs, SIGINT);
	sigprocmask(SIG_BLOCK, &sigs, nullptr);
	int sfd = signalfd(-1, &sigs, SFD_CLOEXEC | SFD_NONBLOCK);
	int hfd = open_heartbeat_socket(c, group);

	Supervisor sup(names, deps, c.manifest.policy, [&c](const AuditEvent &e) { write_audit(c, e); });
	write_audit(c, AuditEvent{mono_ns(), "em_start", "", static_cast<int>(getpid()),
				  "manifest " + manifest_path + ", " + std::to_string(names.size()) + " processes"});

	std::int64_t last_status = 0;
	MachineState last_state = sup.state();
	write_status(c, sup);

	while (!sup.shutdown_complete()) {
		pollfd fds[2] = {{sfd, POLLIN, 0}, {hfd, POLLIN, 0}};
		poll(fds, 2, 50);
		std::int64_t now = mono_ns();
		bool dirty = false; // status file is rewritten whenever a pid or state may have changed

		signalfd_siginfo si{};
		while (read(sfd, &si, sizeof(si)) == static_cast<ssize_t>(sizeof(si))) {
			if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT)
				sup.begin_shutdown(now);
		}
		int status = 0;
		pid_t pid;
		while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
			sup.on_exit(pid, describe(status), now);
			dirty = true;
		}

		// heartbeats: sender identity comes from kernel credentials (SO_PASSCRED), not from the payload
		for (;;) {
			char buf[128];
			char cbuf[CMSG_SPACE(sizeof(ucred))];
			iovec iov{buf, sizeof(buf) - 1};
			msghdr msg{};
			msg.msg_iov = &iov;
			msg.msg_iovlen = 1;
			msg.msg_control = cbuf;
			msg.msg_controllen = sizeof(cbuf);
			ssize_t n = recvmsg(hfd, &msg, 0);
			if (n <= 0)
				break;
			buf[n] = '\0';
			int sender = -1;
			for (cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm))
				if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_CREDENTIALS) {
					ucred cr{};
					std::memcpy(&cr, CMSG_DATA(cm), sizeof(cr));
					sender = cr.pid;
				}
			char name[40] = {};
			unsigned seq = 0;
			if (std::sscanf(buf, "HB1 %39s %u", name, &seq) != 2)
				continue;
			int idx = -1;
			for (std::size_t i = 0; i < sup.size(); i++)
				if (sup.pid(i) == sender)
					idx = static_cast<int>(i);
			const bool was_starting = idx >= 0 && sup.proc_state(static_cast<std::size_t>(idx)) == ProcState::Starting;
			if (sup.on_heartbeat(sender, name, now) && was_starting) {
				dirty = true;
				bool ok = false;
				std::string v = verify_process(c, static_cast<std::size_t>(idx), sender, ok);
				write_audit(c, AuditEvent{now, ok ? "verify" : "security", name, sender, v});
				if (!ok)
					kill(-sender, SIGKILL); // exit is then handled as a failure
			}
		}

		for (const Action &a : sup.tick(now)) {
			dirty = true;
			if (a.kind == Action::Kind::Start) {
				std::string why;
				int p = launch(c, a.proc, why);
				if (p > 0)
					sup.on_started(a.proc, p, mono_ns());
				else
					sup.on_start_failed(a.proc, why, mono_ns());
			} else {
				const int p = sup.pid(a.proc);
				if (p > 0)
					kill(-p, a.kind == Action::Kind::Kill ? SIGKILL : SIGTERM);
			}
		}

		if (dirty || sup.state() != last_state || now - last_status > 1'000'000'000) {
			last_state = sup.state();
			last_status = now;
			write_status(c, sup);
		}
	}

	write_status(c, sup);
	write_audit(c, AuditEvent{mono_ns(), "em_stop", "", static_cast<int>(getpid()), "all processes stopped"});
	unlink(c.hb_path.c_str());
	std::fclose(c.audit);
	return 0;
}
