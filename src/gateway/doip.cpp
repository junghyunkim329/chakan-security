#include "doip.hpp"

#include <cstring>
#include <string>

namespace sil::doip {

namespace {

constexpr std::uint8_t kVersion = SIL_DOIP_PROTOCOL_VERSION;
constexpr std::uint8_t kVersionDefault = 0xFF; // allowed for vehicle identification requests
constexpr std::size_t kMaxPayload = 4 + SIL_DOIP_MAX_UDS_BYTES; // SA + TA + UDS

void put16(Bytes &b, std::uint16_t v)
{
	b.push_back(static_cast<std::uint8_t>(v >> 8));
	b.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

Bytes nack(std::uint8_t code)
{
	return message(kGenericNack, Bytes{code});
}

Event send(Bytes b)
{
	return Event{Event::Kind::SendToTester, std::move(b), 0};
}

Event log(const std::string &s)
{
	return Event{Event::Kind::Log, Bytes(s.begin(), s.end()), 0};
}

Event close_event()
{
	return Event{Event::Kind::Close, {}, 0};
}

} // namespace

std::uint16_t be16(const std::uint8_t *p)
{
	return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

Bytes message(std::uint16_t type, const Bytes &payload)
{
	Bytes b{kVersion, static_cast<std::uint8_t>(~kVersion)};
	put16(b, type);
	const auto n = static_cast<std::uint32_t>(payload.size());
	b.push_back(static_cast<std::uint8_t>(n >> 24));
	b.push_back(static_cast<std::uint8_t>(n >> 16));
	b.push_back(static_cast<std::uint8_t>(n >> 8));
	b.push_back(static_cast<std::uint8_t>(n));
	b.insert(b.end(), payload.begin(), payload.end());
	return b;
}

Session::Session(std::set<std::uint16_t> ecu_addresses) : ecus_(std::move(ecu_addresses)) {}

std::vector<Event> Session::feed(const std::uint8_t *data, std::size_t len)
{
	std::vector<Event> out;
	if (closed_)
		return out;
	buffer_.insert(buffer_.end(), data, data + len);

	while (buffer_.size() >= kHeaderSize && !closed_) {
		const std::uint8_t *h = buffer_.data();
		const std::uint16_t type = be16(h + 2);
		const std::uint32_t plen = (std::uint32_t(h[4]) << 24) | (std::uint32_t(h[5]) << 16) | (std::uint32_t(h[6]) << 8) | h[7];

		// ISO 13400-2 §6.1.2: wrong version/inverse -> NACK and close (the stream cannot be resynchronised)
		if (h[0] != kVersion || h[1] != static_cast<std::uint8_t>(~h[0])) {
			out.push_back(send(nack(kNackIncorrectPattern)));
			out.push_back(log("doip_incorrect_pattern"));
			out.push_back(close_event());
			closed_ = true;
			break;
		}
		if (plen > kMaxPayload) {
			out.push_back(send(nack(kNackMessageTooLarge)));
			out.push_back(log("doip_message_too_large"));
			out.push_back(close_event());
			closed_ = true;
			break;
		}
		if (buffer_.size() < kHeaderSize + plen)
			break; // wait for the rest
		auto events = handle(type, h + kHeaderSize, plen);
		out.insert(out.end(), events.begin(), events.end());
		buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(kHeaderSize + plen));
	}
	return out;
}

std::vector<Event> Session::handle(std::uint16_t type, const std::uint8_t *p, std::size_t len)
{
	std::vector<Event> out;

	switch (type) {
	case kRoutingActivationRequest: {
		if (len != 7 && len != 11) {
			out = {send(nack(kNackInvalidPayloadLength)), log("doip_invalid_length routing_activation"), close_event()};
			closed_ = true;
			return out;
		}
		const std::uint16_t sa = be16(p);
		std::uint8_t code = kRaSuccess;
		if (sa != SIL_DOIP_TESTER_ADDRESS)
			code = kRaDeniedUnknownSourceAddress;
		else if (p[2] != 0x00) // default activation only
			code = kRaDeniedUnsupportedType;
		Bytes r;
		put16(r, sa);
		put16(r, SIL_DOIP_GATEWAY_ADDRESS);
		r.push_back(code);
		r.insert(r.end(), {0, 0, 0, 0});
		out.push_back(send(message(kRoutingActivationResponse, r)));
		if (code == kRaSuccess) {
			routing_active_ = true;
			out.push_back(log("doip_routing_activated"));
		} else {
			out.push_back(log("doip_routing_denied code=" + std::to_string(code) + " sa=" + std::to_string(sa)));
			out.push_back(close_event());
			closed_ = true;
		}
		return out;
	}
	case kDiagnosticMessage: {
		if (len < 5) {
			out = {send(nack(kNackInvalidPayloadLength)), log("doip_invalid_length diagnostic"), close_event()};
			closed_ = true;
			return out;
		}
		const std::uint16_t sa = be16(p), ta = be16(p + 2);
		if (!routing_active_ || sa != SIL_DOIP_TESTER_ADDRESS) {
			// §9.1: only the activated tester may reach ECUs; ISO 13400-2 closes the socket
			out = {send(diagnostic_nack(ta, kDiagNackInvalidSourceAddress)),
			       log("doip_invalid_source_address sa=" + std::to_string(sa)), close_event()};
			closed_ = true;
			return out;
		}
		if (!ecus_.count(ta)) {
			out = {send(diagnostic_nack(ta, kDiagNackUnknownTargetAddress)), log("doip_unknown_target ta=" + std::to_string(ta))};
			return out;
		}
		out.push_back(Event{Event::Kind::Forward, Bytes(p + 4, p + len), ta});
		return out;
	}
	case kAliveCheckResponse:
		return out;
	default:
		out = {send(nack(kNackUnknownPayloadType)), log("doip_unknown_payload_type type=" + std::to_string(type))};
		return out;
	}
}

Bytes Session::diagnostic_ack(std::uint16_t target) const
{
	Bytes r;
	put16(r, target);
	put16(r, SIL_DOIP_TESTER_ADDRESS);
	r.push_back(0x00);
	return message(kDiagnosticAck, r);
}

Bytes Session::diagnostic_nack(std::uint16_t target, std::uint8_t code) const
{
	Bytes r;
	put16(r, target);
	put16(r, SIL_DOIP_TESTER_ADDRESS);
	r.push_back(code);
	return message(kDiagnosticNack, r);
}

Bytes Session::diagnostic_response(std::uint16_t source_ecu, const std::uint8_t *uds, std::size_t len) const
{
	Bytes r;
	put16(r, source_ecu);
	put16(r, SIL_DOIP_TESTER_ADDRESS);
	r.insert(r.end(), uds, uds + len);
	return message(kDiagnosticMessage, r);
}

std::optional<Bytes> handle_udp(const std::uint8_t *data, std::size_t len)
{
	static const std::uint8_t eid[6] = SIL_DOIP_EID;
	static const std::uint8_t gid[6] = SIL_DOIP_GID;

	if (len < kHeaderSize || (data[0] != kVersion && data[0] != kVersionDefault) ||
	    data[1] != static_cast<std::uint8_t>(~data[0]))
		return nack(kNackIncorrectPattern);
	const std::uint16_t type = be16(data + 2);
	const std::uint32_t plen = (std::uint32_t(data[4]) << 24) | (std::uint32_t(data[5]) << 16) | (std::uint32_t(data[6]) << 8) | data[7];
	if (plen != len - kHeaderSize)
		return nack(kNackInvalidPayloadLength);
	const std::uint8_t *p = data + kHeaderSize;

	bool match = false;
	if (type == kVehicleIdRequest && plen == 0)
		match = true;
	else if (type == kVehicleIdRequestEid && plen == 6)
		match = std::memcmp(p, eid, 6) == 0;
	else if (type == kVehicleIdRequestVin && plen == 17)
		match = std::memcmp(p, SIL_DOIP_VIN, 17) == 0;
	else if (type == kVehicleIdRequest || type == kVehicleIdRequestEid || type == kVehicleIdRequestVin)
		return nack(kNackInvalidPayloadLength);
	else
		return nack(kNackUnknownPayloadType);
	if (!match)
		return std::nullopt; // ISO 13400-2: do not answer a request for another vehicle

	Bytes r(SIL_DOIP_VIN, SIL_DOIP_VIN + 17);
	put16(r, SIL_DOIP_GATEWAY_ADDRESS);
	r.insert(r.end(), eid, eid + 6);
	r.insert(r.end(), gid, gid + 6);
	r.push_back(0x00); // further action: none
	return message(kVehicleAnnouncement, r);
}

} // namespace sil::doip
