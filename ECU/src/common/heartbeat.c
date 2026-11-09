#define _GNU_SOURCE
#include "sil/heartbeat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

void sil_heartbeat_init(sil_heartbeat_t *hb)
{
	const char *path = getenv("SIL_HEARTBEAT_SOCKET");
	const char *name = getenv("SIL_PROCESS_NAME");
	const char *period = getenv("SIL_HEARTBEAT_PERIOD_MS");
	long ms = period ? strtol(period, NULL, 10) : 500;

	memset(hb, 0, sizeof(*hb));
	hb->fd = -1;
	if (path == NULL || name == NULL || strlen(path) >= sizeof(hb->path) || strlen(name) >= sizeof(hb->name) || ms <= 0)
		return;
	hb->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (hb->fd < 0)
		return;
	strcpy(hb->path, path);
	strcpy(hb->name, name);
	hb->period_ns = ms * 1000000LL;
	hb->enabled = true;
}

void sil_heartbeat_tick(sil_heartbeat_t *hb, int64_t now_ns)
{
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	char msg[64];
	int n;

	if (!hb->enabled || now_ns < hb->next_ns)
		return;
	hb->next_ns = now_ns + hb->period_ns;
	memcpy(addr.sun_path, hb->path, strlen(hb->path) + 1);
	n = snprintf(msg, sizeof(msg), "HB1 %s %u\n", hb->name, hb->seq++);
	if (sendto(hb->fd, msg, (size_t)n, 0, (struct sockaddr *)&addr, sizeof(addr)) != n)
		hb->send_errors++;
}

void sil_heartbeat_close(sil_heartbeat_t *hb)
{
	if (hb->fd >= 0)
		close(hb->fd);
	hb->fd = -1;
	hb->enabled = false;
}
