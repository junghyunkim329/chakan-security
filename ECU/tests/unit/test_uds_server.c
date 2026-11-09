/* UDS server tests: normal services, permission, timing (S3, security delay, lockout), multi-DID (project.md §9). */
#include <string.h>

#include "sil/uds_server.h"
#include "sil_test.h"

#define MS 1000000LL
#define S (1000 * MS)

static uint8_t g_rand;
static int fake_random(uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++)
		buf[i] = (uint8_t)(0x10 + g_rand + i);
	g_rand++;
	return 0;
}

static int fake_did(void *ctx, uint16_t did, uint8_t *out, size_t cap)
{
	(void)ctx;
	if (cap < 2)
		return -1;
	if (did == SIL_DID_ENGINE_SPEED) {
		out[0] = 0x03; /* 800 rpm */
		out[1] = 0x20;
		return 2;
	}
	if (did == SIL_DID_COOLANT_TEMP) {
		out[0] = 0xFE; /* -40.0 degC */
		out[1] = 0x70;
		return 2;
	}
	return -1;
}

static uint8_t KEY[32];
static uint8_t resp[4096];
static size_t rlen;

static size_t req(sil_uds_server_t *s, int64_t t, size_t n, const uint8_t *r)
{
	rlen = sil_uds_handle(s, r, n, t, resp, sizeof(resp));
	return rlen;
}
#define REQ(s, t, ...) req((s), (t), sizeof((const uint8_t[]){__VA_ARGS__}), (const uint8_t[]){__VA_ARGS__})

static int is_nrc(uint8_t sid, uint8_t code)
{
	return rlen == 3 && resp[0] == 0x7F && resp[1] == sid && resp[2] == code;
}

static void setup(sil_uds_server_t *s)
{
	for (int i = 0; i < 32; i++)
		KEY[i] = (uint8_t)i;
	g_rand = 0;
	sil_uds_init(s, &sil_diag_ecus[SIL_DIAG_ECU_ENGINE], KEY, fake_did, NULL, fake_random);
}

/* extended session + SecurityAccess with the correct key */
static void unlock(sil_uds_server_t *s, int64_t t)
{
	uint8_t k[34] = {0x27, 0x02};

	REQ(s, t, 0x10, 0x03);
	REQ(s, t, 0x27, 0x01);
	CHECK(rlen == 18 && resp[0] == 0x67);
	CHECK_EQ_INT(sil_uds_compute_key(KEY, 0x1001, resp + 2, k + 2), 0);
	req(s, t, sizeof(k), k);
	CHECK(rlen == 2 && resp[0] == 0x67 && resp[1] == 0x02);
}

static void bad_key(sil_uds_server_t *s, int64_t t)
{
	uint8_t k[34] = {0x27, 0x02};

	REQ(s, t, 0x27, 0x01);
	memset(k + 2, 0xAB, 32);
	req(s, t, sizeof(k), k);
}

static void test_hmac_vectors(void)
{
	static const uint8_t expect[32] = {0xec, 0x3a, 0x3c, 0x73, 0x03, 0x23, 0xa6, 0xb6, 0xa9, 0xe7, 0x06, 0x0d, 0xc2, 0xcb, 0x12, 0xa4,
					   0x81, 0xcc, 0x23, 0xe3, 0x0d, 0xc9, 0x70, 0xfc, 0x50, 0xe8, 0x6f, 0x6b, 0x22, 0x22, 0x53, 0xab};
	uint8_t key[32], seed[16], out[32];

	for (int i = 0; i < 32; i++)
		key[i] = (uint8_t)i;
	for (int i = 0; i < 16; i++)
		seed[i] = (uint8_t)(0x10 + i);
	CHECK_EQ_INT(sil_uds_compute_key(key, 0x1001, seed, out), 0);
	CHECK(memcmp(out, expect, 32) == 0); /* matches Python hmac.new(key, la||seed, sha256) */
	CHECK_EQ_INT(sil_uds_compute_key(key, 0x1002, seed, out), 0);
	CHECK(memcmp(out, expect, 32) != 0); /* the logical address binds the response to one ECU */
}

/* §9.4 Default: status and DTC reading only */
static void test_default_session(void)
{
	sil_uds_server_t s;

	setup(&s);
	sil_uds_set_dtc_failed(&s, 0);
	REQ(&s, 0, 0x22, 0xF1, 0x00);
	CHECK(rlen == 5 && resp[0] == 0x62 && resp[1] == 0xF1 && resp[2] == 0x00 && resp[3] == 0x03 && resp[4] == 0x20);
	REQ(&s, 0, 0x19, 0x02, 0xFF);
	CHECK(rlen == 7 && resp[0] == 0x59 && resp[3] == 0x01 && resp[4] == 0x17 && resp[5] == 0x00 && resp[6] == 0x09);
	REQ(&s, 0, 0x19, 0x01, 0x09);
	CHECK(rlen == 6 && resp[5] == 1);
	REQ(&s, 0, 0x3E, 0x00);
	CHECK(rlen == 2 && resp[0] == 0x7E);
	REQ(&s, 0, 0x3E, 0x80);
	CHECK_EQ_INT(rlen, 0); /* suppressed positive response */

	REQ(&s, 0, 0x14, 0xFF, 0xFF, 0xFF);
	CHECK(is_nrc(0x14, SIL_NRC_SERVICE_NOT_IN_SESSION));
	REQ(&s, 0, 0x11, 0x01);
	CHECK(is_nrc(0x11, SIL_NRC_SERVICE_NOT_IN_SESSION));
	REQ(&s, 0, 0x27, 0x01);
	CHECK(is_nrc(0x27, SIL_NRC_SERVICE_NOT_IN_SESSION));
}

/* Extended without SecurityAccess: reset and clear are denied (0x33) */
static void test_extended_needs_security(void)
{
	sil_uds_server_t s;

	setup(&s);
	REQ(&s, 0, 0x10, 0x03);
	CHECK(rlen == 6 && resp[0] == 0x50 && resp[1] == 0x03 && resp[3] == 50 && resp[4] == 0x01 && resp[5] == 0xF4);
	REQ(&s, 0, 0x14, 0xFF, 0xFF, 0xFF);
	CHECK(is_nrc(0x14, SIL_NRC_SECURITY_ACCESS_DENIED));
	REQ(&s, 0, 0x11, 0x01);
	CHECK(is_nrc(0x11, SIL_NRC_SECURITY_ACCESS_DENIED));
}

static void test_unlock_clear_reset(void)
{
	sil_uds_server_t s;

	setup(&s);
	sil_uds_set_dtc_failed(&s, 0);
	sil_uds_set_dtc_failed(&s, 1);
	unlock(&s, 0);
	REQ(&s, 0, 0x22, 0xF1, 0x81);
	CHECK(rlen == 5 && resp[3] == 0x03 && resp[4] == SIL_UDS_SEC_UNLOCKED);
	REQ(&s, 0, 0x27, 0x01); /* already unlocked: zero seed */
	CHECK(rlen == 18 && resp[2] == 0 && resp[17] == 0);
	REQ(&s, 0, 0x22, 0xF1, 0x82);
	CHECK(rlen == 4 && resp[3] == 0x03);
	REQ(&s, 0, 0x14, 0x00, 0x00, 0x01);
	CHECK(is_nrc(0x14, SIL_NRC_REQUEST_OUT_OF_RANGE));
	REQ(&s, 0, 0x14, 0xFF, 0xFF, 0xFF);
	CHECK(rlen == 1 && resp[0] == 0x54);
	REQ(&s, 0, 0x19, 0x02, 0xFF);
	CHECK_EQ_INT(rlen, 3); /* no DTCs left */
	REQ(&s, 0, 0x11, 0x01);
	CHECK(rlen == 2 && resp[0] == 0x51);
	REQ(&s, 0, 0x22, 0xF1, 0x81);
	CHECK(resp[3] == SIL_UDS_SESSION_DEFAULT && resp[4] == SIL_UDS_SEC_LOCKED); /* reset -> default, locked */
}

/* wrong key x3 -> 10 s delay; x5 -> lockout until the process restarts (§9.4) */
static void test_security_attempts(void)
{
	sil_uds_server_t s;
	uint8_t k[34] = {0x27, 0x02};

	setup(&s);
	REQ(&s, 0, 0x10, 0x03);
	req(&s, 0, sizeof(k), k);
	CHECK(is_nrc(0x27, SIL_NRC_REQUEST_SEQUENCE_ERROR)); /* sendKey without seed */
	REQ(&s, 0, 0x27, 0x02, 0x00);
	CHECK(is_nrc(0x27, SIL_NRC_INCORRECT_LENGTH));

	bad_key(&s, 1 * S);
	CHECK(is_nrc(0x27, SIL_NRC_INVALID_KEY));
	bad_key(&s, 2 * S);
	CHECK(is_nrc(0x27, SIL_NRC_INVALID_KEY));
	bad_key(&s, 3 * S);
	CHECK(is_nrc(0x27, SIL_NRC_EXCEEDED_ATTEMPTS));
	REQ(&s, 3 * S + 1, 0x27, 0x01);
	CHECK(is_nrc(0x27, SIL_NRC_DELAY_NOT_EXPIRED));
	REQ(&s, 3 * S + 1, 0x22, 0xF1, 0x81);
	CHECK(resp[4] == SIL_UDS_SEC_DELAY);
	/* keep the session alive with TesterPresent through the 10 s delay */
	for (int64_t t = 4 * S; t < 13 * S; t += 2 * S)
		REQ(&s, t, 0x3E, 0x80);
	REQ(&s, 13 * S - 1, 0x27, 0x01);
	CHECK(is_nrc(0x27, SIL_NRC_DELAY_NOT_EXPIRED));
	REQ(&s, 13 * S, 0x27, 0x01);
	CHECK(rlen == 18);

	bad_key(&s, 14 * S);
	CHECK(is_nrc(0x27, SIL_NRC_INVALID_KEY)); /* 4th */
	bad_key(&s, 15 * S);
	CHECK(is_nrc(0x27, SIL_NRC_EXCEEDED_ATTEMPTS)); /* 5th -> lockout */
	REQ(&s, 60 * S, 0x10, 0x03);
	REQ(&s, 60 * S, 0x27, 0x01);
	CHECK(is_nrc(0x27, SIL_NRC_EXCEEDED_ATTEMPTS)); /* still locked much later */
	REQ(&s, 60 * S, 0x22, 0xF1, 0x81);
	CHECK(resp[4] == SIL_UDS_SEC_LOCKOUT);

	/* a correct key cannot be used after lockout either: restart = new server state */
	setup(&s);
	unlock(&s, 0);
}

/* a seed is valid for one sendKey only, and a wrong key does not unlock */
static void test_seed_single_use(void)
{
	sil_uds_server_t s;
	uint8_t k[34] = {0x27, 0x02};
	uint8_t seed[16];

	setup(&s);
	REQ(&s, 0, 0x10, 0x03);
	REQ(&s, 0, 0x27, 0x01);
	memcpy(seed, resp + 2, 16);
	sil_uds_compute_key(KEY, 0x1001, seed, k + 2);
	req(&s, 0, sizeof(k), k);
	CHECK(rlen == 2);
	REQ(&s, 0, 0x10, 0x03); /* session change re-locks */
	req(&s, 0, sizeof(k), k); /* replay of the old key */
	CHECK(is_nrc(0x27, SIL_NRC_REQUEST_SEQUENCE_ERROR));
	REQ(&s, 0, 0x27, 0x01);
	CHECK(memcmp(resp + 2, seed, 16) != 0); /* fresh seed */
	req(&s, 0, sizeof(k), k);                /* old key against new seed */
	CHECK(is_nrc(0x27, SIL_NRC_INVALID_KEY));
	REQ(&s, 0, 0x14, 0xFF, 0xFF, 0xFF);
	CHECK(is_nrc(0x14, SIL_NRC_SECURITY_ACCESS_DENIED));
}

/* §9.4: no request for 5 s in Extended -> Default and locked; TesterPresent keeps it */
static void test_s3_timeout(void)
{
	sil_uds_server_t s;

	setup(&s);
	unlock(&s, 0);
	for (int64_t t = 1 * S; t <= 9 * S; t += 2 * S)
		REQ(&s, t, 0x3E, 0x80);
	sil_uds_tick(&s, 13 * S);
	CHECK_EQ_INT(s.session, SIL_UDS_SESSION_EXTENDED); /* 4 s since last TesterPresent */
	sil_uds_tick(&s, 14 * S);
	CHECK_EQ_INT(s.session, SIL_UDS_SESSION_DEFAULT);
	REQ(&s, 14 * S, 0x14, 0xFF, 0xFF, 0xFF);
	CHECK(is_nrc(0x14, SIL_NRC_SERVICE_NOT_IN_SESSION));

	/* a request arriving after the S3 window is handled in the Default session */
	unlock(&s, 20 * S);
	REQ(&s, 26 * S, 0x11, 0x01);
	CHECK(is_nrc(0x11, SIL_NRC_SERVICE_NOT_IN_SESSION));
}

/* request and response longer than one CAN frame (multi-frame on ISO-TP) */
static void test_multi_did(void)
{
	sil_uds_server_t s;
	size_t ver = strlen(sil_diag_ecus[SIL_DIAG_ECU_ENGINE].software_version);

	setup(&s);
	REQ(&s, 0, 0x22, 0xF1, 0x00, 0xF1, 0x02, 0xF1, 0x80, 0xF1, 0x81); /* 9-byte request */
	CHECK_EQ_INT(rlen, 1 + 4 + 4 + 2 + ver + 4);
	CHECK(resp[3] == 0x03 && resp[4] == 0x20);
	CHECK(resp[7] == 0xFE && resp[8] == 0x70);
	CHECK(memcmp(resp + 11, sil_diag_ecus[SIL_DIAG_ECU_ENGINE].software_version, ver) == 0);
	CHECK(ver > 7);
}

static void test_malformed(void)
{
	sil_uds_server_t s;

	setup(&s);
	REQ(&s, 0, 0x22, 0xF1, 0x01); /* vehicle speed is not an engine DID */
	CHECK(is_nrc(0x22, SIL_NRC_REQUEST_OUT_OF_RANGE));
	REQ(&s, 0, 0x22, 0xF1);
	CHECK(is_nrc(0x22, SIL_NRC_INCORRECT_LENGTH));
	REQ(&s, 0, 0x10, 0x02); /* programming session is out of scope */
	CHECK(is_nrc(0x10, SIL_NRC_SUBFUNCTION_NOT_SUPPORTED));
	REQ(&s, 0, 0x10);
	CHECK(is_nrc(0x10, SIL_NRC_INCORRECT_LENGTH));
	REQ(&s, 0, 0x34, 0x00, 0x44); /* RequestDownload not supported */
	CHECK(is_nrc(0x34, SIL_NRC_SERVICE_NOT_SUPPORTED));
	REQ(&s, 0, 0x19, 0x0A);
	CHECK(is_nrc(0x19, SIL_NRC_SUBFUNCTION_NOT_SUPPORTED));
	REQ(&s, 0, 0x3E, 0x01);
	CHECK(is_nrc(0x3E, SIL_NRC_SUBFUNCTION_NOT_SUPPORTED));
	CHECK_EQ_INT(sil_uds_handle(&s, (const uint8_t *)"", 0, 0, resp, sizeof(resp)), 0);
}

int main(void)
{
	test_hmac_vectors();
	test_default_session();
	test_extended_needs_security();
	test_unlock_clear_reset();
	test_security_attempts();
	test_seed_single_use();
	test_s3_timeout();
	test_multi_did();
	test_malformed();
	return TEST_REPORT();
}
