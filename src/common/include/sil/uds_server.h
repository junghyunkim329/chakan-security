/* UDS (ISO 14229-1 subset) ECU server logic, project.md §9.2~9.4.
 * Pure state machine: no sockets, time and randomness are injected so every rule is unit-testable.
 * Services: 0x10 0x11 0x14 0x19 0x22 0x27 0x3E. Programming session and download are out of scope (§9.2).
 */
#ifndef SIL_UDS_SERVER_H
#define SIL_UDS_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sil_diag_gen.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
	SIL_UDS_SESSION_DEFAULT = 0x01,
	SIL_UDS_SESSION_EXTENDED = 0x03,
};

/* NRC (ISO 14229-1 Table A.1) */
enum {
	SIL_NRC_SERVICE_NOT_SUPPORTED = 0x11,
	SIL_NRC_SUBFUNCTION_NOT_SUPPORTED = 0x12,
	SIL_NRC_INCORRECT_LENGTH = 0x13,
	SIL_NRC_RESPONSE_TOO_LONG = 0x14,
	SIL_NRC_REQUEST_SEQUENCE_ERROR = 0x24,
	SIL_NRC_REQUEST_OUT_OF_RANGE = 0x31,
	SIL_NRC_SECURITY_ACCESS_DENIED = 0x33,
	SIL_NRC_INVALID_KEY = 0x35,
	SIL_NRC_EXCEEDED_ATTEMPTS = 0x36,
	SIL_NRC_DELAY_NOT_EXPIRED = 0x37,
	SIL_NRC_SERVICE_NOT_IN_SESSION = 0x7F,
};

enum { SIL_UDS_SEC_LOCKED = 0, SIL_UDS_SEC_UNLOCKED = 1, SIL_UDS_SEC_DELAY = 2, SIL_UDS_SEC_LOCKOUT = 3 };

/* Writes the value of a process DID (not F180~F182) into out, returns its length or -1 if not available. */
typedef int (*sil_uds_read_did_fn)(void *ctx, uint16_t did, uint8_t *out, size_t cap);
/* Fills buf with len random bytes, returns 0 on success. */
typedef int (*sil_uds_random_fn)(uint8_t *buf, size_t len);

typedef struct {
	const sil_diag_ecu_t *ecu;
	uint8_t key[SIL_UDS_KEY_BYTES];
	sil_uds_read_did_fn read_did;
	void *ctx;
	sil_uds_random_fn random;

	uint8_t session;
	bool unlocked;
	bool seed_valid;
	uint8_t seed[SIL_UDS_SEED_BYTES];
	unsigned failures;          /* failed sendKey in this execution (§9.4) */
	bool lockout;               /* until the ECU process restarts */
	int64_t delay_until_ns;
	int64_t last_request_ns;    /* S3 timer */
	uint8_t dtc_status[SIL_DIAG_MAX_DTCS]; /* bit0 testFailed, bit3 confirmedDTC */

	unsigned long resets;       /* ECUReset count, for the F181 test and logs */
} sil_uds_server_t;

void sil_uds_init(sil_uds_server_t *s, const sil_diag_ecu_t *ecu, const uint8_t key[SIL_UDS_KEY_BYTES],
		  sil_uds_read_did_fn read_did, void *ctx, sil_uds_random_fn random);

/* Handles one request. Returns the response length (0 = no response, e.g. suppressPosRspMsgIndicationBit). */
size_t sil_uds_handle(sil_uds_server_t *s, const uint8_t *req, size_t len, int64_t now_ns, uint8_t *resp, size_t cap);

/* S3 session timeout: call periodically. */
void sil_uds_tick(sil_uds_server_t *s, int64_t now_ns);

/* Fault injection: mark DTC index as currently failed and confirmed. */
void sil_uds_set_dtc_failed(sil_uds_server_t *s, unsigned index);

uint8_t sil_uds_security_state(const sil_uds_server_t *s, int64_t now_ns);

/* Expected sendKey for a seed (the tester computes the same with the shared key). out: 32 bytes. */
int sil_uds_compute_key(const uint8_t key[SIL_UDS_KEY_BYTES], uint16_t logical_address,
			const uint8_t seed[SIL_UDS_SEED_BYTES], uint8_t out[32]);

/* Loads the 32-byte local secret. Refuses files that are not regular, not owned by the caller or accessible by others. */
int sil_uds_load_key(const char *path, uint8_t key[SIL_UDS_KEY_BYTES], char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif
