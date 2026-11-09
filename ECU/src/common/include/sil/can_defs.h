/* CAN signal definition types. Tables are generated from config/signals/can.yaml. */
#ifndef SIL_CAN_DEFS_H
#define SIL_CAN_DEFS_H

#include <stdint.h>

#define SIL_CAN_DLC 8u
#define SIL_SIGNAL_BYTES 6u /* byte 0~5 signals, byte 6 counter/flags, byte 7 CRC */

typedef enum {
	SIL_SIG_U8,
	SIL_SIG_I8,
	SIL_SIG_U16BE,
	SIL_SIG_I16BE,
	SIL_SIG_FLAG
} sil_sig_type_t;

typedef struct {
	const char *name;
	uint8_t byte;
	uint8_t bit; /* SIL_SIG_FLAG only */
	sil_sig_type_t type;
	int32_t raw_min;
	int32_t raw_max;
	double scale; /* physical = raw * scale */
} sil_sig_def_t;

typedef struct {
	uint16_t can_id;
	const char *name;
	const char *sender;
	uint16_t period_ms;
	uint8_t dlc;
	uint8_t signal_count;
	const sil_sig_def_t *signals;
	uint8_t reserved_mask[SIL_SIGNAL_BYTES]; /* bits not used by any signal; must be 0 */
} sil_pdu_def_t;

#endif
