#include "sil/lin.h"

uint8_t sil_lin_pid(uint8_t frame_id)
{
	unsigned b[6];
	unsigned p0, p1;

	for (int i = 0; i < 6; i++)
		b[i] = (frame_id >> i) & 1u;
	p0 = b[0] ^ b[1] ^ b[2] ^ b[4];
	p1 = 1u ^ (b[1] ^ b[3] ^ b[4] ^ b[5]);
	return (uint8_t)((frame_id & 0x3Fu) | (p0 << 6) | (p1 << 7));
}

bool sil_lin_pid_check(uint8_t pid, uint8_t *frame_id)
{
	uint8_t id = pid & 0x3Fu;

	if (sil_lin_pid(id) != pid)
		return false;
	*frame_id = id;
	return true;
}

uint8_t sil_lin_checksum(uint8_t pid, const uint8_t *data, size_t len)
{
	unsigned sum = pid;

	for (size_t i = 0; i < len; i++) {
		sum += data[i];
		if (sum > 0xFFu)
			sum -= 0xFFu;
	}
	return (uint8_t)(~sum & 0xFFu);
}

const sil_lin_frame_t *sil_lin_frame(uint8_t frame_id)
{
	for (unsigned i = 0; i < SIL_LIN_FRAME_COUNT; i++)
		if (sil_lin_frames[i].id == frame_id)
			return &sil_lin_frames[i];
	return NULL;
}

uint8_t sil_lin_get(const uint8_t *payload, unsigned byte, unsigned bit, unsigned bits)
{
	return (uint8_t)((payload[byte] >> bit) & ((1u << bits) - 1u));
}

void sil_lin_put(uint8_t *payload, unsigned byte, unsigned bit, unsigned bits, uint8_t value)
{
	uint8_t mask = (uint8_t)(((1u << bits) - 1u) << bit);

	payload[byte] = (uint8_t)((payload[byte] & ~mask) | ((value << bit) & mask));
}

sil_lin_sem_t sil_lin_check_semantics(const sil_lin_frame_t *frame, const uint8_t *payload)
{
	for (unsigned i = 0; i < frame->dlc; i++)
		if (payload[i] & (uint8_t)~frame->used_mask[i])
			return SIL_LIN_SEM_RESERVED;
	if (frame->has_valid && !sil_lin_get(payload, frame->valid_byte, frame->valid_bit, 1))
		return SIL_LIN_SEM_NOT_VALID;
	for (unsigned i = frame->range_first; i < (unsigned)frame->range_first + frame->range_count; i++) {
		const sil_lin_range_t *r = &sil_lin_ranges[i];
		const uint8_t v = sil_lin_get(payload, r->byte, r->bit, r->bits);
		if (v < r->min || v > r->max)
			return SIL_LIN_SEM_RANGE;
	}
	return SIL_LIN_SEM_OK;
}
