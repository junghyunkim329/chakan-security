#include "lin_master.hpp"

#include <algorithm>

namespace sil::lin {

namespace {

constexpr std::int64_t kMs = 1'000'000;
constexpr std::int64_t kTick = SIL_LIN_TICK_MS * kMs;

int slave_of(std::uint8_t frame_id)
{
	switch (frame_id) {
	case SIL_LIN_BODY_DOORSTATUS_ID: return static_cast<int>(Slave::Door);
	case SIL_LIN_BODY_WINDOWSTATUS_ID: return static_cast<int>(Slave::Window);
	case SIL_LIN_BODY_LIGHTSTATUS_ID: return static_cast<int>(Slave::Light);
	default: return -1;
	}
}

// publish period of each slave status in the 100 ms schedule
constexpr std::int64_t kPeriodNs[3] = {100 * kMs, 50 * kMs, 100 * kMs};

} // namespace

// the first tick advances to slot 0, so the schedule starts with the 0 ms slot
LinMaster::LinMaster(std::int64_t now_ns)
	: slot_(SIL_LIN_SCHEDULE_SLOTS - 1), slot_start_ns_(now_ns - kTick), last_activity_ns_(now_ns)
{
}

void LinMaster::wake(std::int64_t now_ns)
{
	last_activity_ns_ = now_ns;
	if (state_ == BusState::Sleep)
		wake_pending_ = true;
}

void LinMaster::command_window(std::uint8_t target, std::int64_t now_ns)
{
	window_target_ = target;
	window_cancel_ = false;
	window_cmd_new_ = true;
	wake(now_ns);
}

void LinMaster::cancel_window(std::int64_t now_ns)
{
	window_cancel_ = true;
	window_target_.reset();
	window_cmd_new_ = true;
	wake(now_ns);
}

void LinMaster::command_light(bool on, std::int64_t now_ns)
{
	light_target_ = on ? 100 : 0;
	light_cmd_new_ = true;
	wake(now_ns);
}

std::vector<WireFrame> LinMaster::tick(std::int64_t now_ns)
{
	std::vector<WireFrame> out;

	if (state_ == BusState::Sleep) {
		if (wake_pending_) {
			// §6.2 master wakeup event; the first header follows after the wakeup delay
			wake_pending_ = false;
			state_ = BusState::Waking;
			wake_at_ns_ = now_ns + SIL_LIN_WAKEUP_DELAY_MS * kMs;
			out.push_back({SIL_LIN_WAKEUP_CAN_ID, {}});
		}
		return out;
	}
	if (state_ == BusState::Waking) {
		if (now_ns < wake_at_ns_)
			return out;
		// §6.3: restart at the 0 ms slot of the next major cycle
		state_ = BusState::Running;
		slot_ = 0;
		slot_start_ns_ = now_ns;
		last_activity_ns_ = now_ns;
		emit_slot(now_ns, out);
		return out;
	}

	if (now_ns < slot_start_ns_ + kTick)
		return out;
	finish_slot();
	slot_ = (slot_ + 1) % SIL_LIN_SCHEDULE_SLOTS;
	slot_start_ns_ += kTick;
	if (now_ns - slot_start_ns_ >= kTick) { // the thread was late: keep the schedule order, restart the timing
		overruns_++;
		slot_start_ns_ = now_ns;
	}
	// §6.2: no command and no status change for 5 s -> sleep (checked at the start of a major cycle)
	if (slot_ == 0 && now_ns - last_activity_ns_ >= SIL_LIN_SLEEP_AFTER_MS * kMs) {
		state_ = BusState::Sleep;
		sleeps_++;
		return out;
	}
	emit_slot(now_ns, out);
	return out;
}

void LinMaster::emit_slot(std::int64_t now_ns, std::vector<WireFrame> &out)
{
	const std::uint8_t id = sil_lin_schedule[slot_];
	if (id == SIL_LIN_IDLE_SLOT)
		return;
	const sil_lin_frame_t *f = sil_lin_frame(id);
	out.push_back({SIL_LIN_HEADER_CAN_ID, {f->pid}});

	if (f->publisher != SIL_LIN_NODE_GATEWAY_MASTER) {
		awaiting_ = id;
		awaiting_pid_ = f->pid;
		awaiting_since_ns_ = now_ns;
		return;
	}
	std::vector<std::uint8_t> p(f->dlc, 0);
	if (id == SIL_LIN_BODY_WINDOWCOMMAND_ID) {
		p[SIL_LIN_BODY_WINDOWCOMMAND_TARGET_BYTE] = window_target_.value_or(0);
		sil_lin_put(p.data(), SIL_LIN_BODY_WINDOWCOMMAND_EXECUTE_BYTE, SIL_LIN_BODY_WINDOWCOMMAND_EXECUTE_BIT, 1,
			    window_target_.has_value());
		sil_lin_put(p.data(), SIL_LIN_BODY_WINDOWCOMMAND_CANCEL_BYTE, SIL_LIN_BODY_WINDOWCOMMAND_CANCEL_BIT, 1, window_cancel_);
		window_cancel_ = false; // cancel is a one-shot request
		if (window_cmd_new_) {
			window_cmd_new_ = false;
			window_cmd_tx_ns_ = now_ns;
		}
	} else if (id == SIL_LIN_BODY_LIGHTCOMMAND_ID) {
		p[SIL_LIN_BODY_LIGHTCOMMAND_TARGET_BYTE] = light_target_;
		sil_lin_put(p.data(), SIL_LIN_BODY_LIGHTCOMMAND_FADE_BYTE, SIL_LIN_BODY_LIGHTCOMMAND_FADE_BIT, 1, 1);
		if (light_cmd_new_) {
			light_cmd_new_ = false;
			light_cmd_tx_ns_ = now_ns;
		}
	}
	p.push_back(sil_lin_checksum(f->pid, p.data(), f->dlc));
	out.push_back({SIL_LIN_RESPONSE_CAN_ID, std::move(p)});
}

void LinMaster::finish_slot()
{
	if (awaiting_ >= 0)
		response_bad(static_cast<Slave>(slave_of(static_cast<std::uint8_t>(awaiting_)))); // no response in the slot
	awaiting_ = -1;
}

void LinMaster::on_frame(const WireFrame &f, std::int64_t now_ns)
{
	if (f.can_id != SIL_LIN_RESPONSE_CAN_ID || awaiting_ < 0 || now_ns - awaiting_since_ns_ > SIL_LIN_RESPONSE_WINDOW_MS * kMs) {
		unexpected_++;
		return;
	}
	const sil_lin_frame_t *fr = sil_lin_frame(static_cast<std::uint8_t>(awaiting_));
	const Slave s = static_cast<Slave>(slave_of(fr->id));
	awaiting_ = -1;
	if (f.data.size() != fr->dlc + 1u || f.data[fr->dlc] != sil_lin_checksum(awaiting_pid_, f.data.data(), fr->dlc)) {
		health_[static_cast<int>(s)].bad_checksum++;
		response_bad(s);
		return;
	}
	response_ok(s, f.data.data(), now_ns);
}

void LinMaster::response_bad(Slave s)
{
	SlaveHealth &h = health_[static_cast<int>(s)];
	h.ok = 0;
	h.missed++;
	h.missed_total++;
	if (h.missed >= SIL_LIN_MISSED_LIMIT)
		h.degraded = true; // §6.2 / §6.3 three consecutive missing responses
}

void LinMaster::response_ok(Slave s, const std::uint8_t *p, std::int64_t now_ns)
{
	SlaveHealth &h = health_[static_cast<int>(s)];
	h.missed = 0;
	h.ok++;
	h.ok_total++;
	h.last_ok_ns = now_ns;
	if (h.degraded && h.ok >= SIL_LIN_RECOVERY_COUNT) { // §6.3 three checksum-valid responses -> normal
		h.degraded = false;
		h.recoveries++;
	}

	const BodyStatus before = status_;
	switch (s) {
	case Slave::Door:
		status_.doors = p[0] & 0x0F;
		status_.central_lock = sil_lin_get(p, SIL_LIN_BODY_DOORSTATUS_CENTRAL_LOCK_BYTE, SIL_LIN_BODY_DOORSTATUS_CENTRAL_LOCK_BIT, 1);
		status_.child_lock = sil_lin_get(p, SIL_LIN_BODY_DOORSTATUS_CHILD_LOCK_BYTE, SIL_LIN_BODY_DOORSTATUS_CHILD_LOCK_BIT, 1);
		status_.door_valid = sil_lin_get(p, SIL_LIN_BODY_DOORSTATUS_VALID_BYTE, SIL_LIN_BODY_DOORSTATUS_VALID_BIT, 1);
		break;
	case Slave::Window:
		status_.window_position = p[SIL_LIN_BODY_WINDOWSTATUS_POSITION_BYTE];
		status_.window_motion = p[SIL_LIN_BODY_WINDOWSTATUS_MOTION_BYTE];
		status_.obstruction = sil_lin_get(p, SIL_LIN_BODY_WINDOWSTATUS_OBSTRUCTION_BYTE, SIL_LIN_BODY_WINDOWSTATUS_OBSTRUCTION_BIT, 1);
		status_.window_fault = sil_lin_get(p, SIL_LIN_BODY_WINDOWSTATUS_FAULT_BYTE, SIL_LIN_BODY_WINDOWSTATUS_FAULT_BIT, 1);
		status_.window_valid = sil_lin_get(p, SIL_LIN_BODY_WINDOWSTATUS_VALID_BYTE, SIL_LIN_BODY_WINDOWSTATUS_VALID_BIT, 1);
		break;
	case Slave::Light:
		status_.brightness = p[SIL_LIN_BODY_LIGHTSTATUS_BRIGHTNESS_BYTE];
		status_.light_active = sil_lin_get(p, SIL_LIN_BODY_LIGHTSTATUS_ACTIVE_BYTE, SIL_LIN_BODY_LIGHTSTATUS_ACTIVE_BIT, 1);
		status_.light_fault = sil_lin_get(p, SIL_LIN_BODY_LIGHTSTATUS_FAULT_BYTE, SIL_LIN_BODY_LIGHTSTATUS_FAULT_BIT, 1);
		status_.light_valid = sil_lin_get(p, SIL_LIN_BODY_LIGHTSTATUS_VALID_BYTE, SIL_LIN_BODY_LIGHTSTATUS_VALID_BIT, 1);
		break;
	}
	if (!(status_ == before))
		last_activity_ns_ = now_ns; // a status change keeps the bus awake
}

bool LinMaster::fresh(Slave s, std::int64_t now_ns) const
{
	const SlaveHealth &h = health_[static_cast<int>(s)];
	return state_ == BusState::Running && !h.degraded && h.last_ok_ns > 0 &&
	       now_ns - h.last_ok_ns < 3 * kPeriodNs[static_cast<int>(s)];
}

std::int64_t LinMaster::newest_status_ns() const
{
	std::int64_t t = 0;
	for (auto &h : health_)
		t = std::max(t, h.last_ok_ns);
	return t;
}

} // namespace sil::lin
