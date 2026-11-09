/* Deterministic normal-driving scenario shared by ECU processes (project selection, not plant model). */
#ifndef SIL_SCENARIO_H
#define SIL_SCENARIO_H

#include <stdbool.h>

#define SIL_SCENARIO_CYCLE_S 30.0

typedef struct {
	double speed_kmh;
	double long_accel_mps2;
	int direction; /* 0=standstill 1=forward 2=reverse */
	double rpm;
	double throttle_pct;
	double coolant_c;
	double brake_pressure_bar;
	double brake_pedal_pct;
	bool brake_switch;
	double steer_angle_deg;
	double steer_rate_dps;
	double assist_nm;
	bool eps_active;
} sil_vehicle_state_t;

/* t_s: seconds since the ECU process started (monotonic). */
void sil_scenario_eval(double t_s, sil_vehicle_state_t *out);

#endif
