// LIN master of the Vehicle Gateway (project.md §6.1~6.3): owns the schedule table, sends headers and the
// master-published command frames, checks slave responses, tracks missing responses, sleep and wakeup.
// Pure: frames in and out, time passed in.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "sil/lin.h"

namespace sil::lin {

struct WireFrame {
	std::uint32_t can_id;
	std::vector<std::uint8_t> data;
};

enum class BusState { Running, Sleep, Waking };
enum class Slave { Door = 0, Window = 1, Light = 2 };

struct SlaveHealth {
	unsigned missed = 0;  // consecutive missing or corrupted responses
	unsigned ok = 0;      // consecutive checksum-valid responses
	bool degraded = false;
	std::uint64_t ok_total = 0, missed_total = 0, bad_checksum = 0, recoveries = 0;
	std::int64_t last_ok_ns = 0;
};

struct BodyStatus {
	std::uint8_t doors = 0;
	bool central_lock = false, child_lock = false, door_valid = false;
	std::uint8_t window_position = 0, window_motion = 0;
	bool obstruction = false, window_fault = false, window_valid = false;
	std::uint8_t brightness = 0;
	bool light_active = false, light_fault = false, light_valid = false;
	bool operator==(const BodyStatus &) const = default;
};

class LinMaster {
public:
	explicit LinMaster(std::int64_t now_ns);

	// Call often (about every millisecond). Returns the frames to put on the wire now.
	std::vector<WireFrame> tick(std::int64_t now_ns);
	// A frame read from the wire (slave responses).
	void on_frame(const WireFrame &f, std::int64_t now_ns);

	void command_window(std::uint8_t target, std::int64_t now_ns);
	void cancel_window(std::int64_t now_ns);
	void command_light(bool on, std::int64_t now_ns);

	BusState state() const { return state_; }
	const BodyStatus &status() const { return status_; }
	const SlaveHealth &health(Slave s) const { return health_[static_cast<int>(s)]; }
	bool fresh(Slave s, std::int64_t now_ns) const;
	std::int64_t newest_status_ns() const;

	// for BodyControl method confirmation
	std::int64_t window_cmd_tx_ns() const { return window_cmd_tx_ns_; }
	std::int64_t light_cmd_tx_ns() const { return light_cmd_tx_ns_; }
	std::int64_t status_ns(Slave s) const { return health_[static_cast<int>(s)].last_ok_ns; }

	std::uint64_t slot_overruns() const { return overruns_; }
	std::uint64_t unexpected_frames() const { return unexpected_; }
	std::uint64_t sleeps() const { return sleeps_; }

private:
	void emit_slot(std::int64_t now_ns, std::vector<WireFrame> &out);
	void finish_slot();
	void response_ok(Slave s, const std::uint8_t *payload, std::int64_t now_ns);
	void response_bad(Slave s);
	void wake(std::int64_t now_ns);

	BusState state_ = BusState::Running;
	unsigned slot_ = 0;
	std::int64_t slot_start_ns_;
	std::int64_t wake_at_ns_ = 0;
	bool wake_pending_ = false;
	std::int64_t last_activity_ns_;

	int awaiting_ = -1; // frame ID of the slave response expected in this slot
	std::uint8_t awaiting_pid_ = 0;
	std::int64_t awaiting_since_ns_ = 0;

	std::optional<std::uint8_t> window_target_;
	bool window_cancel_ = false, window_cmd_new_ = false;
	std::uint8_t light_target_ = 0;
	bool light_cmd_new_ = false;
	std::int64_t window_cmd_tx_ns_ = 0, light_cmd_tx_ns_ = 0;

	BodyStatus status_;
	std::array<SlaveHealth, 3> health_{};
	std::uint64_t overruns_ = 0, unexpected_ = 0, sleeps_ = 0;
};

} // namespace sil::lin
