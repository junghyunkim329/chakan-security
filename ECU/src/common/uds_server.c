#define _GNU_SOURCE
#include "sil/uds_server.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>

#define NS_PER_MS 1000000LL
#define SPRMIB 0x80u /* suppressPosRspMsgIndicationBit */
#define DTC_STATUS_AVAILABILITY 0x09u /* testFailed | confirmedDTC */

void sil_uds_init(sil_uds_server_t *s, const sil_diag_ecu_t *ecu, const uint8_t key[SIL_UDS_KEY_BYTES],
		  sil_uds_read_did_fn read_did, void *ctx, sil_uds_random_fn random)
{
	memset(s, 0, sizeof(*s));
	s->ecu = ecu;
	memcpy(s->key, key, SIL_UDS_KEY_BYTES);
	s->read_did = read_did;
	s->ctx = ctx;
	s->random = random;
	s->session = SIL_UDS_SESSION_DEFAULT;
}

static void enter_default_session(sil_uds_server_t *s)
{
	s->session = SIL_UDS_SESSION_DEFAULT;
	s->unlocked = false;
	s->seed_valid = false;
}

uint8_t sil_uds_security_state(const sil_uds_server_t *s, int64_t now_ns)
{
	if (s->lockout)
		return SIL_UDS_SEC_LOCKOUT;
	if (now_ns < s->delay_until_ns)
		return SIL_UDS_SEC_DELAY;
	return s->unlocked ? SIL_UDS_SEC_UNLOCKED : SIL_UDS_SEC_LOCKED;
}

void sil_uds_tick(sil_uds_server_t *s, int64_t now_ns)
{
	if (s->session != SIL_UDS_SESSION_DEFAULT && now_ns - s->last_request_ns >= (int64_t)SIL_UDS_S3_SERVER_MS * NS_PER_MS)
		enter_default_session(s);
}

void sil_uds_set_dtc_failed(sil_uds_server_t *s, unsigned index)
{
	if (index < s->ecu->dtc_count)
		s->dtc_status[index] = DTC_STATUS_AVAILABILITY;
}

int sil_uds_compute_key(const uint8_t key[SIL_UDS_KEY_BYTES], uint16_t logical_address,
			const uint8_t seed[SIL_UDS_SEED_BYTES], uint8_t out[32])
{
	uint8_t msg[2 + SIL_UDS_SEED_BYTES];
	size_t outlen = 0;

	msg[0] = (uint8_t)(logical_address >> 8);
	msg[1] = (uint8_t)(logical_address & 0xFFu);
	memcpy(msg + 2, seed, SIL_UDS_SEED_BYTES);
	if (EVP_Q_mac(NULL, "HMAC", NULL, "SHA256", NULL, key, SIL_UDS_KEY_BYTES, msg, sizeof(msg), out, 32, &outlen) == NULL ||
	    outlen != 32)
		return -1;
	return 0;
}

int sil_uds_load_key(const char *path, uint8_t key[SIL_UDS_KEY_BYTES], char *err, size_t errlen)
{
	struct stat st;
	ssize_t n;
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

	if (fd < 0) {
		snprintf(err, errlen, "open %s: %s", path, strerror(errno));
		return -1;
	}
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077) != 0 ||
	    st.st_size != SIL_UDS_KEY_BYTES) {
		snprintf(err, errlen, "%s: must be a %u-byte regular file owned by uid %u with mode 0600", path,
			 SIL_UDS_KEY_BYTES, (unsigned)geteuid());
		close(fd);
		return -1;
	}
	n = read(fd, key, SIL_UDS_KEY_BYTES);
	close(fd);
	if (n != (ssize_t)SIL_UDS_KEY_BYTES) {
		snprintf(err, errlen, "%s: short read", path);
		return -1;
	}
	return 0;
}

static size_t nrc(uint8_t sid, uint8_t code, uint8_t *resp)
{
	resp[0] = 0x7F;
	resp[1] = sid;
	resp[2] = code;
	return 3;
}

static bool did_supported(const sil_uds_server_t *s, uint16_t did)
{
	for (uint8_t i = 0; i < s->ecu->did_count; i++)
		if (s->ecu->dids[i] == did)
			return true;
	return false;
}

/* returns data length written at out, or -1 */
static int read_did(const sil_uds_server_t *s, uint16_t did, int64_t now_ns, uint8_t *out, size_t cap)
{
	size_t n;
	uint8_t bitmap = 0;

	switch (did) {
	case SIL_DID_SOFTWARE_VERSION:
		n = strlen(s->ecu->software_version);
		if (n > cap)
			return -1;
		memcpy(out, s->ecu->software_version, n);
		return (int)n;
	case SIL_DID_ECU_STATE:
		if (cap < 2)
			return -1;
		out[0] = s->session;
		out[1] = sil_uds_security_state(s, now_ns);
		return 2;
	case SIL_DID_ACTIVE_FAULT_BITMAP:
		if (cap < 1)
			return -1;
		for (uint8_t i = 0; i < s->ecu->dtc_count; i++)
			if (s->dtc_status[i] & 0x01u)
				bitmap |= (uint8_t)(1u << i);
		out[0] = bitmap;
		return 1;
	default:
		return s->read_did ? s->read_did(s->ctx, did, out, cap) : -1;
	}
}

static size_t svc_session(sil_uds_server_t *s, const uint8_t *req, size_t len, uint8_t *resp)
{
	uint8_t sub;

	if (len != 2)
		return nrc(0x10, SIL_NRC_INCORRECT_LENGTH, resp);
	sub = req[1] & (uint8_t)~SPRMIB;
	if (sub != SIL_UDS_SESSION_DEFAULT && sub != SIL_UDS_SESSION_EXTENDED)
		return nrc(0x10, SIL_NRC_SUBFUNCTION_NOT_SUPPORTED, resp);
	/* any session transition re-locks security (ISO 14229-1 §9.2) */
	enter_default_session(s);
	s->session = sub;
	if (req[1] & SPRMIB)
		return 0;
	resp[0] = 0x50;
	resp[1] = sub;
	resp[2] = (uint8_t)(SIL_UDS_P2_SERVER_MS >> 8);
	resp[3] = (uint8_t)(SIL_UDS_P2_SERVER_MS & 0xFFu);
	resp[4] = (uint8_t)((SIL_UDS_P2_STAR_SERVER_MS / 10u) >> 8); /* P2* in 10 ms units */
	resp[5] = (uint8_t)((SIL_UDS_P2_STAR_SERVER_MS / 10u) & 0xFFu);
	return 6;
}

static size_t svc_reset(sil_uds_server_t *s, const uint8_t *req, size_t len, uint8_t *resp)
{
	if (len != 2)
		return nrc(0x11, SIL_NRC_INCORRECT_LENGTH, resp);
	if ((req[1] & (uint8_t)~SPRMIB) != 0x01) /* hardReset only */
		return nrc(0x11, SIL_NRC_SUBFUNCTION_NOT_SUPPORTED, resp);
	if (s->session == SIL_UDS_SESSION_DEFAULT)
		return nrc(0x11, SIL_NRC_SERVICE_NOT_IN_SESSION, resp);
	if (!s->unlocked)
		return nrc(0x11, SIL_NRC_SECURITY_ACCESS_DENIED, resp);
	/* §9.2: only the ECU process's internal state is reset. DTC memory is non-volatile and kept;
	 * the SecurityAccess failure count and lockout are kept until the process really restarts (§9.4). */
	enter_default_session(s);
	s->resets++;
	if (req[1] & SPRMIB)
		return 0;
	resp[0] = 0x51;
	resp[1] = 0x01;
	return 2;
}

static size_t svc_clear_dtc(sil_uds_server_t *s, const uint8_t *req, size_t len, uint8_t *resp)
{
	if (len != 4)
		return nrc(0x14, SIL_NRC_INCORRECT_LENGTH, resp);
	if (s->session == SIL_UDS_SESSION_DEFAULT)
		return nrc(0x14, SIL_NRC_SERVICE_NOT_IN_SESSION, resp);
	if (!s->unlocked)
		return nrc(0x14, SIL_NRC_SECURITY_ACCESS_DENIED, resp);
	if (req[1] != 0xFF || req[2] != 0xFF || req[3] != 0xFF) /* all groups only */
		return nrc(0x14, SIL_NRC_REQUEST_OUT_OF_RANGE, resp);
	memset(s->dtc_status, 0, sizeof(s->dtc_status));
	resp[0] = 0x54;
	return 1;
}

static size_t svc_read_dtc(sil_uds_server_t *s, const uint8_t *req, size_t len, uint8_t *resp, size_t cap)
{
	uint8_t sub, mask;
	size_t n;
	unsigned count = 0;

	if (len < 2)
		return nrc(0x19, SIL_NRC_INCORRECT_LENGTH, resp);
	sub = req[1] & (uint8_t)~SPRMIB;
	if (sub != 0x01 && sub != 0x02)
		return nrc(0x19, SIL_NRC_SUBFUNCTION_NOT_SUPPORTED, resp);
	if (len != 3)
		return nrc(0x19, SIL_NRC_INCORRECT_LENGTH, resp);
	mask = req[2];
	for (uint8_t i = 0; i < s->ecu->dtc_count; i++)
		if (s->dtc_status[i] & mask)
			count++;
	resp[0] = 0x59;
	resp[1] = sub;
	resp[2] = DTC_STATUS_AVAILABILITY;
	if (sub == 0x01) { /* reportNumberOfDTCByStatusMask */
		resp[3] = 0x01;  /* DTCFormatIdentifier: ISO 14229-1 DTC format */
		resp[4] = (uint8_t)(count >> 8);
		resp[5] = (uint8_t)count;
		return 6;
	}
	n = 3; /* reportDTCByStatusMask */
	for (uint8_t i = 0; i < s->ecu->dtc_count; i++) {
		if (!(s->dtc_status[i] & mask))
			continue;
		if (n + 4 > cap)
			return nrc(0x19, SIL_NRC_RESPONSE_TOO_LONG, resp);
		resp[n++] = (uint8_t)(s->ecu->dtcs[i] >> 16);
		resp[n++] = (uint8_t)(s->ecu->dtcs[i] >> 8);
		resp[n++] = (uint8_t)s->ecu->dtcs[i];
		resp[n++] = s->dtc_status[i];
	}
	return n;
}

static size_t svc_read_did(sil_uds_server_t *s, const uint8_t *req, size_t len, int64_t now_ns, uint8_t *resp, size_t cap)
{
	size_t n = 1;

	if (len < 3 || (len - 1) % 2 != 0 || (len - 1) / 2 > 8)
		return nrc(0x22, SIL_NRC_INCORRECT_LENGTH, resp);
	resp[0] = 0x62;
	for (size_t i = 1; i < len; i += 2) {
		uint16_t did = (uint16_t)((req[i] << 8) | req[i + 1]);
		int k;

		if (!did_supported(s, did))
			return nrc(0x22, SIL_NRC_REQUEST_OUT_OF_RANGE, resp);
		if (n + 2 > cap)
			return nrc(0x22, SIL_NRC_RESPONSE_TOO_LONG, resp);
		resp[n++] = req[i];
		resp[n++] = req[i + 1];
		k = read_did(s, did, now_ns, resp + n, cap - n);
		if (k < 0)
			return nrc(0x22, SIL_NRC_RESPONSE_TOO_LONG, resp);
		n += (size_t)k;
	}
	return n;
}

static size_t svc_security(sil_uds_server_t *s, const uint8_t *req, size_t len, int64_t now_ns, uint8_t *resp)
{
	uint8_t sub;

	if (len < 2)
		return nrc(0x27, SIL_NRC_INCORRECT_LENGTH, resp);
	sub = req[1];
	if (sub != SIL_UDS_SECURITY_LEVEL && sub != SIL_UDS_SECURITY_LEVEL + 1)
		return nrc(0x27, SIL_NRC_SUBFUNCTION_NOT_SUPPORTED, resp);
	if (s->session == SIL_UDS_SESSION_DEFAULT)
		return nrc(0x27, SIL_NRC_SERVICE_NOT_IN_SESSION, resp);
	if (s->lockout)
		return nrc(0x27, SIL_NRC_EXCEEDED_ATTEMPTS, resp);

	if (sub == SIL_UDS_SECURITY_LEVEL) { /* requestSeed */
		if (len != 2)
			return nrc(0x27, SIL_NRC_INCORRECT_LENGTH, resp);
		if (now_ns < s->delay_until_ns)
			return nrc(0x27, SIL_NRC_DELAY_NOT_EXPIRED, resp);
		resp[0] = 0x67;
		resp[1] = sub;
		if (s->unlocked) { /* already unlocked: zero seed (ISO 14229-1) */
			memset(resp + 2, 0, SIL_UDS_SEED_BYTES);
			return 2 + SIL_UDS_SEED_BYTES;
		}
		if (s->random(s->seed, SIL_UDS_SEED_BYTES) != 0)
			return nrc(0x27, 0x22 /* conditionsNotCorrect */, resp);
		s->seed_valid = true;
		memcpy(resp + 2, s->seed, SIL_UDS_SEED_BYTES);
		return 2 + SIL_UDS_SEED_BYTES;
	}

	/* sendKey */
	if (len != 2 + 32)
		return nrc(0x27, SIL_NRC_INCORRECT_LENGTH, resp);
	if (!s->seed_valid)
		return nrc(0x27, SIL_NRC_REQUEST_SEQUENCE_ERROR, resp);
	s->seed_valid = false; /* a seed is good for exactly one attempt */
	{
		uint8_t expect[32];

		if (sil_uds_compute_key(s->key, s->ecu->logical_address, s->seed, expect) == 0 &&
		    CRYPTO_memcmp(expect, req + 2, 32) == 0) {
			s->unlocked = true;
			resp[0] = 0x67;
			resp[1] = sub;
			return 2;
		}
	}
	s->failures++;
	if (s->failures >= SIL_UDS_LOCKOUT_AFTER_FAILURES) {
		s->lockout = true;
		return nrc(0x27, SIL_NRC_EXCEEDED_ATTEMPTS, resp);
	}
	if (s->failures == SIL_UDS_DELAY_AFTER_FAILURES) {
		s->delay_until_ns = now_ns + (int64_t)SIL_UDS_SECURITY_DELAY_MS * NS_PER_MS;
		return nrc(0x27, SIL_NRC_EXCEEDED_ATTEMPTS, resp);
	}
	return nrc(0x27, SIL_NRC_INVALID_KEY, resp);
}

static size_t svc_tester_present(const uint8_t *req, size_t len, uint8_t *resp)
{
	if (len != 2)
		return nrc(0x3E, SIL_NRC_INCORRECT_LENGTH, resp);
	if ((req[1] & (uint8_t)~SPRMIB) != 0x00)
		return nrc(0x3E, SIL_NRC_SUBFUNCTION_NOT_SUPPORTED, resp);
	if (req[1] & SPRMIB)
		return 0;
	resp[0] = 0x7E;
	resp[1] = 0x00;
	return 2;
}

size_t sil_uds_handle(sil_uds_server_t *s, const uint8_t *req, size_t len, int64_t now_ns, uint8_t *resp, size_t cap)
{
	if (len == 0 || cap < 8)
		return 0;
	/* the S3 timer is checked before the request so a late request does not keep the old session alive */
	sil_uds_tick(s, now_ns);
	s->last_request_ns = now_ns;

	switch (req[0]) {
	case 0x10: return svc_session(s, req, len, resp);
	case 0x11: return svc_reset(s, req, len, resp);
	case 0x14: return svc_clear_dtc(s, req, len, resp);
	case 0x19: return svc_read_dtc(s, req, len, resp, cap);
	case 0x22: return svc_read_did(s, req, len, now_ns, resp, cap);
	case 0x27: return svc_security(s, req, len, now_ns, resp);
	case 0x3E: return svc_tester_present(req, len, resp);
	default: return nrc(req[0], SIL_NRC_SERVICE_NOT_SUPPORTED, resp);
	}
}
