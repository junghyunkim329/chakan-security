// BodyControl request policy (project.md §7.3): allowlist, payload length and range, rate limit, vehicle state,
// then the LIN command; the method answer waits for the slave's next status (or reports a LIN timeout).
// Pure: no SOME/IP, time passed in.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "lin_master.hpp"

namespace sil::body {

enum Result : std::uint8_t { kAccepted = 0, kInvalidArgument = 1, kStateRejected = 2, kLinTimeout = 3 };
enum Method : std::uint16_t { kSetWindowPosition = 0x0001, kSetInteriorLight = 0x0002 };

struct Decision {
	bool pending = false;     // answer later via poll()
	std::uint8_t code = kAccepted;
	std::string security_event; // non-empty: record as a security event
};

struct Completion {
	std::uint64_t token;
	std::uint8_t code;
};

class BodyControl {
public:
	BodyControl(std::set<std::uint16_t> allowed_clients, unsigned max_per_second, std::int64_t confirm_timeout_ns);

	// ignition_ok: engine ignition ON, valid and fresh (practice selection for the §7.3 vehicle state check)
	Decision request(std::uint16_t method, std::uint16_t client, const std::vector<std::uint8_t> &payload,
			 std::int64_t now_ns, bool ignition_ok, lin::LinMaster &lin, std::uint64_t token);
	std::vector<Completion> poll(std::int64_t now_ns, const lin::LinMaster &lin);

	std::uint64_t rejected(std::uint8_t code) const;

private:
	struct Pending {
		std::uint64_t token;
		std::uint16_t method;
		std::int64_t issued_ns;
	};

	std::set<std::uint16_t> allowed_;
	unsigned max_per_second_;
	std::int64_t confirm_timeout_ns_;
	std::map<std::uint16_t, std::deque<std::int64_t>> recent_;
	std::vector<Pending> pending_;
	std::map<std::uint8_t, std::uint64_t> rejected_;
};

} // namespace sil::body
