#include "can_input_validator.hpp"

namespace sil::gateway {

// Order (project.md §5.5): frame format -> ID -> DLC -> CRC -> reserved -> counter -> valid -> range.
// CRC is checked before the counter so a corrupted frame never moves the counter ledger.
sil_status_t CanInputValidator::validate(const can_frame &frame, sil_pdu_frame_t &out)
{
	// 29-bit ID, RTR and error frames are not project PDUs (§5.1)
	if (frame.can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_ERR_FLAG))
		return SIL_ERR_FRAME_FORMAT;

	const auto can_id = static_cast<std::uint16_t>(frame.can_id & CAN_SFF_MASK);
	sil_status_t st = sil_pdu_unpack(can_id, frame.data, frame.len, &out);
	if (st != SIL_OK)
		return st;

	std::uint8_t lost = 0;
	st = sil_counter_check(&counter_state(out.pdu), out.counter, &lost);
	if (st != SIL_OK)
		return st;
	lost_frames_ += lost;

	// the sender is alive even when it marks data invalid, so the counter ledger has already moved
	return sil_pdu_check_values(&out);
}

} // namespace sil::gateway
