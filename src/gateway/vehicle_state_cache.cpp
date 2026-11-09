#include "vehicle_state_cache.hpp"

#include <algorithm>

namespace sil::gateway {

namespace {

constexpr std::int64_t kNsPerMs = 1000000;

unsigned index_of(const sil_pdu_def_t *pdu)
{
	return static_cast<unsigned>(pdu - sil_pdus);
}

// payload signal -> (PDU, signal) source
struct Source {
	unsigned pdu;
	unsigned signal;
};

constexpr std::array<Source, kStateSignalCount> kSources = {{
	{SIL_PDU_PT_ENGINESTATUS, SIL_PT_ENGINESTATUS_RPM},
	{SIL_PDU_PT_VEHICLEDYNAMICS, SIL_PT_VEHICLEDYNAMICS_SPEED},
	{SIL_PDU_PT_ENGINESTATUS, SIL_PT_ENGINESTATUS_THROTTLE},
	{SIL_PDU_PT_ENGINESTATUS, SIL_PT_ENGINESTATUS_COOLANT},
	{SIL_PDU_CH_BRAKESTATUS, SIL_CH_BRAKESTATUS_PRESSURE},
	{SIL_PDU_CH_BRAKESTATUS, SIL_CH_BRAKESTATUS_ABS_ACTIVE},
}};

} // namespace

void VehicleStateCache::on_frame(sil_status_t status, const sil_pdu_frame_t &frame, std::int64_t now_ns)
{
	if (frame.pdu == nullptr || status == SIL_ERR_UNKNOWN_ID || status == SIL_ERR_FRAME_FORMAT) {
		unattributed_errors_++;
		return;
	}
	PduHealth &h = health_[index_of(frame.pdu)];
	if (status != SIL_OK) {
		// CRC/counter error: drop this PDU (§5.4); keep the last good value
		h.rejected++;
		if (h.consecutive_errors < UINT8_MAX)
			h.consecutive_errors++;
		return;
	}
	h.accepted++;
	h.consecutive_errors = 0;
	h.received = true;
	h.last_rx_ns = now_ns;
	h.last = frame;
}

Freshness VehicleStateCache::freshness(unsigned pdu_index, std::int64_t now_ns) const
{
	const PduHealth &h = health_[pdu_index];
	if (!h.received)
		return Freshness::Unknown;
	const std::int64_t period = sil_pdus[pdu_index].period_ms * kNsPerMs;
	const std::int64_t age = now_ns - h.last_rx_ns;
	if (age >= 5 * period)
		return Freshness::Stale;
	if (age >= 3 * period)
		return Freshness::Degraded;
	return Freshness::Fresh;
}

bool VehicleStateCache::pdu_degraded(unsigned pdu_index, std::int64_t now_ns) const
{
	return health_[pdu_index].consecutive_errors >= kErrorLimit || freshness(pdu_index, now_ns) != Freshness::Fresh;
}

GatewayState VehicleStateCache::state(std::int64_t now_ns) const
{
	if (shutdown_)
		return GatewayState::Shutdown;
	bool all_seen = true;
	bool any_degraded = false;
	for (unsigned i = 0; i < SIL_PDU_COUNT; i++) {
		all_seen = all_seen && health_[i].received;
		any_degraded = any_degraded || pdu_degraded(i, now_ns);
	}
	if (!all_seen)
		return GatewayState::Startup;
	return any_degraded ? GatewayState::Degraded : GatewayState::Running;
}

VehicleStateSnapshot VehicleStateCache::snapshot(std::int64_t now_ns) const
{
	VehicleStateSnapshot s;
	std::array<std::int32_t, kStateSignalCount> raw{};

	for (unsigned bit = 0; bit < kStateSignalCount; bit++) {
		const Source src = kSources[bit];
		const PduHealth &h = health_[src.pdu];
		if (!h.received)
			continue; // UNKNOWN: value 0, valid 0, fresh 0
		raw[bit] = h.last.raw[src.signal];
		s.source_rx_ns = std::max(s.source_rx_ns, h.last_rx_ns);
		// the last good value is still reported, but flagged (§5.4)
		if (h.consecutive_errors < kErrorLimit && !h.last.flags.fault)
			s.validity |= static_cast<std::uint16_t>(1u << bit);
		if (freshness(src.pdu, now_ns) == Freshness::Fresh)
			s.freshness |= static_cast<std::uint16_t>(1u << bit);
	}
	s.rpm = static_cast<std::uint16_t>(raw[kRpm]);
	s.speed = static_cast<std::uint16_t>(raw[kSpeed]);
	s.throttle = static_cast<std::uint8_t>(raw[kThrottle]);
	s.coolant = static_cast<std::int16_t>(raw[kCoolant]);
	s.brake_pressure = static_cast<std::uint16_t>(raw[kBrakePressure]);
	s.abs_active = static_cast<std::uint8_t>(raw[kAbsActive]);
	s.state = state(now_ns);
	return s;
}

} // namespace sil::gateway
