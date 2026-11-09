/* CRC-8 project profile (project.md §5.2): CRC-8/SAE-J1850, poly 0x1D, init 0xFF, xor-out 0xFF, no reflection. */
#ifndef SIL_CRC8_H
#define SIL_CRC8_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint8_t sil_crc8(const uint8_t *data, size_t len);

/* CRC over CAN ID (2-byte big-endian) followed by payload byte 0~6. */
uint8_t sil_frame_crc(uint16_t can_id, const uint8_t payload[8]);

#ifdef __cplusplus
}
#endif

#endif
