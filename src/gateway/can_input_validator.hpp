// Gateway CAN input validation (project.md §5.1, §5.4, §5.5).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <linux/can.h>

#include "sil/pdu_codec.h"

namespace sil::gateway {

class CanInputValidator {
public:
	// Returns SIL_OK only when the frame may be used. On SIL_OK, out holds the decoded values.
	sil_status_t validate(const can_frame &frame, sil_pdu_frame_t &out);

	// Frames skipped between accepted counters (counter delta 2~3).
	std::uint64_t lost_frames() const { return lost_frames_; }

private:
	// Each PDU has its own counter ledger.
	sil_counter_state_t &counter_state(const sil_pdu_def_t *pdu)
	{
		return counters_[static_cast<std::size_t>(pdu - sil_pdus)];
	}

	std::array<sil_counter_state_t, SIL_PDU_COUNT> counters_{};
	std::uint64_t lost_frames_ = 0;
};

} // namespace sil::gateway
