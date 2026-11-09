/* Periodic CAN transmitter shared by ECU processes. */
#ifndef SIL_ECU_RUNTIME_H
#define SIL_ECU_RUNTIME_H

#include <stdint.h>

#include "sil/pdu_codec.h"
#include "sil/scenario.h"
#include "sil/uds_server.h"

typedef struct {
	unsigned pdu_index; /* SIL_PDU_* */
	/* fill raw[] from scenario state; return flags for this cycle */
	sil_flags_t (*fill)(const sil_vehicle_state_t *st, const sil_pdu_def_t *pdu, int32_t *raw);
} sil_ecu_tx_t;

/* Optional UDS server (project.md §9). read_did receives the latest sil_vehicle_state_t as ctx. */
typedef struct {
	unsigned diag_index; /* SIL_DIAG_ECU_* */
	sil_uds_read_did_fn read_did;
} sil_ecu_diag_t;

/*
 * Parses --iface <name> [--duration-s <sec>] [--diag-key <file>] [--inject-dtc], opens a CAN_RAW socket and
 * sends every tx PDU at its period (CLOCK_MONOTONIC absolute deadlines) until SIGINT/SIGTERM or duration.
 * With diag and --diag-key, also serves UDS on a kernel ISO-TP socket between the PDU deadlines.
 * --inject-dtc marks every DTC of this ECU as failed at start (test fault injection).
 */
int sil_ecu_main(int argc, char **argv, const char *ecu_name, const sil_ecu_tx_t *tx, unsigned tx_count,
		 const sil_ecu_diag_t *diag);

/* fill helper: raw value of a named physical quantity */
int32_t sil_raw(const sil_pdu_def_t *pdu, unsigned signal, double phys);

#endif
