// Execution manifest loading and static checks (project.md §8.1, §11.1 configuration schema validation).
#pragma once

#include <map>
#include <string>
#include <vector>

#include "supervisor.hpp"

namespace sil::em {

struct ProcessManifest {
	std::string name;
	std::string executable;
	std::vector<std::string> args;
	std::string netns;
	std::string user;
	std::vector<std::string> capabilities;
	std::vector<std::string> depends_on;
	std::map<std::string, std::string> env;
	unsigned memory_mb = 0;
	std::string cpus;     // cpu list for sched_setaffinity, e.g. "1" or "2-3"
	int nice = 0;
	unsigned open_files = 0;
};

struct Manifest {
	Policy policy;
	std::vector<ProcessManifest> processes;
};

// Throws std::runtime_error with a readable message on any schema error or unresolved ${VAR}.
Manifest load_manifest(const std::string &path, const std::map<std::string, std::string> &vars);

// Replaces ${NAME} from vars. Throws on unknown names.
std::string substitute(const std::string &s, const std::map<std::string, std::string> &vars);

// Parses "1", "0-2", "1,3" into cpu indices. Throws on bad syntax.
std::vector<int> parse_cpu_list(const std::string &s);

} // namespace sil::em
