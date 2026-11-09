#include "sil/scenario.h"

#include <math.h>

/*
 * 30 s cycle:
 *   0~5   standstill, brake held
 *   5~15  accelerate 0 -> 60 km/h
 *   15~22 cruise 60 km/h
 *   22~28 brake 60 -> 0 km/h
 *   28~30 standstill, brake held
 */
void sil_scenario_eval(double t_s, sil_vehicle_state_t *out)
{
	const double pi = 3.14159265358979323846;
	double tc = fmod(t_s, SIL_SCENARIO_CYCLE_S);
	double w = 2.0 * pi / 6.0; /* steering weave, 6 s period */

	*out = (sil_vehicle_state_t){0};
	out->coolant_c = fmin(90.0, 20.0 + 0.5 * t_s);

	if (tc < 5.0 || tc >= 28.0) {
		out->rpm = 800.0;
		out->brake_pressure_bar = 20.0;
		out->brake_pedal_pct = 20.0;
		out->brake_switch = true;
	} else if (tc < 15.0) {
		out->speed_kmh = 6.0 * (tc - 5.0);
		out->long_accel_mps2 = 60.0 / 3.6 / 10.0;
		out->throttle_pct = 30.0;
		out->rpm = 800.0 + out->speed_kmh / 60.0 * 1700.0;
	} else if (tc < 22.0) {
		out->speed_kmh = 60.0;
		out->throttle_pct = 15.0;
		out->rpm = 2000.0;
	} else {
		out->speed_kmh = 60.0 - 10.0 * (tc - 22.0);
		out->long_accel_mps2 = -60.0 / 3.6 / 6.0;
		out->rpm = 800.0 + out->speed_kmh / 60.0 * 1200.0;
		out->brake_pressure_bar = 30.0;
		out->brake_pedal_pct = 35.0;
		out->brake_switch = true;
	}

	if (out->speed_kmh > 0.0) {
		out->direction = 1;
		out->eps_active = true;
		out->steer_angle_deg = 15.0 * sin(w * tc);
		out->steer_rate_dps = 15.0 * w * cos(w * tc);
		out->assist_nm = 0.1 * out->steer_angle_deg;
	}
}
