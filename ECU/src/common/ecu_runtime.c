#define _GNU_SOURCE
#include "sil/ecu_runtime.h"

#include "sil/heartbeat.h"

#include <errno.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <linux/can.h>
#include <linux/can/isotp.h>
#include <linux/can/raw.h>
#include <sys/random.h>

#define MAX_TX 8u
#define NS_PER_MS 1000000LL
#define NS_PER_S 1000000000LL

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
	return (int64_t)ts.tv_sec * NS_PER_S + ts.tv_nsec;
}

static void sleep_until(int64_t t_ns)
{
	struct timespec ts = {.tv_sec = t_ns / NS_PER_S, .tv_nsec = t_ns % NS_PER_S};

	while (!g_stop && clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
		;
}

static int open_can(const char *ifname)
{
	struct sockaddr_can addr = {0};
	unsigned idx = if_nametoindex(ifname);
	int fd;

	if (idx == 0) {
		fprintf(stderr, "interface %s: %s\n", ifname, strerror(errno));
		return -1;
	}
	fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
	if (fd < 0) {
		perror("socket(PF_CAN)");
		return -1;
	}
	/* transmit only: receive no frames */
	if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, NULL, 0) < 0) {
		perror("CAN_RAW_FILTER");
		close(fd);
		return -1;
	}
	addr.can_family = AF_CAN;
	addr.can_ifindex = (int)idx;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		close(fd);
		return -1;
	}
	return fd;
}

static void usage(const char *prog)
{
	fprintf(stderr, "usage: %s --iface <can-if> [--duration-s <sec>] [--diag-key <file>] [--inject-dtc]\n", prog);
}

/* ---- UDS over kernel ISO-TP (project.md §9.1) ---- */

typedef struct {
	int fd;
	sil_uds_server_t uds;
	unsigned long requests;
} ecu_diag_t;

static int os_random(uint8_t *buf, size_t len)
{
	return getrandom(buf, len, 0) == (ssize_t)len ? 0 : -1;
}

static int open_isotp(const char *ifname, const sil_diag_ecu_t *ecu)
{
	struct sockaddr_can addr = {0};
	struct can_isotp_options opts = {0};
	int fd = socket(PF_CAN, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, CAN_ISOTP);

	if (fd < 0) {
		perror("socket(CAN_ISOTP)");
		return -1;
	}
	opts.flags = CAN_ISOTP_TX_PADDING;
	opts.txpad_content = SIL_ISOTP_TX_PADDING;
	if (setsockopt(fd, SOL_CAN_ISOTP, CAN_ISOTP_OPTS, &opts, sizeof(opts)) < 0) {
		perror("CAN_ISOTP_OPTS");
		close(fd);
		return -1;
	}
	addr.can_family = AF_CAN;
	addr.can_ifindex = (int)if_nametoindex(ifname);
	addr.can_addr.tp.rx_id = ecu->request_id;
	addr.can_addr.tp.tx_id = ecu->response_id;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind(CAN_ISOTP)");
		close(fd);
		return -1;
	}
	return fd;
}

static void serve_diag(ecu_diag_t *d)
{
	uint8_t req[SIL_DOIP_MAX_UDS_BYTES], resp[SIL_DOIP_MAX_UDS_BYTES];
	ssize_t n;

	while ((n = recv(d->fd, req, sizeof(req), MSG_DONTWAIT)) > 0) {
		size_t r = sil_uds_handle(&d->uds, req, (size_t)n, now_ns(), resp, sizeof(resp));

		d->requests++;
		/* a multi-frame response blocks here until the tester's Flow Control (or N_Bs timeout) */
		if (r > 0 && send(d->fd, resp, r, 0) != (ssize_t)r)
			perror("send(CAN_ISOTP)");
	}
}

/* sleep until t_ns, serving diagnostic requests that arrive in the meantime */
static void wait_until(int64_t t_ns, ecu_diag_t *d)
{
	while (!g_stop) {
		int64_t rem = t_ns - now_ns();
		struct pollfd p;
		struct timespec ts;

		if (rem <= 0)
			return;
		if (d == NULL) {
			sleep_until(t_ns);
			return;
		}
		p = (struct pollfd){.fd = d->fd, .events = POLLIN};
		ts = (struct timespec){.tv_sec = rem / NS_PER_S, .tv_nsec = rem % NS_PER_S};
		if (ppoll(&p, 1, &ts, NULL) > 0 && (p.revents & POLLIN))
			serve_diag(d);
	}
}

int32_t sil_raw(const sil_pdu_def_t *pdu, unsigned signal, double phys)
{
	return sil_phys_to_raw(&pdu->signals[signal], phys);
}

int sil_ecu_main(int argc, char **argv, const char *ecu_name, const sil_ecu_tx_t *tx, unsigned tx_count,
		 const sil_ecu_diag_t *diag)
{
	const char *ifname = NULL, *key_path = NULL;
	bool inject_dtc = false;
	ecu_diag_t dstate, *d = NULL;
	sil_vehicle_state_t latest;
	double duration_s = 0.0;
	int64_t start, end = 0, next[MAX_TX];
	uint8_t counter[MAX_TX] = {0};
	unsigned long sent[MAX_TX] = {0}, overruns[MAX_TX] = {0}, errors[MAX_TX] = {0};
	struct sigaction sa = {0};
	sil_heartbeat_t hb;
	int fd;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--iface") == 0 && i + 1 < argc) {
			ifname = argv[++i];
		} else if (strcmp(argv[i], "--duration-s") == 0 && i + 1 < argc) {
			duration_s = strtod(argv[++i], NULL);
		} else if (strcmp(argv[i], "--diag-key") == 0 && i + 1 < argc && diag != NULL) {
			key_path = argv[++i];
		} else if (strcmp(argv[i], "--inject-dtc") == 0 && diag != NULL) {
			inject_dtc = true;
		} else {
			usage(argv[0]);
			return 2;
		}
	}
	if (ifname == NULL || tx_count == 0 || tx_count > MAX_TX || duration_s < 0.0) {
		usage(argv[0]);
		return 2;
	}

	fd = open_can(ifname);
	if (fd < 0)
		return 1;

	sil_scenario_eval(0.0, &latest);
	if (key_path != NULL) {
		const sil_diag_ecu_t *ecu = &sil_diag_ecus[diag->diag_index];
		uint8_t key[SIL_UDS_KEY_BYTES];
		char err[256];

		if (sil_uds_load_key(key_path, key, err, sizeof(err)) != 0) {
			fprintf(stderr, "[%s] diagnostic key rejected: %s\n", ecu_name, err);
			close(fd);
			return 1;
		}
		dstate.fd = open_isotp(ifname, ecu);
		if (dstate.fd < 0) {
			close(fd);
			return 1;
		}
		sil_uds_init(&dstate.uds, ecu, key, diag->read_did, &latest, os_random);
		memset(key, 0, sizeof(key));
		dstate.requests = 0;
		if (inject_dtc)
			for (unsigned i = 0; i < ecu->dtc_count; i++)
				sil_uds_set_dtc_failed(&dstate.uds, i);
		d = &dstate;
		fprintf(stderr, "[%s] UDS on ISO-TP rx 0x%03X tx 0x%03X, logical address 0x%04X%s\n", ecu_name, ecu->request_id,
			ecu->response_id, ecu->logical_address, inject_dtc ? ", DTCs injected" : "");
	}

	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	start = now_ns();
	if (duration_s > 0.0)
		end = start + (int64_t)(duration_s * (double)NS_PER_S);
	for (unsigned i = 0; i < tx_count; i++)
		next[i] = start;

	sil_heartbeat_init(&hb);
	fprintf(stderr, "[%s] start iface=%s heartbeat=%s\n", ecu_name, ifname, hb.enabled ? "on" : "off");
	while (!g_stop) {
		int64_t wake = next[0], t;
		sil_vehicle_state_t st;

		for (unsigned i = 1; i < tx_count; i++)
			if (next[i] < wake)
				wake = next[i];
		if (end && wake >= end)
			break;
		wait_until(wake, d);
		if (g_stop)
			break;

		t = now_ns();
		/* heartbeat comes from the same loop that sends PDUs: a stuck loop stops both */
		sil_heartbeat_tick(&hb, t);
		sil_scenario_eval((double)(t - start) / (double)NS_PER_S, &st);
		latest = st; /* DID values come from the same state the PDUs carry */
		if (d)
			sil_uds_tick(&d->uds, t);
		for (unsigned i = 0; i < tx_count; i++) {
			const sil_pdu_def_t *pdu = &sil_pdus[tx[i].pdu_index];
			int64_t period = pdu->period_ms * NS_PER_MS;
			int32_t raw[SIL_MAX_SIGNALS] = {0};
			struct can_frame fr = {0};
			sil_flags_t flags;
			sil_status_t rc;

			if (next[i] > t)
				continue;
			flags = tx[i].fill(&st, pdu, raw);
			fr.can_id = pdu->can_id;
			fr.len = pdu->dlc;
			rc = sil_pdu_encode(pdu, raw, counter[i], flags, fr.data);
			if (rc != SIL_OK) {
				fprintf(stderr, "[%s] encode %s: %s\n", ecu_name, pdu->name, sil_status_name(rc));
				errors[i]++;
			} else if (write(fd, &fr, sizeof(fr)) != (ssize_t)sizeof(fr)) {
				errors[i]++;
			} else {
				sent[i]++;
				counter[i] = (uint8_t)((counter[i] + 1u) % SIL_COUNTER_MODULO);
			}
			/* fixed-rate schedule; if late by a whole period, skip ahead instead of bursting */
			next[i] += period;
			if (t - next[i] >= period) {
				int64_t missed = (t - next[i]) / period + 1;
				overruns[i] += (unsigned long)missed;
				next[i] += missed * period;
			}
		}
	}

	sil_heartbeat_close(&hb);
	if (d) {
		fprintf(stderr, "[%s] UDS requests=%lu resets=%lu security_failures=%u\n", ecu_name, d->requests,
			d->uds.resets, d->uds.failures);
		close(d->fd);
	}
	close(fd);
	for (unsigned i = 0; i < tx_count; i++)
		fprintf(stderr, "[%s] %s 0x%03X sent=%lu overruns=%lu errors=%lu\n", ecu_name,
			sil_pdus[tx[i].pdu_index].name, sil_pdus[tx[i].pdu_index].can_id, sent[i], overruns[i], errors[i]);
	return 0;
}
