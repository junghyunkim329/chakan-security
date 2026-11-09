/* Steering ECU (EPS): 0x200 CH_SteeringStatus every 10 ms. */
#include "sil/ecu_runtime.h"

static sil_flags_t fill_steering_status(const sil_vehicle_state_t *st, const sil_pdu_def_t *pdu, int32_t *raw)
{
	raw[SIL_CH_STEERINGSTATUS_ANGLE] = sil_raw(pdu, SIL_CH_STEERINGSTATUS_ANGLE, st->steer_angle_deg);
	raw[SIL_CH_STEERINGSTATUS_ANGLE_RATE] = sil_raw(pdu, SIL_CH_STEERINGSTATUS_ANGLE_RATE, st->steer_rate_dps);
	raw[SIL_CH_STEERINGSTATUS_ASSIST_TORQUE] = sil_raw(pdu, SIL_CH_STEERINGSTATUS_ASSIST_TORQUE, st->assist_nm);
	raw[SIL_CH_STEERINGSTATUS_EPS_ACTIVE] = st->eps_active;
	raw[SIL_CH_STEERINGSTATUS_FN_DEGRADED] = 0;
	raw[SIL_CH_STEERINGSTATUS_FN_FAULT] = 0;
	return (sil_flags_t){.valid = true};
}

int main(int argc, char **argv)
{
	static const sil_ecu_tx_t tx[] = {
		{SIL_PDU_CH_STEERINGSTATUS, fill_steering_status},
	};

	return sil_ecu_main(argc, argv, "steering", tx, sizeof(tx) / sizeof(tx[0]), NULL);
}
