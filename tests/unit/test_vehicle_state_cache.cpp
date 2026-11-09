// State cache, validity/freshness and Gateway state tests (project.md §5.4, §7.2, §7.5).
#include "can_input_validator.hpp"
#include "sil_test.h"
#include "vehicle_state_cache.hpp"

using namespace sil::gateway;

namespace {

constexpr std::int64_t MS = 1000000;

sil_pdu_frame_t frame(unsigned pdu_index, std::int32_t first = 0, bool fault = false)
{
	sil_pdu_frame_t f{};
	f.pdu = &sil_pdus[pdu_index];
	f.raw[0] = first;
	f.flags.valid = true;
	f.flags.fault = fault;
	return f;
}

void feed_all(VehicleStateCache &c, std::int64_t t)
{
	for (unsigned i = 0; i < SIL_PDU_COUNT; i++)
		c.on_frame(SIL_OK, frame(i, 100), t);
}

void test_startup_until_every_pdu_seen()
{
	VehicleStateCache c;

	CHECK(c.state(0) == GatewayState::Startup);
	CHECK(c.freshness(SIL_PDU_PT_ENGINESTATUS, 0) == Freshness::Unknown);
	c.on_frame(SIL_OK, frame(SIL_PDU_PT_ENGINESTATUS), 0);
	CHECK(c.state(1 * MS) == GatewayState::Startup);
	feed_all(c, 2 * MS);
	CHECK(c.state(3 * MS) == GatewayState::Running);
	VehicleStateSnapshot s = c.snapshot(3 * MS);
	CHECK_EQ_INT(s.validity, 0x3F);
	CHECK_EQ_INT(s.freshness, 0x3F);
	CHECK_EQ_INT(s.source_rx_ns, 2 * MS);
}

void test_unknown_signals_report_zero_bits()
{
	VehicleStateCache c;

	c.on_frame(SIL_OK, frame(SIL_PDU_PT_ENGINESTATUS, 800), 0);
	VehicleStateSnapshot s = c.snapshot(1 * MS);
	CHECK_EQ_INT(s.rpm, 800);
	// rpm, throttle, coolant come from 0x100; speed and brake are still UNKNOWN
	CHECK_EQ_INT(s.validity, (1u << kRpm) | (1u << kThrottle) | (1u << kCoolant));
	CHECK_EQ_INT(s.freshness, (1u << kRpm) | (1u << kThrottle) | (1u << kCoolant));
	CHECK_EQ_INT(s.speed, 0);
}

// 3x period -> DEGRADED, 5x period -> STALE; last value kept but freshness bit cleared
void test_freshness_thresholds()
{
	VehicleStateCache c;
	const unsigned eng = SIL_PDU_PT_ENGINESTATUS; // 10 ms
	const unsigned dyn = SIL_PDU_PT_VEHICLEDYNAMICS; // 20 ms

	feed_all(c, 0);
	CHECK(c.freshness(eng, 29 * MS) == Freshness::Fresh);
	CHECK(c.freshness(eng, 30 * MS) == Freshness::Degraded);
	CHECK(c.freshness(eng, 49 * MS) == Freshness::Degraded);
	CHECK(c.freshness(eng, 50 * MS) == Freshness::Stale);
	CHECK(c.freshness(dyn, 59 * MS) == Freshness::Fresh);
	CHECK(c.freshness(dyn, 60 * MS) == Freshness::Degraded);
	CHECK(c.freshness(dyn, 100 * MS) == Freshness::Stale);

	VehicleStateSnapshot s = c.snapshot(50 * MS);
	CHECK_EQ_INT(s.rpm, 100);                     // last good value still shown
	CHECK_EQ_INT(s.freshness & (1u << kRpm), 0);  // but marked not fresh
	CHECK(s.state == GatewayState::Degraded);

	feed_all(c, 60 * MS);
	CHECK(c.state(61 * MS) == GatewayState::Running);
}

// 3 consecutive errors -> PDU degraded and validity cleared; one good frame recovers
void test_consecutive_errors()
{
	VehicleStateCache c;
	const unsigned brk = SIL_PDU_CH_BRAKESTATUS;

	feed_all(c, 0);
	c.on_frame(SIL_ERR_CRC, frame(brk), 1 * MS);
	c.on_frame(SIL_ERR_COUNTER_JUMP, frame(brk), 2 * MS);
	CHECK(!c.pdu_degraded(brk, 3 * MS));
	CHECK(c.state(3 * MS) == GatewayState::Running);
	c.on_frame(SIL_ERR_RANGE, frame(brk), 3 * MS);
	CHECK(c.pdu_degraded(brk, 4 * MS));
	CHECK(c.state(4 * MS) == GatewayState::Degraded);
	VehicleStateSnapshot s = c.snapshot(4 * MS);
	CHECK_EQ_INT(s.validity & (1u << kBrakePressure), 0);
	CHECK_EQ_INT(s.validity & (1u << kAbsActive), 0);
	CHECK_EQ_INT(s.validity & (1u << kRpm), 1u << kRpm);
	CHECK_EQ_INT(c.health(brk).rejected, 3);

	c.on_frame(SIL_OK, frame(brk, 250), 5 * MS);
	CHECK(!c.pdu_degraded(brk, 6 * MS));
	CHECK_EQ_INT(c.snapshot(6 * MS).brake_pressure, 250);
}

// a rejected frame never replaces the last good value
void test_rejected_frame_keeps_last_value()
{
	VehicleStateCache c;

	c.on_frame(SIL_OK, frame(SIL_PDU_PT_ENGINESTATUS, 800), 0);
	c.on_frame(SIL_ERR_RANGE, frame(SIL_PDU_PT_ENGINESTATUS, 9000), 1 * MS);
	CHECK_EQ_INT(c.snapshot(2 * MS).rpm, 800);
	CHECK_EQ_INT(c.health(SIL_PDU_PT_ENGINESTATUS).last_rx_ns, 0);
}

void test_fault_flag_clears_validity()
{
	VehicleStateCache c;

	feed_all(c, 0);
	c.on_frame(SIL_OK, frame(SIL_PDU_PT_ENGINESTATUS, 800, true), 1 * MS);
	VehicleStateSnapshot s = c.snapshot(2 * MS);
	CHECK_EQ_INT(s.validity & (1u << kRpm), 0);
	CHECK_EQ_INT(s.freshness & (1u << kRpm), 1u << kRpm);
}

void test_unattributed_and_shutdown()
{
	VehicleStateCache c;
	sil_pdu_frame_t none{};

	c.on_frame(SIL_ERR_UNKNOWN_ID, none, 0);
	c.on_frame(SIL_ERR_FRAME_FORMAT, none, 0);
	CHECK_EQ_INT(c.unattributed_errors(), 2);
	c.shutdown();
	CHECK(c.state(0) == GatewayState::Shutdown);
}

// validator + cache together on encoded frames
void test_validator_into_cache()
{
	CanInputValidator v;
	VehicleStateCache c;
	std::int32_t raw[SIL_MAX_SIGNALS] = {};
	can_frame fr{};
	sil_pdu_frame_t out{};

	raw[SIL_PT_ENGINESTATUS_RPM] = 2500;
	raw[SIL_PT_ENGINESTATUS_COOLANT] = 900;
	fr.can_id = SIL_PT_ENGINESTATUS_ID;
	fr.len = 8;
	CHECK_EQ_INT(sil_pdu_encode(&sil_pdus[SIL_PDU_PT_ENGINESTATUS], raw, 0, sil_flags_t{true, false, false}, fr.data), SIL_OK);
	c.on_frame(v.validate(fr, out), out, 0);
	VehicleStateSnapshot s = c.snapshot(1 * MS);
	CHECK_EQ_INT(s.rpm, 2500);
	CHECK_EQ_INT(s.coolant, 900);
}

} // namespace

int main()
{
	test_startup_until_every_pdu_seen();
	test_unknown_signals_report_zero_bits();
	test_freshness_thresholds();
	test_consecutive_errors();
	test_rejected_frame_keeps_last_value();
	test_fault_flag_clears_validity();
	test_unattributed_and_shutdown();
	test_validator_into_cache();
	return TEST_REPORT();
}
