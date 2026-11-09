/* CRC-8 profile tests (project.md §5.2). */
#include <string.h>

#include "sil/crc8.h"
#include "sil_test.h"

int main(void)
{
	const uint8_t check[] = "123456789";
	uint8_t payload[8] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0x00};
	uint8_t crc;

	/* CRC-8/SAE-J1850 catalogue check value */
	CHECK_EQ_INT(sil_crc8(check, 9), 0x4B);
	/* empty input: init 0xFF xor-out 0xFF */
	CHECK_EQ_INT(sil_crc8(check, 0), 0x00);

	/* byte 7 (CRC slot) is not part of the input */
	crc = sil_frame_crc(0x100, payload);
	payload[7] = 0xA5;
	CHECK_EQ_INT(sil_frame_crc(0x100, payload), crc);

	/* CAN ID is part of the input: same payload under another ID differs */
	CHECK(sil_frame_crc(0x180, payload) != crc);

	/* every single-bit error in ID+payload changes the CRC */
	for (int byte = 0; byte < 7; byte++) {
		for (int bit = 0; bit < 8; bit++) {
			payload[byte] ^= (uint8_t)(1u << bit);
			CHECK(sil_frame_crc(0x100, payload) != crc);
			payload[byte] ^= (uint8_t)(1u << bit);
		}
	}
	for (int bit = 0; bit < 11; bit++)
		CHECK(sil_frame_crc((uint16_t)(0x100 ^ (1u << bit)), payload) != crc);

	return TEST_REPORT();
}
