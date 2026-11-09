/* LIN protocol and slave behaviour tests (project.md §6.2, §6.3). */
#include <string.h>

#include "lin_slave.h"
#include "sil_test.h"

#define MS 1000000LL

/* master side helpers: send a command frame (header + response) to a slave */
static void send_command(sil_lin_slave_t *s, uint8_t id, const uint8_t *payload, uint8_t dlc, int64_t t)
{
	uint8_t pid = sil_lin_pid(id), out[8], rsp[8];

	CHECK_EQ_INT(sil_lin_slave_on_header(s, pid, t, out), 0);
	memcpy(rsp, payload, dlc);
	rsp[dlc] = sil_lin_checksum(pid, payload, dlc);
	sil_lin_slave_on_response(s, rsp, dlc + 1u);
}

static size_t poll_status(sil_lin_slave_t *s, uint8_t id, int64_t t, uint8_t out[8])
{
	return sil_lin_slave_on_header(s, sil_lin_pid(id), t, out);
}

static void test_parity_and_checksum(void)
{
	uint8_t id = 0;
	static const uint8_t lin_spec_data[] = {0x55, 0x93, 0xE5};

	/* LIN 2.x protected identifier table */
	CHECK_EQ_INT(sil_lin_pid(0x10), 0x50);
	CHECK_EQ_INT(sil_lin_pid(0x11), 0x11);
	CHECK_EQ_INT(sil_lin_pid(0x12), 0x92);
	CHECK_EQ_INT(sil_lin_pid(0x13), 0xD3);
	CHECK_EQ_INT(sil_lin_pid(0x14), 0x14);
	CHECK_EQ_INT(sil_lin_pid(0x3C), 0x3C);
	CHECK_EQ_INT(sil_lin_pid(0x3D), 0x7D);
	CHECK_EQ_INT(SIL_LIN_BODY_DOORSTATUS_PID, 0x50);
	for (int i = 0; i < 64; i++) {
		CHECK(sil_lin_pid_check(sil_lin_pid((uint8_t)i), &id) && id == i);
		CHECK(!sil_lin_pid_check(sil_lin_pid((uint8_t)i) ^ 0x40u, &id));
		CHECK(!sil_lin_pid_check(sil_lin_pid((uint8_t)i) ^ 0x80u, &id));
	}
	/* LIN 2.x specification enhanced checksum example */
	CHECK_EQ_INT(sil_lin_checksum(0x4A, lin_spec_data, 3), 0xE6);
	/* carry-around: 0xFF + 0x01 wraps to 0x01 */
	{
		const uint8_t d[] = {0x01};
		CHECK_EQ_INT(sil_lin_checksum(0xFF, d, 1), (uint8_t)~0x01u);
	}
}

static void test_schedule_table(void)
{
	static const uint8_t expect[10] = {0x11, 0x12, 0x13, 0x14, 0x10, 0x11, 0x12, 0xFF, 0xFF, 0xFF};

	CHECK(memcmp(sil_lin_schedule, expect, 10) == 0);
	CHECK_EQ_INT(SIL_LIN_TICK_MS, 10);
}

static void test_door_initial_and_changes(void)
{
	sil_lin_slave_t s;
	uint8_t out[8];
	size_t n;

	sil_lin_slave_init(&s, SIL_LIN_ROLE_DOOR, 0);
	n = poll_status(&s, 0x10, 0, out);
	CHECK_EQ_INT(n, 3);
	CHECK(out[0] == 0x00 && out[1] == 0x81); /* doors closed; central lock + valid */
	CHECK_EQ_INT(out[2], sil_lin_checksum(0x50, out, 2));
	CHECK_EQ_INT(sil_lin_slave_control(&s, "door FL 1"), 0);
	CHECK_EQ_INT(sil_lin_slave_control(&s, "lock 0"), 0);
	poll_status(&s, 0x10, 0, out);
	CHECK(out[0] == 0x01 && out[1] == 0x80);
	CHECK(sil_lin_slave_control(&s, "door XX 1") != 0 && sil_lin_slave_control(&s, "jump 1") != 0);
	/* the door ECU does not answer other frames */
	CHECK_EQ_INT(poll_status(&s, 0x12, 0, out), 0);
}

static void test_window_motion(void)
{
	sil_lin_slave_t s;
	uint8_t cmd[2] = {50, 0x01}, out[8];
	int responses = 0;

	sil_lin_slave_init(&s, SIL_LIN_ROLE_WINDOW, 0);
	send_command(&s, 0x11, cmd, 2, 0);
	CHECK_EQ_INT(s.target, 50);
	/* 5 % per response, 50 ms apart */
	while (s.position < 50 && responses < 20) {
		CHECK_EQ_INT(poll_status(&s, 0x12, 0, out), 4);
		responses++;
		CHECK(out[0] == responses * 5 || out[0] == 50);
		CHECK(out[1] == 1 || out[0] == 50);
	}
	CHECK_EQ_INT(responses, 10);
	poll_status(&s, 0x12, 0, out);
	CHECK(out[0] == 50 && out[1] == 0 && out[2] == 0x80);

	/* cancel stops at once */
	cmd[0] = 0;
	send_command(&s, 0x11, cmd, 2, 0);
	poll_status(&s, 0x12, 0, out);
	CHECK(out[0] == 45 && out[1] == 2);
	cmd[1] = 0x02;
	send_command(&s, 0x11, cmd, 2, 0);
	poll_status(&s, 0x12, 0, out);
	CHECK(out[0] == 45 && out[1] == 0);

	/* obstruction: stop, keep position, fault bit; new commands ignored while obstructed */
	cmd[0] = 100;
	cmd[1] = 0x01;
	send_command(&s, 0x11, cmd, 2, 0);
	poll_status(&s, 0x12, 0, out);
	sil_lin_slave_control(&s, "obstruct 1");
	poll_status(&s, 0x12, 0, out);
	CHECK(out[0] == 50 && out[1] == 0 && (out[2] & 0x03) == 0x03);
	send_command(&s, 0x11, cmd, 2, 0);
	poll_status(&s, 0x12, 0, out);
	CHECK_EQ_INT(out[0], 50);

	/* a command with a wrong checksum is ignored */
	sil_lin_slave_init(&s, SIL_LIN_ROLE_WINDOW, 0);
	sil_lin_slave_on_header(&s, sil_lin_pid(0x11), 0, out);
	{
		uint8_t bad[3] = {80, 0x01, 0x00};
		sil_lin_slave_on_response(&s, bad, 3);
	}
	CHECK(s.target == 0 && s.bad_checksum == 1);
}

static void test_light_fade(void)
{
	sil_lin_slave_t s;
	uint8_t cmd[2] = {100, 0x01}, out[8];

	sil_lin_slave_init(&s, SIL_LIN_ROLE_LIGHT, 0);
	send_command(&s, 0x13, cmd, 2, 0);
	poll_status(&s, 0x14, 0, out);
	CHECK(out[0] == 10 && (out[1] & 0x01));
	for (int i = 0; i < 9; i++)
		poll_status(&s, 0x14, 0, out);
	CHECK_EQ_INT(out[0], 100);
	cmd[0] = 0;
	cmd[1] = 0x00; /* fade off: at once */
	send_command(&s, 0x13, cmd, 2, 0);
	poll_status(&s, 0x14, 0, out);
	CHECK(out[0] == 0 && !(out[1] & 0x01) && (out[1] & 0x80));
}

static void test_fault_injection_and_parity(void)
{
	sil_lin_slave_t s;
	uint8_t out[8];

	sil_lin_slave_init(&s, SIL_LIN_ROLE_LIGHT, 0);
	sil_lin_slave_control(&s, "mute 1");
	CHECK_EQ_INT(poll_status(&s, 0x14, 0, out), 0);
	sil_lin_slave_control(&s, "mute 0");
	sil_lin_slave_control(&s, "corrupt 1");
	CHECK_EQ_INT(poll_status(&s, 0x14, 0, out), 3);
	CHECK(out[2] != sil_lin_checksum(0x14, out, 2));
	/* a header with wrong parity bits is ignored */
	CHECK_EQ_INT(sil_lin_slave_on_header(&s, 0x14 | 0x40, 0, out), 0);
	CHECK_EQ_INT(s.bad_parity, 1);
}

static void test_bus_sleep(void)
{
	sil_lin_slave_t s;

	sil_lin_slave_init(&s, SIL_LIN_ROLE_DOOR, 0);
	sil_lin_slave_tick(&s, 3999 * MS);
	CHECK(!s.asleep);
	sil_lin_slave_tick(&s, 4000 * MS);
	CHECK(s.asleep);
	sil_lin_slave_on_wakeup(&s, 5000 * MS);
	CHECK(!s.asleep && s.wakeups == 1);
}

int main(void)
{
	test_parity_and_checksum();
	test_schedule_table();
	test_door_initial_and_changes();
	test_window_motion();
	test_light_fade();
	test_fault_injection_and_parity();
	test_bus_sleep();
	return TEST_REPORT();
}
