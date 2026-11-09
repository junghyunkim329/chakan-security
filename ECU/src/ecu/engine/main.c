/* Engine ECU: 0x100 PT_EngineStatus every 10 ms. */
#include "sil/ecu_runtime.h"

static sil_flags_t fill_engine_status(const sil_vehicle_state_t *st, const sil_pdu_def_t *pdu, int32_t *raw)
{
	raw[SIL_PT_ENGINESTATUS_RPM] = sil_raw(pdu, SIL_PT_ENGINESTATUS_RPM, st->rpm);
	raw[SIL_PT_ENGINESTATUS_THROTTLE] = sil_raw(pdu, SIL_PT_ENGINESTATUS_THROTTLE, st->throttle_pct);
	raw[SIL_PT_ENGINESTATUS_COOLANT] = sil_raw(pdu, SIL_PT_ENGINESTATUS_COOLANT, st->coolant_c);
	raw[SIL_PT_ENGINESTATUS_IGNITION_ON] = 1;
	raw[SIL_PT_ENGINESTATUS_ENGINE_RUNNING] = 1;
	raw[SIL_PT_ENGINESTATUS_FN_DEGRADED] = 0;
	raw[SIL_PT_ENGINESTATUS_FN_FAULT] = 0;
	return (sil_flags_t){.valid = true};
}

/* DID values use the same raw units as the CAN signals (config/diagnostics/uds.yaml) */
static int read_did(void *ctx, uint16_t did, uint8_t *out, size_t cap)
{
	const sil_vehicle_state_t *st = ctx;
	const sil_pdu_def_t *pdu = &sil_pdus[SIL_PDU_PT_ENGINESTATUS];
	int32_t raw;

	if (cap < 2)
		return -1;
	if (did == SIL_DID_ENGINE_SPEED)
		raw = sil_raw(pdu, SIL_PT_ENGINESTATUS_RPM, st->rpm);
	else if (did == SIL_DID_COOLANT_TEMP)
		raw = sil_raw(pdu, SIL_PT_ENGINESTATUS_COOLANT, st->coolant_c);
	else
		return -1;
	out[0] = (uint8_t)((uint32_t)raw >> 8);
	out[1] = (uint8_t)((uint32_t)raw & 0xFFu);
	return 2;
}

int main(int argc, char **argv)
{
	static const sil_ecu_tx_t tx[] = {
		{SIL_PDU_PT_ENGINESTATUS, fill_engine_status},
	};
	static const sil_ecu_diag_t diag = {SIL_DIAG_ECU_ENGINE, read_did};

	return sil_ecu_main(argc, argv, "engine", tx, sizeof(tx) / sizeof(tx[0]), &diag);
}
