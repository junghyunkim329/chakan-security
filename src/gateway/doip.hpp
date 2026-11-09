// DoIP (ISO 13400-2) message handling for the Vehicle Gateway, project.md §9.1.
// Pure: parses the TCP byte stream and decides what to answer or forward; sockets live in doip_main.cpp.
#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <vector>

#include "sil_diag_gen.h"

namespace sil::doip {

using Bytes = std::vector<std::uint8_t>;

enum PayloadType : std::uint16_t {
	kGenericNack = 0x0000,
	kVehicleIdRequest = 0x0001,
	kVehicleIdRequestEid = 0x0002,
	kVehicleIdRequestVin = 0x0003,
	kVehicleAnnouncement = 0x0004,
	kRoutingActivationRequest = 0x0005,
	kRoutingActivationResponse = 0x0006,
	kAliveCheckRequest = 0x0007,
	kAliveCheckResponse = 0x0008,
	kDiagnosticMessage = 0x8001,
	kDiagnosticAck = 0x8002,
	kDiagnosticNack = 0x8003,
};

// Generic header NACK codes (ISO 13400-2 Table 19)
enum : std::uint8_t {
	kNackIncorrectPattern = 0x00,
	kNackUnknownPayloadType = 0x01,
	kNackMessageTooLarge = 0x02,
	kNackInvalidPayloadLength = 0x04,
};

// Diagnostic message NACK codes (Table 30)
enum : std::uint8_t {
	kDiagNackInvalidSourceAddress = 0x02,
	kDiagNackUnknownTargetAddress = 0x03,
	kDiagNackMessageTooLarge = 0x04,
	kDiagNackTargetUnreachable = 0x06,
	kDiagNackTransportProtocolError = 0x08,
};

// Routing activation response codes (Table 25)
enum : std::uint8_t {
	kRaDeniedUnknownSourceAddress = 0x00,
	kRaDeniedUnsupportedType = 0x06,
	kRaSuccess = 0x10,
};

constexpr std::size_t kHeaderSize = 8;

Bytes message(std::uint16_t type, const Bytes &payload);

struct Event {
	enum class Kind { SendToTester, Forward, Close, Log } kind;
	Bytes data;               // SendToTester: full DoIP message; Forward: UDS request; Log: text
	std::uint16_t target = 0; // Forward
};

// One TCP connection from a tester.
class Session {
public:
	explicit Session(std::set<std::uint16_t> ecu_addresses);

	// Appends bytes from the socket and handles every complete message.
	std::vector<Event> feed(const std::uint8_t *data, std::size_t len);

	// After a Forward event: positive ack (forwarded) or diagnostic NACK with the given code.
	Bytes diagnostic_ack(std::uint16_t target) const;
	Bytes diagnostic_nack(std::uint16_t target, std::uint8_t code) const;
	// UDS response from an ECU, wrapped for the tester.
	Bytes diagnostic_response(std::uint16_t source_ecu, const std::uint8_t *uds, std::size_t len) const;

	bool routing_active() const { return routing_active_; }

private:
	std::vector<Event> handle(std::uint16_t type, const std::uint8_t *p, std::size_t len);

	std::set<std::uint16_t> ecus_;
	Bytes buffer_;
	bool routing_active_ = false;
	bool closed_ = false;
};

// UDP vehicle identification (request -> announcement payload). Returns nothing for messages to ignore.
std::optional<Bytes> handle_udp(const std::uint8_t *data, std::size_t len);

std::uint16_t be16(const std::uint8_t *p);

} // namespace sil::doip
