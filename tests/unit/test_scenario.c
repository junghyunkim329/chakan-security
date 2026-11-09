/* The normal scenario must never produce a value the ECUs would refuse to encode. */
#include "sil/pdu_codec.h"
#include "sil/scenario.h"
#include "sil_test.h"

static void in_range(unsigned pdu, unsigned sig, double phys)
{
	const sil_sig_def_t *s = &sil_pdus[pdu].signals[sig];
	int32_t raw = sil_phys_to_raw(s, phys);

	if (raw < s->raw_min || raw > s->raw_max) {
		fprintf(stderr, "%s.%s = %f out of range\n", sil_pdus[pdu].name, s->name, phys);
		CHECK(0);
	}
}

int main(void)
{
	sil_vehicle_state_t st;
	double max_speed = 0.0;

	/* two hours of scenario time at 1 ms resolution covers coolant warm-up and many cycles */
	for (long ms = 0; ms <= 2L * 3600 * 1000; ms++) {
		sil_scenario_eval((double)ms / 1000.0, &st);
		in_range(SIL_PDU_PT_ENGINESTATUS, SIL_PT_ENGINESTATUS_RPM, st.rpm);
		in_range(SIL_PDU_PT_ENGINESTATUS, SIL_PT_ENGINESTATUS_THROTTLE, st.throttle_pct);
		in_range(SIL_PDU_PT_ENGINESTATUS, SIL_PT_ENGINESTATUS_COOLANT, st.coolant_c);
		in_range(SIL_PDU_PT_VEHICLEDYNAMICS, SIL_PT_VEHICLEDYNAMICS_SPEED, st.speed_kmh);
		in_range(SIL_PDU_PT_VEHICLEDYNAMICS, SIL_PT_VEHICLEDYNAMICS_LONG_ACCEL, st.long_accel_mps2);
		in_range(SIL_PDU_PT_VEHICLEDYNAMICS, SIL_PT_VEHICLEDYNAMICS_DIRECTION, st.direction);
		in_range(SIL_PDU_CH_BRAKESTATUS, SIL_CH_BRAKESTATUS_PRESSURE, st.brake_pressure_bar);
		in_range(SIL_PDU_CH_BRAKESTATUS, SIL_CH_BRAKESTATUS_PEDAL, st.brake_pedal_pct);
		in_range(SIL_PDU_CH_STEERINGSTATUS, SIL_CH_STEERINGSTATUS_ANGLE, st.steer_angle_deg);
		in_range(SIL_PDU_CH_STEERINGSTATUS, SIL_CH_STEERINGSTATUS_ANGLE_RATE, st.steer_rate_dps);
		in_range(SIL_PDU_CH_STEERINGSTATUS, SIL_CH_STEERINGSTATUS_ASSIST_TORQUE, st.assist_nm);
		if (st.speed_kmh > max_speed)
			max_speed = st.speed_kmh;
	}

	/* scenario shape */
	sil_scenario_eval(0.0, &st);
	CHECK(st.speed_kmh == 0.0 && st.brake_switch && st.direction == 0);
	sil_scenario_eval(18.0, &st);
	CHECK(st.speed_kmh == 60.0 && !st.brake_switch && st.direction == 1);
	sil_scenario_eval(25.0, &st);
	CHECK(st.long_accel_mps2 < 0.0 && st.brake_switch);
	CHECK(max_speed == 60.0);
	/* deterministic */
	{
		sil_vehicle_state_t a, b;

		sil_scenario_eval(12.345, &a);
		sil_scenario_eval(12.345, &b);
		CHECK(a.speed_kmh == b.speed_kmh && a.steer_angle_deg == b.steer_angle_deg);
	}
	return TEST_REPORT();
}
