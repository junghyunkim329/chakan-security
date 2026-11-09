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
