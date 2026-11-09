#include "sil/pdu_codec.h"

#include <math.h>
#include <stddef.h>

#include "sil/crc8.h"

const char *sil_status_name(sil_status_t st)
{
	switch (st) {
	case SIL_OK: return "OK";
	case SIL_ERR_UNKNOWN_ID: return "ERR_UNKNOWN_ID";
	case SIL_ERR_DLC: return "ERR_DLC";
	case SIL_ERR_CRC: return "ERR_CRC";
	case SIL_ERR_RESERVED: return "ERR_RESERVED";
	case SIL_ERR_NOT_VALID: return "ERR_NOT_VALID";
	case SIL_ERR_RANGE: return "ERR_RANGE";
	case SIL_ERR_COUNTER_DUPLICATE: return "ERR_COUNTER_DUPLICATE";
	case SIL_ERR_COUNTER_JUMP: return "ERR_COUNTER_JUMP";
	case SIL_ERR_ARG: return "ERR_ARG";
	case SIL_ERR_FRAME_FORMAT: return "ERR_FRAME_FORMAT";
	}
	return "UNKNOWN";
}

const sil_pdu_def_t *sil_pdu_find(uint16_t can_id)
{
	for (size_t i = 0; i < SIL_PDU_COUNT; i++)
		if (sil_pdus[i].can_id == can_id)
			return &sil_pdus[i];
	return NULL;
}

static void put_signal(const sil_sig_def_t *s, int32_t raw, uint8_t *buf)
{
	switch (s->type) {
	case SIL_SIG_FLAG:
		if (raw)
			buf[s->byte] |= (uint8_t)(1u << s->bit);
		break;
	case SIL_SIG_U8:
	case SIL_SIG_I8:
		buf[s->byte] = (uint8_t)((uint32_t)raw & 0xFFu);
		break;
	case SIL_SIG_U16BE:
	case SIL_SIG_I16BE:
		buf[s->byte] = (uint8_t)(((uint32_t)raw >> 8) & 0xFFu);
		buf[s->byte + 1] = (uint8_t)((uint32_t)raw & 0xFFu);
		break;
	}
}

static int32_t get_signal(const sil_sig_def_t *s, const uint8_t *buf)
{
	uint16_t w;

	switch (s->type) {
	case SIL_SIG_FLAG:
		return (buf[s->byte] >> s->bit) & 1;
	case SIL_SIG_U8:
		return buf[s->byte];
	case SIL_SIG_I8:
		return (int8_t)buf[s->byte];
	case SIL_SIG_U16BE:
		return (int32_t)(((uint16_t)buf[s->byte] << 8) | buf[s->byte + 1]);
	case SIL_SIG_I16BE:
		w = (uint16_t)(((uint16_t)buf[s->byte] << 8) | buf[s->byte + 1]);
		return (int16_t)w;
	}
	return 0;
}

sil_status_t sil_pdu_encode(const sil_pdu_def_t *pdu, const int32_t *raw, uint8_t counter,
			    sil_flags_t flags, uint8_t out[8])
{
	if (pdu == NULL || raw == NULL || out == NULL || counter >= SIL_COUNTER_MODULO)
		return SIL_ERR_ARG;

	for (uint8_t i = 0; i < pdu->signal_count; i++)
		if (raw[i] < pdu->signals[i].raw_min || raw[i] > pdu->signals[i].raw_max)
			return SIL_ERR_RANGE;

	for (int i = 0; i < 8; i++)
		out[i] = 0;
	for (uint8_t i = 0; i < pdu->signal_count; i++)
		put_signal(&pdu->signals[i], raw[i], out);

	out[SIL_FLAGS_BYTE] = (uint8_t)((counter << SIL_COUNTER_SHIFT)
					| ((flags.valid ? 1u : 0u) << SIL_FLAG_VALID_BIT)
					| ((flags.degraded ? 1u : 0u) << SIL_FLAG_DEGRADED_BIT)
					| ((flags.fault ? 1u : 0u) << SIL_FLAG_FAULT_BIT));
	out[SIL_CRC_BYTE] = sil_frame_crc(pdu->can_id, out);
	return SIL_OK;
}

sil_status_t sil_pdu_unpack(uint16_t can_id, const uint8_t *data, uint8_t dlc, sil_pdu_frame_t *out)
{
	const sil_pdu_def_t *pdu = sil_pdu_find(can_id);
	uint8_t f;

	if (data == NULL || out == NULL)
		return SIL_ERR_ARG;
	out->pdu = pdu;
	if (pdu == NULL)
		return SIL_ERR_UNKNOWN_ID;
	if (dlc != pdu->dlc)
		return SIL_ERR_DLC;
	if (data[SIL_CRC_BYTE] != sil_frame_crc(can_id, data))
		return SIL_ERR_CRC;
	for (unsigned i = 0; i < SIL_SIGNAL_BYTES; i++)
		if (data[i] & pdu->reserved_mask[i])
			return SIL_ERR_RESERVED;
	f = data[SIL_FLAGS_BYTE];
	if (f & (1u << SIL_FLAG_RESERVED_BIT))
		return SIL_ERR_RESERVED;

	for (uint8_t i = 0; i < pdu->signal_count; i++)
		out->raw[i] = get_signal(&pdu->signals[i], data);
	out->counter = (uint8_t)((f >> SIL_COUNTER_SHIFT) & (SIL_COUNTER_MODULO - 1u));
	out->flags.valid = (f >> SIL_FLAG_VALID_BIT) & 1u;
	out->flags.degraded = (f >> SIL_FLAG_DEGRADED_BIT) & 1u;
	out->flags.fault = (f >> SIL_FLAG_FAULT_BIT) & 1u;
	return SIL_OK;
}

sil_status_t sil_pdu_check_values(const sil_pdu_frame_t *frame)
{
	if (frame == NULL || frame->pdu == NULL)
		return SIL_ERR_ARG;
	if (!frame->flags.valid)
		return SIL_ERR_NOT_VALID;
	for (uint8_t i = 0; i < frame->pdu->signal_count; i++)
		if (frame->raw[i] < frame->pdu->signals[i].raw_min || frame->raw[i] > frame->pdu->signals[i].raw_max)
			return SIL_ERR_RANGE;
	return SIL_OK;
}

sil_status_t sil_counter_check(sil_counter_state_t *st, uint8_t counter, uint8_t *lost)
{
	uint8_t delta;

	if (st == NULL || lost == NULL || counter >= SIL_COUNTER_MODULO)
		return SIL_ERR_ARG;
	*lost = 0;
	if (!st->initialized) {
		st->initialized = true;
		st->last = counter;
		return SIL_OK;
	}
	delta = (uint8_t)((counter + SIL_COUNTER_MODULO - st->last) % SIL_COUNTER_MODULO);
	if (delta == 0)
		return SIL_ERR_COUNTER_DUPLICATE;
	/* jump or reverse also resynchronizes to the received counter */
	st->last = counter;
	if (delta > SIL_COUNTER_MAX_DELTA)
		return SIL_ERR_COUNTER_JUMP;
	*lost = (uint8_t)(delta - 1u);
	return SIL_OK;
}

int32_t sil_phys_to_raw(const sil_sig_def_t *sig, double phys)
{
	return (int32_t)lround(phys / sig->scale);
}

double sil_raw_to_phys(const sil_sig_def_t *sig, int32_t raw)
{
	return raw * sig->scale;
}
