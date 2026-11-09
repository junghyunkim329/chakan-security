#include "sil/crc8.h"

#include "sil_can_signals_gen.h"

uint8_t sil_crc8(const uint8_t *data, size_t len)
{
	uint8_t crc = SIL_CRC_INIT;

	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int b = 0; b < 8; b++)
			crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ SIL_CRC_POLY) : (uint8_t)(crc << 1);
	}
	return (uint8_t)(crc ^ SIL_CRC_XOR_OUT);
}

uint8_t sil_frame_crc(uint16_t can_id, const uint8_t payload[8])
{
	uint8_t buf[9];

	buf[0] = (uint8_t)(can_id >> 8);
	buf[1] = (uint8_t)(can_id & 0xFFu);
	for (int i = 0; i < 7; i++)
		buf[2 + i] = payload[i];
	return sil_crc8(buf, sizeof(buf));
}
