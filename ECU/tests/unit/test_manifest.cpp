// Manifest schema checks (project.md §8.1, §11.1 configuration schema validation).
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

#include "manifest.hpp"
#include "sil_test.h"

using namespace sil::em;

namespace {

const std::map<std::string, std::string> VARS = {
	{"ROOT", "/w"}, {"BIN_DIR", "/w/bin"}, {"RUN_AS", "sil"}, {"NS_AP", "ns_ap"}, {"NS_GW", "ns_gateway"}, {"NS_VEH", "ns_vehicle"},
	{"CAN_VEH_IF", "vxcan_veh"}, {"CAN_GW_IF", "vxcan_gw"}};

bool rejects(const std::string &json, const std::string &expect)
{
	const std::string path = "test_manifest_tmp.json";
	std::ofstream(path) << json;
	try {
		load_manifest(path, VARS);
	} catch (const std::runtime_error &e) {
		std::remove(path.c_str());
		if (std::string(e.what()).find(expect) == std::string::npos) {
			std::fprintf(stderr, "unexpected error: %s (wanted '%s')\n", e.what(), expect.c_str());
			return false;
		}
		return true;
	}
	std::remove(path.c_str());
	return false;
}

std::string one(const std::string &process_body)
{
	return R"({"schema": 1, "processes": [{"name": "engine", "executable": "/w/build/ecu_engine", "args": [], )"
	       R"("netns": "ns_vehicle", "user": "sil", "capabilities": [], "depends_on": [], )" +
	       process_body + "}]}";
}

const std::string LIMITS = R"("limits": {"memory_mb": 64, "cpus": "1"})";

// one valid process plus a policy object
std::string with_policy(const std::string &policy)
{
	std::string j = one(LIMITS);
	return j.insert(j.find("\"processes\""), "\"policy\": {" + policy + "}, ");
}

Manifest loads(const std::string &json)
{
	const std::string path = "test_manifest_tmp.json";
	std::ofstream(path) << json;
	Manifest m = load_manifest(path, VARS);
	std::remove(path.c_str());
	return m;
}

// #6: every time policy has a range, checked before unit conversion; the error names the key
void test_time_policy_ranges()
{
	struct Key {
		const char *name;
		long long min, max, def;
		std::int64_t Policy::*field;
		std::int64_t ns;
	};
	const Key keys[] = {
		{"restart_delay_ms", 1, 3000, 500, &Policy::restart_delay_ns, 1'000'000},
		{"failure_window_s", 1, 3600, 60, &Policy::failure_window_ns, 1'000'000'000},
		{"startup_timeout_ms", 100, 60000, 5000, &Policy::startup_timeout_ns, 1'000'000},
		{"stop_timeout_ms", 100, 30000, 2000, &Policy::stop_timeout_ns, 1'000'000},
	};
	const Manifest d = loads(one(LIMITS)); // no policy object: defaults
	for (const Key &k : keys) {
		const std::string key = std::string("\"") + k.name + "\": ";
		CHECK_EQ_INT(d.policy.*k.field, k.def * k.ns);
		CHECK_EQ_INT(loads(with_policy(key + std::to_string(k.min))).policy.*k.field, k.min * k.ns);
		CHECK_EQ_INT(loads(with_policy(key + std::to_string(k.max))).policy.*k.field, k.max * k.ns);
		CHECK(rejects(with_policy(key + "0"), k.name));
		CHECK(rejects(with_policy(key + "-1"), k.name));
		CHECK(rejects(with_policy(key + std::to_string(k.min - 1)), k.name));
		CHECK(rejects(with_policy(key + std::to_string(k.max + 1)), k.name));
		CHECK(rejects(with_policy(key + "9223372036854775807"), k.name));     // would overflow when converted
		CHECK(rejects(with_policy(key + "99999999999999999999"), k.name));    // does not fit int64
		CHECK(rejects(with_policy(key + "\"abc\""), k.name));
		CHECK(rejects(with_policy(key + "\"10x\""), k.name));
	}
	// heartbeat period: positive, and the ms -> ns conversion must not overflow
	CHECK(rejects(with_policy(R"("heartbeat_period_ms": 0)"), "heartbeat_period_ms"));
	CHECK(rejects(with_policy(R"("heartbeat_period_ms": -5)"), "heartbeat_period_ms"));
	CHECK(rejects(with_policy(R"("heartbeat_period_ms": 9223372036854775807)"), "heartbeat_period_ms"));
	CHECK_EQ_INT(loads(with_policy(R"("heartbeat_period_ms": 500)")).policy.heartbeat_period_ns, 500'000'000);
}

} // namespace

int main(int argc, char **argv)
{
	// the project manifest loads and resolves
	Manifest m = load_manifest(argc > 1 ? argv[1] : "config/manifests/execution.json", VARS);
	CHECK_EQ_INT(m.processes.size(), 5);
	CHECK(m.processes[0].executable == "/w/bin/ecu_engine");
	CHECK(m.processes[0].args.size() == 2 && m.processes[0].args[1] == "vxcan_veh");
	CHECK(m.processes[3].depends_on.size() == 3);
	CHECK(m.processes[3].env.at("VSOMEIP_CONFIGURATION") == "/w/config/vsomeip/gateway.json");
	CHECK(m.processes[4].netns == "ns_ap");
	CHECK(m.policy.heartbeat_period_ns == 500'000'000);
	CHECK(m.policy.failure_window_ns == 60'000'000'000);
	for (auto &p : m.processes)
		CHECK(p.capabilities.empty() && p.memory_mb > 0);

	CHECK(substitute("${ROOT}/a/${RUN_AS}", VARS) == "/w/a/sil");
	CHECK((parse_cpu_list("1,3-4") == std::vector<int>{1, 3, 4}));

	// #5: affinity is compared as a CPU set, not as text (kernel may print another canonical form)
	CHECK(same_cpu_set("1", "1"));
	CHECK(same_cpu_set("1,2", "1-2"));
	CHECK(same_cpu_set("1-2", "1,2"));
	CHECK(same_cpu_set("2,1", "1-2"));
	CHECK(same_cpu_set("0,2-3,5", "0,2,3,5"));
	CHECK(same_cpu_set("\t1-2\n", "1,2"));
	CHECK(!same_cpu_set("1,2", "1-3"));
	CHECK(!same_cpu_set("1", "2"));
	CHECK(!same_cpu_set("1-2", "0-2"));
	CHECK(!same_cpu_set("", "1"));
	CHECK(!same_cpu_set("x", "x"));

	CHECK(rejects(one(LIMITS).replace(one(LIMITS).find("\"args\": []"), 10, "\"args\": [\"${NOPE}\"]"), "unresolved"));
	CHECK(rejects(one(R"("limits": {"memory_mb": 64, "cpus": "x"})"), "cpu"));
	CHECK(rejects(one(R"("limits": {"memory_mb": 1, "cpus": "1"})"), "limits"));
	CHECK(rejects(one(LIMITS).replace(one(LIMITS).find("\"capabilities\": []"), 18, R"("capabilities": ["CAP_NET_RAW"])"),
		      "capabilities"));
	CHECK(rejects(one(LIMITS).replace(one(LIMITS).find("/w/build/ecu_engine"), 19, "ecu_engine"), "absolute"));
	CHECK(rejects(one(LIMITS).replace(one(LIMITS).find("\"engine\""), 8, "\"Bad Name\""), "name"));
	CHECK(rejects(R"({"schema": 2, "processes": []})", "schema"));
	CHECK(rejects("{not json", "manifest"));
	test_time_policy_ranges();
	return TEST_REPORT();
}
