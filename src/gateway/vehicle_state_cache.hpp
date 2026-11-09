// Latest validated vehicle state with validity and freshness (project.md §5.4, §7.2).
#pragma once

#include <array>
#include <cstdint>

#include "sil/pdu_codec.h"

namespace sil::gateway {

enum class Freshness : std::uint8_t { Unknown, Fresh, Degraded, Stale };
enum class GatewayState : std::uint8_t { Startup = 0, Running = 1, Degraded = 2, Shutdown = 3 };

// Bit positions in the VehicleState validity/freshness bitmaps (§7.2, decision in §7.5)
enum StateSignal : std::uint8_t {
	kRpm = 0,
	kSpeed = 1,
	kThrottle = 2,
	kCoolant = 3,
	kBrakePressure = 4,
	kAbsActive = 5,
	kStateSignalCount
};

struct PduHealth {
	bool received = false;           // at least one accepted frame
	std::int64_t last_rx_ns = 0;     // monotonic time of last accepted frame
	std::uint8_t consecutive_errors = 0;
	std::uint64_t accepted = 0;
	std::uint64_t rejected = 0;
	sil_pdu_frame_t last{};          // last accepted frame
};

struct VehicleStateSnapshot {
	std::int64_t source_rx_ns = 0;   // monotonic rx time of the newest accepted frame used here
	std::uint16_t rpm = 0;           // raw units as on CAN (1 rpm/bit)
	std::uint16_t speed = 0;         // 0.01 km/h/bit
	std::uint8_t throttle = 0;       // 0.5 %/bit
	std::int16_t coolant = 0;        // 0.1 degC/bit
	std::uint16_t brake_pressure = 0; // 0.1 bar/bit
	std::uint8_t abs_active = 0;
	std::uint16_t validity = 0;
	std::uint16_t freshness = 0;
	GatewayState state = GatewayState::Startup;
};

class VehicleStateCache {
public:
	static constexpr std::uint8_t kErrorLimit = 3; // §5.4: 3 consecutive errors -> DEGRADED

	// result of CanInputValidator for a frame received at now_ns.
	// unknown IDs and frame format errors cannot be attributed to a PDU and are only counted.
	void on_frame(sil_status_t status, const sil_pdu_frame_t &frame, std::int64_t now_ns);

	Freshness freshness(unsigned pdu_index, std::int64_t now_ns) const;
	bool pdu_degraded(unsigned pdu_index, std::int64_t now_ns) const;
	GatewayState state(std::int64_t now_ns) const;
	VehicleStateSnapshot snapshot(std::int64_t now_ns) const;
	void shutdown() { shutdown_ = true; }

	const PduHealth &health(unsigned pdu_index) const { return health_[pdu_index]; }
	std::uint64_t unattributed_errors() const { return unattributed_errors_; }

private:
	std::array<PduHealth, SIL_PDU_COUNT> health_{};
	std::uint64_t unattributed_errors_ = 0;
	bool shutdown_ = false;
};

} // namespace sil::gateway
