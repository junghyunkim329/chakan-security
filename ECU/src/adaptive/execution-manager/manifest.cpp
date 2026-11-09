#include "manifest.hpp"

#include <regex>
#include <set>
#include <stdexcept>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

namespace pt = boost::property_tree;

namespace sil::em {

std::string substitute(const std::string &s, const std::map<std::string, std::string> &vars)
{
	static const std::regex var(R"(\$\{([A-Z0-9_]+)\})");
	std::string out;
	auto it = std::sregex_iterator(s.begin(), s.end(), var);
	std::size_t last = 0;
	for (; it != std::sregex_iterator(); ++it) {
		const auto &m = *it;
		auto v = vars.find(m[1].str());
		if (v == vars.end())
			throw std::runtime_error("manifest: unresolved variable ${" + m[1].str() + "}");
		out += s.substr(last, static_cast<std::size_t>(m.position(0)) - last) + v->second;
		last = static_cast<std::size_t>(m.position(0) + m.length(0));
	}
	out += s.substr(last);
	if (out.find("${") != std::string::npos)
		throw std::runtime_error("manifest: malformed variable in '" + s + "'");
	return out;
}

std::vector<int> parse_cpu_list(const std::string &s)
{
	static const std::regex item(R"(^(\d+)(?:-(\d+))?$)");
	std::vector<int> cpus;
	std::size_t start = 0;
	while (start <= s.size()) {
		std::size_t comma = s.find(',', start);
		std::string part = s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
		std::smatch m;
		if (!std::regex_match(part, m, item))
			throw std::runtime_error("manifest: bad cpu list '" + s + "'");
		int a = std::stoi(m[1].str()), b = m[2].matched ? std::stoi(m[2].str()) : a;
		if (b < a || b > 1023)
			throw std::runtime_error("manifest: bad cpu range '" + part + "'");
		for (int c = a; c <= b; c++)
			cpus.push_back(c);
		if (comma == std::string::npos)
			break;
		start = comma + 1;
	}
	return cpus;
}

namespace {

std::vector<std::string> string_array(const pt::ptree &node, const std::string &key,
				      const std::map<std::string, std::string> &vars)
{
	std::vector<std::string> out;
	auto child = node.get_child_optional(key);
	if (!child)
		throw std::runtime_error("manifest: missing array '" + key + "'");
	for (auto &kv : *child) {
		if (!kv.first.empty() || !kv.second.empty())
			throw std::runtime_error("manifest: '" + key + "' must be an array of strings");
		out.push_back(substitute(kv.second.data(), vars));
	}
	return out;
}

std::string required(const pt::ptree &node, const std::string &key, const std::map<std::string, std::string> &vars)
{
	auto v = node.get_optional<std::string>(key);
	if (!v || v->empty())
		throw std::runtime_error("manifest: missing '" + key + "'");
	return substitute(*v, vars);
}

std::int64_t ms(const pt::ptree &p, const std::string &key, std::int64_t def_ms)
{
	return p.get<std::int64_t>(key, def_ms) * 1'000'000;
}

} // namespace

Manifest load_manifest(const std::string &path, const std::map<std::string, std::string> &vars)
{
	pt::ptree root;
	try {
		pt::read_json(path, root);
	} catch (const pt::json_parser_error &e) {
		throw std::runtime_error(std::string("manifest: ") + e.what());
	}
	if (root.get<int>("schema", 0) != 1)
		throw std::runtime_error("manifest: unsupported schema");

	Manifest m;
	if (auto p = root.get_child_optional("policy")) {
		m.policy.heartbeat_period_ns = ms(*p, "heartbeat_period_ms", 500);
		m.policy.missed_heartbeats = p->get<int>("missed_heartbeats", 3);
		m.policy.restart_delay_ns = ms(*p, "restart_delay_ms", 500);
		m.policy.max_failures = p->get<int>("max_failures", 3);
		m.policy.failure_window_ns = p->get<std::int64_t>("failure_window_s", 60) * 1'000'000'000;
		m.policy.startup_timeout_ns = ms(*p, "startup_timeout_ms", 5000);
		m.policy.stop_timeout_ns = ms(*p, "stop_timeout_ms", 2000);
	}
	if (m.policy.heartbeat_period_ns <= 0 || m.policy.missed_heartbeats < 1 || m.policy.max_failures < 1)
		throw std::runtime_error("manifest: invalid policy");

	static const std::regex name_re("^[a-z][a-z0-9_]{0,30}$");
	std::set<std::string> names;
	for (auto &kv : root.get_child("processes")) {
		const pt::ptree &n = kv.second;
		ProcessManifest p;
		p.name = required(n, "name", vars);
		if (!std::regex_match(p.name, name_re) || !names.insert(p.name).second)
			throw std::runtime_error("manifest: invalid or duplicate process name '" + p.name + "'");
		p.executable = required(n, "executable", vars);
		if (p.executable.front() != '/')
			throw std::runtime_error("manifest: " + p.name + ": executable must be an absolute path");
		p.args = string_array(n, "args", vars);
		p.netns = required(n, "netns", vars);
		p.user = required(n, "user", vars);
		p.capabilities = string_array(n, "capabilities", vars);
		// CAN_RAW sockets need no capability; the project grants none (§8.1, §11.1)
		if (!p.capabilities.empty())
			throw std::runtime_error("manifest: " + p.name + ": capabilities are not granted by this Execution Manager");
		p.depends_on = string_array(n, "depends_on", vars);
		if (auto env = n.get_child_optional("env"))
			for (auto &e : *env)
				p.env[e.first] = substitute(e.second.data(), vars);
		const pt::ptree &lim = n.get_child("limits");
		p.memory_mb = lim.get<unsigned>("memory_mb");
		p.cpus = lim.get<std::string>("cpus");
		p.nice = lim.get<int>("nice", 0);
		p.open_files = lim.get<unsigned>("open_files", 64);
		if (p.memory_mb < 16 || p.open_files < 16 || p.nice < 0 || p.nice > 19)
			throw std::runtime_error("manifest: " + p.name + ": limits out of range");
		parse_cpu_list(p.cpus);
		m.processes.push_back(std::move(p));
	}
	if (m.processes.empty())
		throw std::runtime_error("manifest: no processes");
	return m;
}

} // namespace sil::em
