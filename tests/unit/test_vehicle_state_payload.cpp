// VehicleStateData serializer tests: golden bytes, round trip, consumer-side rejection (project.md §7.2).
#include <cstring>

#include "sil_services_gen.hpp"
#include "sil_test.h"
#include "vehicle_state_payload.hpp"

using namespace sil;

namespace {

void test_golden_bytes()
{
	auto b = services::serialize(services::golden_VehicleStateData());

	CHECK(b == services::kVehicleStateDataGoldenBytes);
	CHECK_EQ_INT(b.size(), 28);
	// spot-check the documented layout independently of the generator
	CHECK_EQ_INT(b[0], 0x01);
	CHECK_EQ_INT(b[3], 0x04);
	CHECK_EQ_INT(b[4], 0x11);
	CHECK_EQ_INT(b[11], 0x88);
	CHECK_EQ_INT(b[12], 0x09); // 2500 = 0x09C4
	CHECK_EQ_INT(b[13], 0xC4);
	CHECK_EQ_INT(b[17], 0xFE); // -400 = 0xFE70
	CHECK_EQ_INT(b[18], 0x70);
	CHECK_EQ_INT(b[22], 0x00);
	CHECK_EQ_INT(b[23], 0x3F);
	CHECK_EQ_INT(b[26], 0x01);
	CHECK_EQ_INT(b[27], 0x00);
}

void test_round_trip()
{
	services::VehicleStateData in = services::golden_VehicleStateData(), out;
	auto b = services::serialize(in);

	CHECK(services::deserialize(b.data(), b.size(), out));
	CHECK_EQ_INT(out.sequence, in.sequence);
	CHECK(out.timestamp_us == in.timestamp_us);
	CHECK_EQ_INT(out.coolant, -400);
	CHECK_EQ_INT(out.validity, 0x3F);
	CHECK_EQ_INT(out.freshness, 0x1E);
}

// §7.2: consumer must not update state when length != 28 or reserved != 0
void test_consumer_rejects_malformed()
{
	services::VehicleStateData out;
	auto b = services::serialize(services::golden_VehicleStateData());

	out.rpm = 1234;
	CHECK(!services::deserialize(b.data(), 27, out));
	CHECK(!services::deserialize(b.data(), 0, out));
	CHECK(!services::deserialize(nullptr, 28, out));
	std::uint8_t longer[29] = {};
	std::memcpy(longer, b.data(), 28);
	CHECK(!services::deserialize(longer, 29, out));
	b[27] = 0x01;
	CHECK(!services::deserialize(b.data(), b.size(), out));
	CHECK_EQ_INT(out.rpm, 1234); // unchanged
}

void test_snapshot_to_payload()
{
	gateway::VehicleStateSnapshot s;
	s.source_rx_ns = 123456789;
	s.rpm = 800;
	s.coolant = -10;
	s.validity = 0x3F;
	s.freshness = 0x3F;
	s.state = gateway::GatewayState::Degraded;
	services::VehicleStateData d = gateway::to_payload(s, 7);

	CHECK_EQ_INT(d.sequence, 7);
	CHECK(d.timestamp_us == 123456);
	CHECK_EQ_INT(d.rpm, 800);
	CHECK_EQ_INT(d.coolant, -10);
	CHECK_EQ_INT(d.gateway_state, 2);
	CHECK_EQ_INT(d.reserved, 0);
}

void test_service_constants()
{
	CHECK_EQ_INT(services::VehicleState::kServiceId, 0x1000);
	CHECK_EQ_INT(services::VehicleState::kInstanceId, 0x0001);
	CHECK_EQ_INT(services::VehicleState::kGetSnapshotMethodId, 0x0001);
	CHECK_EQ_INT(services::VehicleState::kVehicleStateChangedEventId, 0x8001);
	CHECK_EQ_INT(services::VehicleState::kVehicleStateChangedEventgroupId, 0x0001);
	CHECK_EQ_INT(services::VehicleState::kVehicleStateChangedCycleMs, 20);
	CHECK_EQ_INT(services::VehicleState::kUnreliablePort, 30501);
	CHECK_EQ_INT(services::VehicleState::kReliablePort, 30502);
}

} // namespace

int main()
{
	test_golden_bytes();
	test_round_trip();
	test_consumer_rejects_malformed();
	test_snapshot_to_payload();
	test_service_constants();
	return TEST_REPORT();
}
