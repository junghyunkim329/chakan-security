/* Heartbeat client for supervised processes (project.md §8.1, §8.2).
 *
 * Sends "HB1 <name> <seq>\n" as a datagram to the Execution Manager's unix socket every period.
 * Configured by environment, so processes started by hand (without the Execution Manager) just skip it:
 *   SIL_HEARTBEAT_SOCKET     socket path (unset -> disabled)
 *   SIL_PROCESS_NAME         name in the manifest
 *   SIL_HEARTBEAT_PERIOD_MS  default 500
 * The Execution Manager authenticates the sender by kernel credentials (pid), not by the name.
 */
#ifndef SIL_HEARTBEAT_H
#define SIL_HEARTBEAT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	bool enabled;
	int fd;
	char path[108];
	char name[32];
	uint32_t seq;
	int64_t period_ns;
	int64_t next_ns;
	unsigned long send_errors;
} sil_heartbeat_t;

void sil_heartbeat_init(sil_heartbeat_t *hb);
/* call from the process main loop; sends when a period has elapsed. now_ns: CLOCK_MONOTONIC */
void sil_heartbeat_tick(sil_heartbeat_t *hb, int64_t now_ns);
void sil_heartbeat_close(sil_heartbeat_t *hb);

#ifdef __cplusplus
}
#endif

#endif
