// Gateway CAN input validation tests. Each case is one decision the guard must make.
#include <cstring>
#include <random>

#include "can_input_validator.hpp"
#include "sil/crc8.h"
#include "sil_test.h"

using sil::gateway::CanInputValidator;

namespace {

const sil_pdu_def_t &ENGINE = sil_pdus[SIL_PDU_PT_ENGINESTATUS];
const sil_pdu_def_t &BRAKE = sil_pdus[SIL_PDU_CH_BRAKESTATUS];

can_frame make_frame(const sil_pdu_def_t &pdu, std::uint8_t counter, std::int32_t first_signal = 0, bool valid = true)
{
	std::int32_t raw[SIL_MAX_SIGNALS] = {};
	can_frame fr{};

	raw[0] = first_signal;
	fr.can_id = pdu.can_id;
	fr.len = pdu.dlc;
	CHECK_EQ_INT(sil_pdu_encode(&pdu, raw, counter, sil_flags_t{valid, false, false}, fr.data), SIL_OK);
	return fr;
}

sil_status_t check(CanInputValidator &v, const can_frame &fr)
{
	sil_pdu_frame_t out{};
	return v.validate(fr, out);
}

// 정상 메시지: 통과하고 값이 그대로 나온다
void test_normal_frame()
{
	CanInputValidator v;
	sil_pdu_frame_t out{};

	CHECK_EQ_INT(v.validate(make_frame(ENGINE, 5, 800), out), SIL_OK);
	CHECK(out.pdu == &ENGINE);
	CHECK_EQ_INT(out.raw[SIL_PT_ENGINESTATUS_RPM], 800);
	CHECK_EQ_INT(out.counter, 5);
}

// 받는 곳이 틀림 / 상자 크기가 틀림
void test_unknown_id_and_length()
{
	CanInputValidator v;
	can_frame fr = make_frame(ENGINE, 5);

	fr.can_id = 0x123;
	CHECK_EQ_INT(check(v, fr), SIL_ERR_UNKNOWN_ID);
	fr = make_frame(ENGINE, 5);
	fr.len = 7;
	CHECK_EQ_INT(check(v, fr), SIL_ERR_DLC);
}

// 같은 번호가 또 오면 버린다
void test_duplicate_counter()
{
	CanInputValidator v;

	CHECK_EQ_INT(check(v, make_frame(ENGINE, 5)), SIL_OK);
	CHECK_EQ_INT(check(v, make_frame(ENGINE, 5)), SIL_ERR_COUNTER_DUPLICATE);
	CHECK_EQ_INT(check(v, make_frame(ENGINE, 6)), SIL_OK);
}

// 망가진 메시지의 번호는 믿지 않는다: 장부는 5 그대로, 멀쩡한 6번은 통과
void test_crc_error_does_not_touch_counter()
{
	CanInputValidator v;
	can_frame broken = make_frame(ENGINE, 6);

	broken.data[0] ^= 0x01;
	CHECK_EQ_INT(check(v, make_frame(ENGINE, 5)), SIL_OK);
	CHECK_EQ_INT(check(v, broken), SIL_ERR_CRC);
	CHECK_EQ_INT(check(v, make_frame(ENGINE, 6)), SIL_OK);
	CHECK_EQ_INT(v.lost_frames(), 0);
}

// 번호가 조금 건너뛰면 받되 개수를 기록, 많이 건너뛰면 버리고 다시 맞춘다
void test_counter_gap()
{
	CanInputValidator v;

	CHECK_EQ_INT(check(v, make_frame(ENGINE, 5)), SIL_OK);
	CHECK_EQ_INT(check(v, make_frame(ENGINE, 7)), SIL_OK);
	CHECK_EQ_INT(v.lost_frames(), 1);
	CHECK_EQ_INT(check(v, make_frame(ENGINE, 12)), SIL_ERR_COUNTER_JUMP);
	CHECK_EQ_INT(check(v, make_frame(ENGINE, 13)), SIL_OK);
}

// 범위 밖 값은 CRC가 맞아도 버린다. 보낸 쪽은 살아 있으므로 장부는 앞으로 간다
void test_out_of_range()
{
	CanInputValidator v;
	can_frame fr = make_frame(ENGINE, 6);

	fr.data[0] = 0x23; // 9000 rpm
	fr.data[1] = 0x28;
	fr.data[SIL_CRC_BYTE] = sil_frame_crc(ENGINE.can_id, fr.data);
	CHECK_EQ_INT(check(v, fr), SIL_ERR_RANGE);
	CHECK_EQ_INT(check(v, make_frame(ENGINE, 7)), SIL_OK);
	CHECK_EQ_INT(v.lost_frames(), 0);
}

// 보낸 쪽이 valid=0이라고 하면 값을 쓰지 않는다. 장부는 앞으로 간다
void test_not_valid()
{
	CanInputValidator v;

	CHECK_EQ_INT(check(v, make_frame(ENGINE, 5, 800, false)), SIL_ERR_NOT_VALID);
	CHECK_EQ_INT(check(v, make_frame(ENGINE, 6)), SIL_OK);
	CHECK_EQ_INT(v.lost_frames(), 0);
}

// 메시지 종류마다 장부가 따로 있다
void test_counters_are_per_pdu()
{
	CanInputValidator v;

	CHECK_EQ_INT(check(v, make_frame(ENGINE, 5)), SIL_OK);
	CHECK_EQ_INT(check(v, make_frame(BRAKE, 5)), SIL_OK);
	CHECK_EQ_INT(check(v, make_frame(ENGINE, 6)), SIL_OK);
	CHECK_EQ_INT(check(v, make_frame(BRAKE, 6)), SIL_OK);
}

// 29-bit ID, RTR, error frame: 아래 11비트가 0x100이어도 엔진 메시지로 착각하면 안 된다
void test_not_classical_data_frame()
{
	const canid_t flags[] = {CAN_EFF_FLAG, CAN_RTR_FLAG, CAN_ERR_FLAG};

	for (canid_t flag : flags) {
		CanInputValidator v;
		can_frame fr = make_frame(ENGINE, 5);

		fr.can_id |= flag;
		CHECK_EQ_INT(check(v, fr), SIL_ERR_FRAME_FORMAT);
	}
}

// 알려진 한계: CRC와 번호를 맞춘 범위 안의 위조는 통과한다 (§11.3, R155 M10 진위성 미구현)
void test_forged_frame_is_accepted_known_limit()
{
	CanInputValidator v;
	sil_pdu_frame_t out{};

	CHECK_EQ_INT(check(v, make_frame(ENGINE, 5, 800)), SIL_OK);
	CHECK_EQ_INT(v.validate(make_frame(ENGINE, 6, 3000), out), SIL_OK);
	CHECK_EQ_INT(out.raw[SIL_PT_ENGINESTATUS_RPM], 3000);
}

// 무작위 입력: 죽지 않고, 통과한 값은 항상 범위 안
void test_random_frames()
{
	std::mt19937 rng(20261105);
	CanInputValidator v;

	for (int i = 0; i < 200000; i++) {
		can_frame fr{};
		sil_pdu_frame_t out{};

		fr.can_id = (i % 2) ? rng() : sil_pdus[rng() % SIL_PDU_COUNT].can_id;
		fr.len = static_cast<std::uint8_t>((i % 8 == 0) ? rng() % 16 : 8);
		for (auto &b : fr.data)
			b = static_cast<std::uint8_t>(rng());
		if (i % 2 == 0)
			fr.data[SIL_CRC_BYTE] = sil_frame_crc(static_cast<std::uint16_t>(fr.can_id), fr.data);

		if (v.validate(fr, out) == SIL_OK) {
			CHECK(out.flags.valid);
			for (std::uint8_t s = 0; s < out.pdu->signal_count; s++)
				CHECK(out.raw[s] >= out.pdu->signals[s].raw_min && out.raw[s] <= out.pdu->signals[s].raw_max);
		}
	}
}

} // namespace

int main()
{
	test_normal_frame();
	test_unknown_id_and_length();
	test_duplicate_counter();
	test_crc_error_does_not_touch_counter();
	test_counter_gap();
	test_out_of_range();
	test_not_valid();
	test_counters_are_per_pdu();
	test_not_classical_data_frame();
	test_forged_frame_is_accepted_known_limit();
	test_random_frames();
	return TEST_REPORT();
}
