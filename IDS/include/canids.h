#ifndef CANIDS_H
#define CANIDS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { CANIDS_OK=0, CANIDS_INVALID_ARGUMENT=-1, CANIDS_TIME_REGRESSION=-2, CANIDS_NO_MEMORY=-3 };
enum { CANIDS_ID_UNSEEN=1, CANIDS_DLC_UNSEEN=2, CANIDS_INTERVAL_SHORT=3, CANIDS_RATE_HIGH=4 };
enum { CANIDS_C_CAN=0, CANIDS_P_CAN=1, CANIDS_B_CAN=2, CANIDS_MAX_FINDINGS=4 };

typedef struct {
    uint8_t bus;
    uint16_t can_id;
    uint16_t allowed_dlcs; /* Bit n allows DLC n, n=0..8. */
    int64_t min_interval_us; /* -1 means no supported minimum. */
    uint64_t max_count_1s;
} canids_profile;

typedef struct {
    uint8_t bus;
    uint8_t dlc;
    uint16_t can_id;
    int64_t timestamp_us;
    uint8_t data[8]; /* Retained for adapters; v1 rules do not interpret signals. */
} canids_frame;

typedef struct {
    uint32_t rule;
    uint64_t observed;
    uint64_t threshold; /* DLC rule uses the allowed-DLC bitmask. */
} canids_finding;

typedef struct canids_detector canids_detector;

uint32_t canids_abi_version(void);
size_t canids_frame_size(void);
size_t canids_profile_size(void);
size_t canids_finding_size(void);

/* Profiles are validated and copied. Caller retains ownership of its array.
 * v1 uses the fixed measured-envelope window of 1,000,000 us.
 * A detector is single-stream/single-thread state; independent streams need
 * independent instances. No function sends frames or performs prevention.
 */
canids_detector *canids_create(const canids_profile *profiles, size_t count, uint64_t window_us, int *error);
void canids_destroy(canids_detector *detector);

/* Frame times must be globally nondecreasing on the shared dataset clock.
 * The output array must have capacity >= CANIDS_MAX_FINDINGS. Output count is
 * zero on error. Known-ID windows store active timestamps on a growable heap
 * ring. Allocation failure is an error, never silent frame/event loss.
 */
int canids_observe(canids_detector *detector, const canids_frame *frame,
                   canids_finding *findings, size_t capacity, size_t *count);

#ifdef __cplusplus
}
#endif
#endif
