#include "lin_slave.h"

#include <stdio.h>
#include <string.h>

#define NS_PER_MS 1000000LL

void sil_lin_slave_init(sil_lin_slave_t *s, sil_lin_role_t role, int64_t now_ns)
{
	memset(s, 0, sizeof(*s));
	s->role = role;
	/* §6.3 initial state: doors closed, central lock on, window 0, light 0, all valid */
	s->central_lock = true;
	s->expecting = -1;
	s->last_header_ns = now_ns;
}

static uint8_t own_frame(const sil_lin_slave_t *s)
{
	switch (s->role) {
	case SIL_LIN_ROLE_DOOR: return SIL_LIN_BODY_DOORSTATUS_ID;
	case SIL_LIN_ROLE_WINDOW: return SIL_LIN_BODY_WINDOWSTATUS_ID;
	case SIL_LIN_ROLE_LIGHT: return SIL_LIN_BODY_LIGHTSTATUS_ID;
	}
	return 0xFF;
}

static int subscribed_frame(const sil_lin_slave_t *s)
{
	if (s->role == SIL_LIN_ROLE_WINDOW)
		return SIL_LIN_BODY_WINDOWCOMMAND_ID;
	if (s->role == SIL_LIN_ROLE_LIGHT)
		return SIL_LIN_BODY_LIGHTCOMMAND_ID;
	return -1;
}

static uint8_t step_towards(uint8_t from, uint8_t to, uint8_t step)
{
	if (from < to)
		return (uint8_t)(to - from > step ? from + step : to);
	return (uint8_t)(from - to > step ? from - step : to);
}

static size_t build_status(sil_lin_slave_t *s, uint8_t *p)
{
	memset(p, 0, 8);
	switch (s->role) {
	case SIL_LIN_ROLE_DOOR:
		p[0] = s->doors & 0x0Fu;
		sil_lin_put(p, SIL_LIN_BODY_DOORSTATUS_CENTRAL_LOCK_BYTE, SIL_LIN_BODY_DOORSTATUS_CENTRAL_LOCK_BIT, 1, s->central_lock);
		sil_lin_put(p, SIL_LIN_BODY_DOORSTATUS_CHILD_LOCK_BYTE, SIL_LIN_BODY_DOORSTATUS_CHILD_LOCK_BIT, 1, s->child_lock);
		sil_lin_put(p, SIL_LIN_BODY_DOORSTATUS_VALID_BYTE, SIL_LIN_BODY_DOORSTATUS_VALID_BIT, 1, 1);
		return SIL_LIN_BODY_DOORSTATUS_DLC;
	case SIL_LIN_ROLE_WINDOW:
		/* §6.3: at most 5 % per 50 ms response, stopped while obstructed */
		if (s->obstruction) {
			s->target = s->position;
			s->motion = 0;
		} else if (s->position != s->target) {
			s->motion = s->position < s->target ? 1 : 2;
			s->position = step_towards(s->position, s->target, SIL_LIN_WINDOW_STEP);
		} else {
			s->motion = 0;
		}
		p[SIL_LIN_BODY_WINDOWSTATUS_POSITION_BYTE] = s->position;
		p[SIL_LIN_BODY_WINDOWSTATUS_MOTION_BYTE] = s->motion;
		sil_lin_put(p, SIL_LIN_BODY_WINDOWSTATUS_OBSTRUCTION_BYTE, SIL_LIN_BODY_WINDOWSTATUS_OBSTRUCTION_BIT, 1, s->obstruction);
		sil_lin_put(p, SIL_LIN_BODY_WINDOWSTATUS_FAULT_BYTE, SIL_LIN_BODY_WINDOWSTATUS_FAULT_BIT, 1, s->window_fault);
		sil_lin_put(p, SIL_LIN_BODY_WINDOWSTATUS_VALID_BYTE, SIL_LIN_BODY_WINDOWSTATUS_VALID_BIT, 1, 1);
		return SIL_LIN_BODY_WINDOWSTATUS_DLC;
	case SIL_LIN_ROLE_LIGHT:
		/* §6.3: fade off -> target at once, fade on -> at most 10 % per 100 ms */
		s->brightness = s->fade ? step_towards(s->brightness, s->light_target, SIL_LIN_LIGHT_FADE_STEP) : s->light_target;
		p[SIL_LIN_BODY_LIGHTSTATUS_BRIGHTNESS_BYTE] = s->brightness;
		sil_lin_put(p, SIL_LIN_BODY_LIGHTSTATUS_ACTIVE_BYTE, SIL_LIN_BODY_LIGHTSTATUS_ACTIVE_BIT, 1, s->brightness > 0);
		sil_lin_put(p, SIL_LIN_BODY_LIGHTSTATUS_FAULT_BYTE, SIL_LIN_BODY_LIGHTSTATUS_FAULT_BIT, 1, s->light_fault);
		sil_lin_put(p, SIL_LIN_BODY_LIGHTSTATUS_VALID_BYTE, SIL_LIN_BODY_LIGHTSTATUS_VALID_BIT, 1, 1);
		return SIL_LIN_BODY_LIGHTSTATUS_DLC;
	}
	return 0;
}

size_t sil_lin_slave_on_header(sil_lin_slave_t *s, uint8_t pid, int64_t now_ns, uint8_t out[8])
{
	uint8_t id;
	size_t n;

	s->expecting = -1;
	s->last_header_ns = now_ns;
	s->asleep = false;
	if (!sil_lin_pid_check(pid, &id)) { /* §6.2 protected identifier parity */
		s->bad_parity++;
		return 0;
	}
	s->headers++;
	if (id == subscribed_frame(s)) {
		s->expecting = id;
		s->expecting_pid = pid;
		return 0;
	}
	if (id != own_frame(s) || s->mute)
		return 0; /* a slave only answers its own frame in the master's slot (§6.1) */
	n = build_status(s, out);
	out[n] = sil_lin_checksum(pid, out, n);
	if (s->corrupt)
		out[n] ^= 0x5Au;
	s->responses_sent++;
	return n + 1;
}

void sil_lin_slave_on_response(sil_lin_slave_t *s, const uint8_t *data, size_t len)
{
	const sil_lin_frame_t *f;

	if (s->expecting < 0)
		return;
	f = sil_lin_frame((uint8_t)s->expecting);
	s->expecting = -1;
	if (f == NULL || len != (size_t)f->dlc + 1u)
		return;
	if (data[f->dlc] != sil_lin_checksum(s->expecting_pid, data, f->dlc)) {
		s->bad_checksum++;
		return;
	}
	if (f->id == SIL_LIN_BODY_WINDOWCOMMAND_ID) {
		uint8_t target = data[SIL_LIN_BODY_WINDOWCOMMAND_TARGET_BYTE];
		bool execute = sil_lin_get(data, SIL_LIN_BODY_WINDOWCOMMAND_EXECUTE_BYTE, SIL_LIN_BODY_WINDOWCOMMAND_EXECUTE_BIT, 1);
		bool cancel = sil_lin_get(data, SIL_LIN_BODY_WINDOWCOMMAND_CANCEL_BYTE, SIL_LIN_BODY_WINDOWCOMMAND_CANCEL_BIT, 1);

		if (cancel) {
			s->target = s->position; /* §6.3 cancel: stop and keep the position */
			s->commands_applied++;
		} else if (execute && target <= 100 && !s->obstruction && s->target != target) {
			s->target = target;
			s->commands_applied++;
		}
	} else if (f->id == SIL_LIN_BODY_LIGHTCOMMAND_ID) {
		uint8_t target = data[SIL_LIN_BODY_LIGHTCOMMAND_TARGET_BYTE];

		if (target <= 100 && (target != s->light_target ||
				      s->fade != sil_lin_get(data, SIL_LIN_BODY_LIGHTCOMMAND_FADE_BYTE, SIL_LIN_BODY_LIGHTCOMMAND_FADE_BIT, 1))) {
			s->light_target = target;
			s->fade = sil_lin_get(data, SIL_LIN_BODY_LIGHTCOMMAND_FADE_BYTE, SIL_LIN_BODY_LIGHTCOMMAND_FADE_BIT, 1);
			s->commands_applied++;
		}
	}
}

void sil_lin_slave_on_wakeup(sil_lin_slave_t *s, int64_t now_ns)
{
	s->wakeups++;
	s->asleep = false;
	s->last_header_ns = now_ns;
}

void sil_lin_slave_tick(sil_lin_slave_t *s, int64_t now_ns)
{
	if (!s->asleep && now_ns - s->last_header_ns >= (int64_t)SIL_LIN_SLAVE_BUS_IDLE_MS * NS_PER_MS)
		s->asleep = true;
}

int sil_lin_slave_control(sil_lin_slave_t *s, const char *cmd)
{
	char what[16] = {0}, arg[8] = {0};
	int v = -1, n = sscanf(cmd, "%15s %7s %d", what, arg, &v);

	if (n == 3 && !strcmp(what, "door")) {
		static const char *names[4] = {"FL", "FR", "RL", "RR"};
		for (int i = 0; i < 4; i++)
			if (!strcmp(arg, names[i])) {
				s->doors = (uint8_t)(v ? s->doors | (1u << i) : s->doors & ~(1u << i));
				return 0;
			}
		return -1;
	}
	if (n < 2 || sscanf(arg, "%d", &v) != 1 || (v != 0 && v != 1))
		return -1;
	if (!strcmp(what, "lock"))
		s->central_lock = v;
	else if (!strcmp(what, "childlock"))
		s->child_lock = v;
	else if (!strcmp(what, "obstruct")) {
		/* §6.3 obstruction injection: stop at once, keep the position, set the fault bit */
		s->obstruction = v;
		if (v) {
			s->target = s->position;
			s->motion = 0;
			s->window_fault = true;
		}
	} else if (!strcmp(what, "fault"))
		s->window_fault = s->light_fault = v;
	else if (!strcmp(what, "mute"))
		s->mute = v;
	else if (!strcmp(what, "corrupt"))
		s->corrupt = v;
	else
		return -1;
	return 0;
}
