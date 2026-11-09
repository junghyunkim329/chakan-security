/* Prints C-encoded frames as JSON lines so tests/unit/python can cross-check the Python codec. */
#include <stdio.h>

#include "sil/pdu_codec.h"

static void emit(const sil_pdu_def_t *pdu, const int32_t *raw, uint8_t counter, sil_flags_t f)
{
	uint8_t d[8];

	if (sil_pdu_encode(pdu, raw, counter, f, d) != SIL_OK)
		return;
	printf("{\"id\": %u, \"counter\": %u, \"valid\": %d, \"degraded\": %d, \"fault\": %d, \"raw\": [",
	       pdu->can_id, counter, f.valid, f.degraded, f.fault);
	for (uint8_t i = 0; i < pdu->signal_count; i++)
		printf("%s%d", i ? ", " : "", raw[i]);
	printf("], \"data\": \"");
	for (int i = 0; i < 8; i++)
		printf("%02x", d[i]);
	printf("\"}\n");
}

int main(void)
{
	for (unsigned p = 0; p < SIL_PDU_COUNT; p++) {
		const sil_pdu_def_t *pdu = &sil_pdus[p];
		int32_t raw[SIL_MAX_SIGNALS];

		for (int which = 0; which < 4; which++) {
			for (uint8_t i = 0; i < pdu->signal_count; i++) {
				const sil_sig_def_t *s = &pdu->signals[i];
				int32_t span = s->raw_max - s->raw_min;

				raw[i] = which == 0 ? s->raw_min : which == 1 ? s->raw_max
					: s->raw_min + (int32_t)(((long)span * (which * 7 + (int)i * 3)) % (span + 1));
			}
			for (uint8_t c = 0; c < SIL_COUNTER_MODULO; c++)
				emit(pdu, raw, c, (sil_flags_t){.valid = true});
			emit(pdu, raw, 9, (sil_flags_t){.valid = false, .degraded = true, .fault = true});
		}
	}
	return 0;
}
