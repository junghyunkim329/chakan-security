/* Brake ECU (ABS/ESC): 0x180 CH_BrakeStatus every 10 ms, 0x110 PT_VehicleDynamics every 20 ms. */
#include "sil/ecu_runtime.h"

static sil_flags_t fill_brake_status(const sil_vehicle_state_t *st, const sil_pdu_def_t *pdu, int32_t *raw)
{
	raw[SIL_CH_BRAKESTATUS_PRESSURE] = sil_raw(pdu, SIL_CH_BRAKESTATUS_PRESSURE, st->brake_pressure_bar);
	raw[SIL_CH_BRAKESTATUS_PEDAL] = sil_raw(pdu, SIL_CH_BRAKESTATUS_PEDAL, st->brake_pedal_pct);
	raw[SIL_CH_BRAKESTATUS_BRAKE_SWITCH] = st->brake_switch;
	raw[SIL_CH_BRAKESTATUS_ABS_ACTIVE] = 0;
	raw[SIL_CH_BRAKESTATUS_FN_DEGRADED] = 0;
	raw[SIL_CH_BRAKESTATUS_FN_FAULT] = 0;
	return (sil_flags_t){.valid = true};
}

static sil_flags_t fill_vehicle_dynamics(const sil_vehicle_state_t *st, const sil_pdu_def_t *pdu, int32_t *raw)
{
	raw[SIL_PT_VEHICLEDYNAMICS_SPEED] = sil_raw(pdu, SIL_PT_VEHICLEDYNAMICS_SPEED, st->speed_kmh);
	raw[SIL_PT_VEHICLEDYNAMICS_LONG_ACCEL] = sil_raw(pdu, SIL_PT_VEHICLEDYNAMICS_LONG_ACCEL, st->long_accel_mps2);
	raw[SIL_PT_VEHICLEDYNAMICS_DIRECTION] = st->direction;
	raw[SIL_PT_VEHICLEDYNAMICS_FN_DEGRADED] = 0;
	raw[SIL_PT_VEHICLEDYNAMICS_FN_FAULT] = 0;
	return (sil_flags_t){.valid = true};
}

/* DID values use the same raw units as the CAN signals (config/diagnostics/uds.yaml) */
static int read_did(void *ctx, uint16_t did, uint8_t *out, size_t cap)
{
	const sil_vehicle_state_t *st = ctx;
	int32_t raw;

	if (did == SIL_DID_ABS_STATE) {
		if (cap < 1)
			return -1;
		out[0] = 0; /* ABS never active in the normal scenario */
		return 1;
	}
	if (cap < 2)
		return -1;
	if (did == SIL_DID_VEHICLE_SPEED)
		raw = sil_raw(&sil_pdus[SIL_PDU_PT_VEHICLEDYNAMICS], SIL_PT_VEHICLEDYNAMICS_SPEED, st->speed_kmh);
	else if (did == SIL_DID_BRAKE_PRESSURE)
		raw = sil_raw(&sil_pdus[SIL_PDU_CH_BRAKESTATUS], SIL_CH_BRAKESTATUS_PRESSURE, st->brake_pressure_bar);
	else
		return -1;
	out[0] = (uint8_t)((uint32_t)raw >> 8);
	out[1] = (uint8_t)((uint32_t)raw & 0xFFu);
	return 2;
}

int main(int argc, char **argv)
{
	static const sil_ecu_tx_t tx[] = {
		{SIL_PDU_CH_BRAKESTATUS, fill_brake_status},
		{SIL_PDU_PT_VEHICLEDYNAMICS, fill_vehicle_dynamics},
	};
	static const sil_ecu_diag_t diag = {SIL_DIAG_ECU_BRAKE, read_did};

	return sil_ecu_main(argc, argv, "brake", tx, sizeof(tx) / sizeof(tx[0]), &diag);
}
