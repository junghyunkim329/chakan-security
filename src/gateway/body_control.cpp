#include "body_control.hpp"

namespace sil::body {

namespace {
constexpr std::int64_t kSecond = 1'000'000'000;
}

BodyControl::BodyControl(std::set<std::uint16_t> allowed_clients, unsigned max_per_second, std::int64_t confirm_timeout_ns)
	: allowed_(std::move(allowed_clients)), max_per_second_(max_per_second), confirm_timeout_ns_(confirm_timeout_ns)
{
}

Decision BodyControl::request(std::uint16_t method, std::uint16_t client, const std::vector<std::uint8_t> &payload,
			      std::int64_t now_ns, bool ignition_ok, lin::LinMaster &lin, std::uint64_t token)
{
	Decision d;
	auto reject = [&](std::uint8_t code, std::string event) {
		d.code = code;
		d.security_event = std::move(event);
		rejected_[code]++;
		return d;
	};

	if (!allowed_.count(client))
		return reject(kStateRejected, "body_control_client_not_allowed client=" + std::to_string(client));
	if (method != kSetWindowPosition && method != kSetInteriorLight)
		return reject(kInvalidArgument, "body_control_unknown_method");
	if (payload.size() != 1)
		return reject(kInvalidArgument, "body_control_invalid_length len=" + std::to_string(payload.size()));
	const std::uint8_t v = payload[0];
	if ((method == kSetWindowPosition && v > 100) || (method == kSetInteriorLight && v > 1))
		return reject(kInvalidArgument, "");

	// rate limit per method over a sliding 1 s window
	auto &q = recent_[method];
	while (!q.empty() && now_ns - q.front() >= kSecond)
		q.pop_front();
	if (q.size() >= max_per_second_)
		return reject(kStateRejected, "body_control_rate_limited method=" + std::to_string(method));
	q.push_back(now_ns);

	const lin::Slave slave = method == kSetWindowPosition ? lin::Slave::Window : lin::Slave::Light;
	if (method == kSetWindowPosition && !ignition_ok)
		return reject(kStateRejected, "");
	if (lin.health(slave).degraded)
		return reject(kLinTimeout, ""); // the slave is in communication error: no LIN command is created

	if (method == kSetWindowPosition)
		lin.command_window(v, now_ns);
	else
		lin.command_light(v == 1, now_ns);
	pending_.push_back({token, method, now_ns});
	d.pending = true;
	return d;
}

std::vector<Completion> BodyControl::poll(std::int64_t now_ns, const lin::LinMaster &lin)
{
	std::vector<Completion> done;
	for (auto it = pending_.begin(); it != pending_.end();) {
		const bool window = it->method == kSetWindowPosition;
		const std::int64_t tx = window ? lin.window_cmd_tx_ns() : lin.light_cmd_tx_ns();
		const std::int64_t st = lin.status_ns(window ? lin::Slave::Window : lin::Slave::Light);
		// confirmed: the command frame went out after the request and the slave answered after that frame
		if (tx >= it->issued_ns && st > tx) {
			done.push_back({it->token, kAccepted});
			it = pending_.erase(it);
		} else if (now_ns - it->issued_ns >= confirm_timeout_ns_) {
			rejected_[kLinTimeout]++;
			done.push_back({it->token, kLinTimeout});
			it = pending_.erase(it);
		} else {
			++it;
		}
	}
	return done;
}

std::uint64_t BodyControl::rejected(std::uint8_t code) const
{
	auto it = rejected_.find(code);
	return it == rejected_.end() ? 0 : it->second;
}

} // namespace sil::body
