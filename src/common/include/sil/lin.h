/* LIN protocol helpers (project.md §6.2): protected identifier, enhanced checksum, signal access,
 * and the practice encapsulation of LIN frames on a dedicated vxcan wire. */
#ifndef SIL_LIN_H
#define SIL_LIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sil_lin_gen.h"

#ifdef __cplusplus
extern "C" {
#endif

/* P0 = ID0^ID1^ID2^ID4, P1 = NOT(ID1^ID3^ID4^ID5) */
uint8_t sil_lin_pid(uint8_t frame_id);
/* true and *frame_id if both parity bits are correct */
bool sil_lin_pid_check(uint8_t pid, uint8_t *frame_id);
/* inverted carry-around sum of PID and data */
uint8_t sil_lin_checksum(uint8_t pid, const uint8_t *data, size_t len);

const sil_lin_frame_t *sil_lin_frame(uint8_t frame_id);

uint8_t sil_lin_get(const uint8_t *payload, unsigned byte, unsigned bit, unsigned bits);
void sil_lin_put(uint8_t *payload, unsigned byte, unsigned bit, unsigned bits, uint8_t value);

#ifdef __cplusplus
}
#endif

#endif
