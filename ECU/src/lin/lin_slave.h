/* LIN slave behaviour for the body ECUs (project.md §6.1~6.3). Pure: no sockets, time is passed in. */
#ifndef SIL_LIN_SLAVE_H
#define SIL_LIN_SLAVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sil/lin.h"

typedef enum { SIL_LIN_ROLE_DOOR, SIL_LIN_ROLE_WINDOW, SIL_LIN_ROLE_LIGHT } sil_lin_role_t;

typedef struct {
	sil_lin_role_t role;

	/* door */
	uint8_t doors; /* bit0 FL, bit1 FR, bit2 RL, bit3 RR */
	bool central_lock, child_lock;
	/* window */
	uint8_t position, target, motion; /* motion: 0 stopped, 1 opening, 2 closing */
	bool obstruction, window_fault;
	/* light */
	uint8_t brightness, light_target;
	bool fade, light_fault;

	/* test fault injection */
	bool mute;    /* do not answer own headers (missing response) */
	bool corrupt; /* answer with a wrong checksum */

	/* bus */
	int expecting;           /* frame ID of a subscribed response that should follow, or -1 */
	uint8_t expecting_pid;
	int64_t last_header_ns;
	bool asleep;

	unsigned long headers, bad_parity, responses_sent, commands_applied, bad_checksum, wakeups;
} sil_lin_slave_t;

void sil_lin_slave_init(sil_lin_slave_t *s, sil_lin_role_t role, int64_t now_ns);

/* Header with protected identifier pid. Returns the response length to send (payload + checksum) or 0. */
size_t sil_lin_slave_on_header(sil_lin_slave_t *s, uint8_t pid, int64_t now_ns, uint8_t out[8]);

/* Response element seen on the bus (payload + checksum). Applied only if it answers a subscribed header. */
void sil_lin_slave_on_response(sil_lin_slave_t *s, const uint8_t *data, size_t len);

void sil_lin_slave_on_wakeup(sil_lin_slave_t *s, int64_t now_ns);

/* LIN 2.x bus inactivity -> sleep */
void sil_lin_slave_tick(sil_lin_slave_t *s, int64_t now_ns);

/* Test control: "door FL 1", "lock 0", "childlock 1", "obstruct 1", "fault 1", "mute 1", "corrupt 1".
 * Returns 0 if understood. */
int sil_lin_slave_control(sil_lin_slave_t *s, const char *cmd);

#endif
