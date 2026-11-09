/* PDU encode/decode with project protection format (project.md §5.1~5.4). */
#ifndef SIL_PDU_CODEC_H
#define SIL_PDU_CODEC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "sil_can_signals_gen.h"

/* Names are shared with tools/sil_can/codec.py */
typedef enum {
	SIL_OK = 0,
	SIL_ERR_UNKNOWN_ID,
	SIL_ERR_DLC,
	SIL_ERR_CRC,
	SIL_ERR_RESERVED,
	SIL_ERR_NOT_VALID,
	SIL_ERR_RANGE,
	SIL_ERR_COUNTER_DUPLICATE,
	SIL_ERR_COUNTER_JUMP,
	SIL_ERR_ARG,
	SIL_ERR_FRAME_FORMAT /* not Classical CAN data frame: 29-bit ID, RTR or error frame */
} sil_status_t;

typedef struct {
	bool valid;
	bool degraded;
	bool fault;
} sil_flags_t;

typedef struct {
	const sil_pdu_def_t *pdu;
	int32_t raw[SIL_MAX_SIGNALS];
	uint8_t counter;
	sil_flags_t flags;
} sil_pdu_frame_t;

typedef struct {
	bool initialized;
	uint8_t last;
} sil_counter_state_t;

const char *sil_status_name(sil_status_t st);
const sil_pdu_def_t *sil_pdu_find(uint16_t can_id);

/* Sender side. Rejects raw values outside the defined range and counter >= 16. */
sil_status_t sil_pdu_encode(const sil_pdu_def_t *pdu, const int32_t *raw, uint8_t counter,
			    sil_flags_t flags, uint8_t out[8]);

/* Receiver side, stateless: CAN ID -> DLC -> CRC -> reserved bits. */
sil_status_t sil_pdu_unpack(uint16_t can_id, const uint8_t *data, uint8_t dlc, sil_pdu_frame_t *out);

/* Receiver side: data-valid flag, then physical range of every signal. */
sil_status_t sil_pdu_check_values(const sil_pdu_frame_t *frame);

/* Receiver side, per PDU: alive counter. *lost receives the number of skipped frames. */
sil_status_t sil_counter_check(sil_counter_state_t *st, uint8_t counter, uint8_t *lost);

/* physical <-> raw helpers for scenario code and tests */
int32_t sil_phys_to_raw(const sil_sig_def_t *sig, double phys);
double sil_raw_to_phys(const sil_sig_def_t *sig, int32_t raw);

#ifdef __cplusplus
}
#endif

#endif
