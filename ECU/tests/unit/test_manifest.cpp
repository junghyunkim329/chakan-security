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
	{"ROOT", "/w"}, {"BIN_DIR", "/w/bin"}, {"RUN_AS", "sil"}, {"NS_AP", "hj_ap"}, {"NS_GW", "hj_gw"}, {"NS_VEH", "hj_veh"},
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
	       R"("netns": "hj_veh", "user": "sil", "capabilities": [], "depends_on": [], )" +
	       process_body + "}]}";
}

const std::string LIMITS = R"("limits": {"memory_mb": 64, "cpus": "1"})";

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
	CHECK(m.processes[4].netns == "hj_ap");
	CHECK(m.policy.heartbeat_period_ns == 500'000'000);
	CHECK(m.policy.failure_window_ns == 60'000'000'000);
	for (auto &p : m.processes)
		CHECK(p.capabilities.empty() && p.memory_mb > 0);

	CHECK(substitute("${ROOT}/a/${RUN_AS}", VARS) == "/w/a/sil");
	CHECK((parse_cpu_list("1,3-4") == std::vector<int>{1, 3, 4}));

	CHECK(rejects(one(LIMITS).replace(one(LIMITS).find("\"args\": []"), 10, "\"args\": [\"${NOPE}\"]"), "unresolved"));
	CHECK(rejects(one(R"("limits": {"memory_mb": 64, "cpus": "x"})"), "cpu"));
	CHECK(rejects(one(R"("limits": {"memory_mb": 1, "cpus": "1"})"), "limits"));
	CHECK(rejects(one(LIMITS).replace(one(LIMITS).find("\"capabilities\": []"), 18, R"("capabilities": ["CAP_NET_RAW"])"),
		      "capabilities"));
	CHECK(rejects(one(LIMITS).replace(one(LIMITS).find("/w/build/ecu_engine"), 19, "ecu_engine"), "absolute"));
	CHECK(rejects(one(LIMITS).replace(one(LIMITS).find("\"engine\""), 8, "\"Bad Name\""), "name"));
	CHECK(rejects(R"({"schema": 2, "processes": []})", "schema"));
	CHECK(rejects("{not json", "manifest"));
	return TEST_REPORT();
}
