/* LIN slave process: Door, Window or Interior Light ECU on the LIN wire (project.md §6).
 * usage: lin_slave --role door|window|light --iface <lin-if> [--control <socket>] [--duration-s <sec>]
 * --control: unix datagram socket for test inputs (door/lock changes and fault injection), mode 0600. */
#define _GNU_SOURCE
#include <errno.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <linux/can.h>

#include "lin_slave.h"

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static int64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int open_wire(const char *ifname)
{
	struct sockaddr_can addr = {0};
	int fd = socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC, CAN_RAW);

	if (fd < 0)
		return -1;
	addr.can_family = AF_CAN;
	addr.can_ifindex = (int)if_nametoindex(ifname);
	if (addr.can_ifindex == 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int open_control(const char *path)
{
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	int fd;

	if (strlen(path) >= sizeof(addr.sun_path))
		return -1;
	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return -1;
	strcpy(addr.sun_path, path);
	unlink(path);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || chmod(path, 0600) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int main(int argc, char **argv)
{
	const char *role_s = NULL, *ifname = NULL, *control = NULL;
	double duration_s = 0;
	sil_lin_slave_t s;
	sil_lin_role_t role;
	struct sigaction sa = {0};
	int64_t start, end = 0;
	int wire, cfd = -1;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--role") && i + 1 < argc)
			role_s = argv[++i];
		else if (!strcmp(argv[i], "--iface") && i + 1 < argc)
			ifname = argv[++i];
		else if (!strcmp(argv[i], "--control") && i + 1 < argc)
			control = argv[++i];
		else if (!strcmp(argv[i], "--duration-s") && i + 1 < argc)
			duration_s = strtod(argv[++i], NULL);
		else
			role_s = NULL, i = argc;
	}
	if (role_s && !strcmp(role_s, "door"))
		role = SIL_LIN_ROLE_DOOR;
	else if (role_s && !strcmp(role_s, "window"))
		role = SIL_LIN_ROLE_WINDOW;
	else if (role_s && !strcmp(role_s, "light"))
		role = SIL_LIN_ROLE_LIGHT;
	else {
		fprintf(stderr, "usage: %s --role door|window|light --iface <lin-if> [--control <socket>] [--duration-s <sec>]\n", argv[0]);
		return 2;
	}
	if (!ifname || (wire = open_wire(ifname)) < 0) {
		fprintf(stderr, "[lin_%s] LIN wire %s: %s\n", role_s, ifname ? ifname : "?", strerror(errno));
		return 1;
	}
	if (control && (cfd = open_control(control)) < 0) {
		fprintf(stderr, "[lin_%s] control socket %s: %s\n", role_s, control, strerror(errno));
		return 1;
	}
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	start = now_ns();
	if (duration_s > 0)
		end = start + (int64_t)(duration_s * 1e9);
	sil_lin_slave_init(&s, role, start);
	fprintf(stderr, "[lin_%s] start iface=%s\n", role_s, ifname);

	while (!g_stop && (!end || now_ns() < end)) {
		struct pollfd p[2] = {{wire, POLLIN, 0}, {cfd, POLLIN, 0}};
		bool was_asleep = s.asleep;

		poll(p, cfd >= 0 ? 2 : 1, 100);
		if (p[0].revents & POLLIN) {
			struct can_frame fr;

			while (recv(wire, &fr, sizeof(fr), MSG_DONTWAIT) == (ssize_t)sizeof(fr)) {
				int64_t t = now_ns();
				uint8_t out[8];
				size_t n;

				if (fr.can_id == SIL_LIN_HEADER_CAN_ID && fr.len == 1) {
					n = sil_lin_slave_on_header(&s, fr.data[0], t, out);
					if (n > 0) {
						struct can_frame rsp = {.can_id = SIL_LIN_RESPONSE_CAN_ID, .len = (uint8_t)n};
						memcpy(rsp.data, out, n);
						if (write(wire, &rsp, sizeof(rsp)) != (ssize_t)sizeof(rsp))
							perror("write response");
					}
				} else if (fr.can_id == SIL_LIN_RESPONSE_CAN_ID) {
					sil_lin_slave_on_response(&s, fr.data, fr.len);
				} else if (fr.can_id == SIL_LIN_WAKEUP_CAN_ID) {
					sil_lin_slave_on_wakeup(&s, t);
				}
			}
		}
		if (cfd >= 0 && (p[1].revents & POLLIN)) {
			char cmd[64];
			ssize_t n;

			while ((n = recv(cfd, cmd, sizeof(cmd) - 1, MSG_DONTWAIT)) > 0) {
				cmd[n] = '\0';
				cmd[strcspn(cmd, "\n")] = '\0';
				fprintf(stderr, "[lin_%s] control '%s' %s\n", role_s, cmd, sil_lin_slave_control(&s, cmd) ? "rejected" : "applied");
			}
		}
		sil_lin_slave_tick(&s, now_ns());
		if (s.asleep != was_asleep)
			fprintf(stderr, "[lin_%s] bus %s\n", role_s, s.asleep ? "sleep" : "awake");
	}

	fprintf(stderr, "[lin_%s] headers=%lu bad_parity=%lu responses=%lu commands=%lu bad_checksum=%lu wakeups=%lu\n", role_s,
		s.headers, s.bad_parity, s.responses_sent, s.commands_applied, s.bad_checksum, s.wakeups);
	if (control)
		unlink(control);
	close(wire);
	return 0;
}
