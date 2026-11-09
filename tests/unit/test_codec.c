/* PDU codec tests: normal values, boundaries, endian/sign, and invalid data (project.md §5, §15.1). */
#include <string.h>

#include "sil/crc8.h"
#include "sil/pdu_codec.h"
#include "sil_test.h"

static const sil_flags_t VALID = {.valid = true};

static void fill_all(const sil_pdu_def_t *pdu, int32_t *raw, int which)
{
	for (uint8_t i = 0; i < pdu->signal_count; i++) {
		const sil_sig_def_t *s = &pdu->signals[i];
		raw[i] = which < 0 ? s->raw_min : which > 0 ? s->raw_max : s->raw_min + (s->raw_max - s->raw_min) / 2;
	}
}

static void roundtrip(const sil_pdu_def_t *pdu, const int32_t *raw, uint8_t counter, sil_flags_t flags)
{
	uint8_t data[8];
	sil_pdu_frame_t fr;

	CHECK_EQ_INT(sil_pdu_encode(pdu, raw, counter, flags, data), SIL_OK);
	CHECK_EQ_INT(sil_pdu_unpack(pdu->can_id, data, 8, &fr), SIL_OK);
	CHECK(fr.pdu == pdu);
	CHECK_EQ_INT(fr.counter, counter);
	CHECK_EQ_INT(fr.flags.valid, flags.valid);
	CHECK_EQ_INT(fr.flags.degraded, flags.degraded);
	CHECK_EQ_INT(fr.flags.fault, flags.fault);
	for (uint8_t i = 0; i < pdu->signal_count; i++)
		CHECK_EQ_INT(fr.raw[i], raw[i]);
	CHECK_EQ_INT(sil_pdu_check_values(&fr), flags.valid ? SIL_OK : SIL_ERR_NOT_VALID);
}

/* normal and boundary values for every PDU in the definition */
static void test_all_pdus_boundaries(void)
{
	for (unsigned p = 0; p < SIL_PDU_COUNT; p++) {
		const sil_pdu_def_t *pdu = &sil_pdus[p];
		int32_t raw[SIL_MAX_SIGNALS];
		uint8_t data[8];

		for (int which = -1; which <= 1; which++) {
			fill_all(pdu, raw, which);
			for (uint8_t c = 0; c < SIL_COUNTER_MODULO; c++)
				roundtrip(pdu, raw, c, VALID);
			roundtrip(pdu, raw, 5, (sil_flags_t){.valid = true, .degraded = true, .fault = true});
		}

		/* one step outside min/max is refused by the sender */
		for (uint8_t i = 0; i < pdu->signal_count; i++) {
			fill_all(pdu, raw, 0);
			raw[i] = pdu->signals[i].raw_min - 1;
			CHECK_EQ_INT(sil_pdu_encode(pdu, raw, 0, VALID, data), SIL_ERR_RANGE);
			raw[i] = pdu->signals[i].raw_max + 1;
			CHECK_EQ_INT(sil_pdu_encode(pdu, raw, 0, VALID, data), SIL_ERR_RANGE);
		}

		fill_all(pdu, raw, 0);
		CHECK_EQ_INT(sil_pdu_encode(pdu, raw, 16, VALID, data), SIL_ERR_ARG);
	}
}

/* known byte layout: big-endian and two's complement */
static void test_known_bytes(void)
{
	const sil_pdu_def_t *eng = &sil_pdus[SIL_PDU_PT_ENGINESTATUS];
	const sil_pdu_def_t *str = &sil_pdus[SIL_PDU_CH_STEERINGSTATUS];
	int32_t raw[SIL_MAX_SIGNALS] = {0};
	uint8_t d[8];

	raw[SIL_PT_ENGINESTATUS_RPM] = 0x1234;      /* 4660 rpm */
	raw[SIL_PT_ENGINESTATUS_THROTTLE] = 200;    /* 100 % */
	raw[SIL_PT_ENGINESTATUS_COOLANT] = -400;    /* -40.0 degC */
	raw[SIL_PT_ENGINESTATUS_IGNITION_ON] = 1;
	raw[SIL_PT_ENGINESTATUS_ENGINE_RUNNING] = 1;
	CHECK_EQ_INT(sil_pdu_encode(eng, raw, 0xA, VALID, d), SIL_OK);
	CHECK_EQ_INT(d[0], 0x12);
	CHECK_EQ_INT(d[1], 0x34);
	CHECK_EQ_INT(d[2], 0xC8);
	CHECK_EQ_INT(d[3], 0xFE);
	CHECK_EQ_INT(d[4], 0x70);
	CHECK_EQ_INT(d[5], 0x03);       /* ignition + running */
	CHECK_EQ_INT(d[6], 0xA8);       /* counter 0xA << 4 | valid bit3 */
	CHECK_EQ_INT(d[7], sil_frame_crc(0x100, d));

	memset(raw, 0, sizeof(raw));
	raw[SIL_CH_STEERINGSTATUS_ANGLE] = -7800;        /* -780.0 deg */
	raw[SIL_CH_STEERINGSTATUS_ASSIST_TORQUE] = -120; /* -12.0 Nm */
	CHECK_EQ_INT(sil_pdu_encode(str, raw, 0, VALID, d), SIL_OK);
	CHECK_EQ_INT(d[0], 0xE1);
	CHECK_EQ_INT(d[1], 0x88);
	CHECK_EQ_INT(d[4], 0x88);

	/* physical conversion */
	CHECK_EQ_INT(sil_phys_to_raw(&eng->signals[SIL_PT_ENGINESTATUS_COOLANT], 90.0), 900);
	CHECK_EQ_INT(sil_phys_to_raw(&eng->signals[SIL_PT_ENGINESTATUS_THROTTLE], 37.5), 75);
}

static void valid_frame(const sil_pdu_def_t *pdu, uint8_t d[8])
{
	int32_t raw[SIL_MAX_SIGNALS];

	fill_all(pdu, raw, 0);
	CHECK_EQ_INT(sil_pdu_encode(pdu, raw, 3, VALID, d), SIL_OK);
}

/* wrong data must be rejected in the order ID -> DLC -> CRC -> reserved -> valid -> range */
static void test_invalid_data(void)
{
	const sil_pdu_def_t *brk = &sil_pdus[SIL_PDU_CH_BRAKESTATUS];
	const sil_pdu_def_t *eng = &sil_pdus[SIL_PDU_PT_ENGINESTATUS];
	const sil_pdu_def_t *dyn = &sil_pdus[SIL_PDU_PT_VEHICLEDYNAMICS];
	sil_pdu_frame_t fr;
	uint8_t d[8];

	valid_frame(brk, d);
	CHECK_EQ_INT(sil_pdu_unpack(0x123, d, 8, &fr), SIL_ERR_UNKNOWN_ID);
	CHECK_EQ_INT(sil_pdu_unpack(0x7FF, d, 8, &fr), SIL_ERR_UNKNOWN_ID);
	CHECK_EQ_INT(sil_pdu_unpack(brk->can_id, d, 7, &fr), SIL_ERR_DLC);
	CHECK_EQ_INT(sil_pdu_unpack(brk->can_id, d, 0, &fr), SIL_ERR_DLC);

	/* any single bit flip in byte 0..7 is caught */
	for (int byte = 0; byte < 8; byte++) {
		for (int bit = 0; bit < 8; bit++) {
			valid_frame(brk, d);
			d[byte] ^= (uint8_t)(1u << bit);
			CHECK_EQ_INT(sil_pdu_unpack(brk->can_id, d, 8, &fr), SIL_ERR_CRC);
		}
	}
	/* frame valid for 0x180 but presented as 0x100 */
	valid_frame(brk, d);
	CHECK_EQ_INT(sil_pdu_unpack(eng->can_id, d, 8, &fr), SIL_ERR_CRC);

	/* reserved byte 4 of 0x180 set, CRC recomputed */
	valid_frame(brk, d);
	d[4] = 0x01;
	d[7] = sil_frame_crc(brk->can_id, d);
	CHECK_EQ_INT(sil_pdu_unpack(brk->can_id, d, 8, &fr), SIL_ERR_RESERVED);
	/* reserved bits 4..7 of the 0x180 status byte */
	valid_frame(brk, d);
	d[3] |= 0x80;
	d[7] = sil_frame_crc(brk->can_id, d);
	CHECK_EQ_INT(sil_pdu_unpack(brk->can_id, d, 8, &fr), SIL_ERR_RESERVED);
	/* 0x110 byte 5 bit 0 is reserved */
	valid_frame(dyn, d);
	d[5] |= 0x01;
	d[7] = sil_frame_crc(dyn->can_id, d);
	CHECK_EQ_INT(sil_pdu_unpack(dyn->can_id, d, 8, &fr), SIL_ERR_RESERVED);
	/* byte 6 bit 0 is reserved */
	valid_frame(brk, d);
	d[6] |= 0x01;
	d[7] = sil_frame_crc(brk->can_id, d);
	CHECK_EQ_INT(sil_pdu_unpack(brk->can_id, d, 8, &fr), SIL_ERR_RESERVED);

	/* data valid = 0 */
	{
		int32_t raw[SIL_MAX_SIGNALS];

		fill_all(brk, raw, 0);
		CHECK_EQ_INT(sil_pdu_encode(brk, raw, 0, (sil_flags_t){0}, d), SIL_OK);
		CHECK_EQ_INT(sil_pdu_unpack(brk->can_id, d, 8, &fr), SIL_OK);
		CHECK_EQ_INT(sil_pdu_check_values(&fr), SIL_ERR_NOT_VALID);
	}

	/* out of physical range with a correct CRC (attacker or faulty sender) */
	valid_frame(eng, d);
	d[0] = 0x1F; /* rpm 8001 */
	d[1] = 0x41;
	d[7] = sil_frame_crc(eng->can_id, d);
	CHECK_EQ_INT(sil_pdu_unpack(eng->can_id, d, 8, &fr), SIL_OK);
	CHECK_EQ_INT(sil_pdu_check_values(&fr), SIL_ERR_RANGE);

	valid_frame(dyn, d);
	d[4] = 3; /* direction 3 undefined */
	d[7] = sil_frame_crc(dyn->can_id, d);
	CHECK_EQ_INT(sil_pdu_unpack(dyn->can_id, d, 8, &fr), SIL_OK);
	CHECK_EQ_INT(sil_pdu_check_values(&fr), SIL_ERR_RANGE);

	valid_frame(eng, d);
	d[2] = 201; /* throttle 100.5 % */
	d[7] = sil_frame_crc(eng->can_id, d);
	CHECK_EQ_INT(sil_pdu_unpack(eng->can_id, d, 8, &fr), SIL_OK);
	CHECK_EQ_INT(sil_pdu_check_values(&fr), SIL_ERR_RANGE);
}

static void test_counter(void)
{
	sil_counter_state_t st = {0};
	uint8_t lost;

	CHECK_EQ_INT(sil_counter_check(&st, 14, &lost), SIL_OK); /* first frame */
	CHECK_EQ_INT(sil_counter_check(&st, 15, &lost), SIL_OK);
	CHECK_EQ_INT(lost, 0);
	CHECK_EQ_INT(sil_counter_check(&st, 0, &lost), SIL_OK);  /* wrap 15 -> 0 */
	CHECK_EQ_INT(lost, 0);
	CHECK_EQ_INT(sil_counter_check(&st, 0, &lost), SIL_ERR_COUNTER_DUPLICATE);
	CHECK_EQ_INT(sil_counter_check(&st, 2, &lost), SIL_OK);  /* one lost */
	CHECK_EQ_INT(lost, 1);
	CHECK_EQ_INT(sil_counter_check(&st, 5, &lost), SIL_OK);  /* delta 3 = max */
	CHECK_EQ_INT(lost, 2);
	CHECK_EQ_INT(sil_counter_check(&st, 9, &lost), SIL_ERR_COUNTER_JUMP); /* delta 4 */
	CHECK_EQ_INT(sil_counter_check(&st, 10, &lost), SIL_OK); /* resynchronized */
	CHECK_EQ_INT(sil_counter_check(&st, 8, &lost), SIL_ERR_COUNTER_JUMP); /* reverse */
	CHECK_EQ_INT(sil_counter_check(&st, 9, &lost), SIL_OK);
	CHECK_EQ_INT(sil_counter_check(&st, 16, &lost), SIL_ERR_ARG);
}

int main(void)
{
	test_all_pdus_boundaries();
	test_known_bytes();
	test_invalid_data();
	test_counter();
	return TEST_REPORT();
}
