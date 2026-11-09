// VehicleStateCache snapshot -> VehicleStateData payload (project.md §7.2).
#pragma once

#include <cstdint>

#include "sil_services_gen.hpp"
#include "vehicle_state_cache.hpp"

namespace sil::gateway {

inline services::VehicleStateData to_payload(const VehicleStateSnapshot &s, std::uint32_t sequence)
{
	services::VehicleStateData d;
	d.sequence = sequence;
	d.timestamp_us = static_cast<std::uint64_t>(s.source_rx_ns / 1000);
	d.rpm = s.rpm;
	d.speed = s.speed;
	d.throttle = s.throttle;
	d.coolant = s.coolant;
	d.brake_pressure = s.brake_pressure;
	d.abs_state = s.abs_active;
	d.validity = s.validity;
	d.freshness = s.freshness;
	d.gateway_state = static_cast<std::uint8_t>(s.state);
	d.reserved = 0;
	return d;
}

} // namespace sil::gateway
