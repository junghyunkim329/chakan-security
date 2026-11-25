#include "canids.h"
#include <stdbool.h>
#include <stdlib.h>

#define CANIDS_ID_COUNT 2048u
#define CANIDS_ENTRY_COUNT (3u * CANIDS_ID_COUNT)

typedef struct {
    int64_t *times;
    size_t head, count, capacity;
} time_ring;

typedef struct {
    canids_profile policy;
    bool present, have_previous;
    int64_t previous;
    time_ring window;
} entry;

struct canids_detector {
    entry entries[CANIDS_ENTRY_COUNT];
    bool have_time;
    int64_t last_time;
    uint64_t window_us;
};

uint32_t canids_abi_version(void) { return 1; }
size_t canids_frame_size(void) { return sizeof(canids_frame); }
size_t canids_profile_size(void) { return sizeof(canids_profile); }
size_t canids_finding_size(void) { return sizeof(canids_finding); }

void canids_destroy(canids_detector *d) {
    if (d == NULL) return;
    for (size_t i=0; i<CANIDS_ENTRY_COUNT; ++i) free(d->entries[i].window.times);
    free(d);
}

canids_detector *canids_create(const canids_profile *profiles, size_t count, uint64_t window_us, int *error) {
    if (error != NULL) *error=CANIDS_INVALID_ARGUMENT;
    if (profiles==NULL || count==0 || count>CANIDS_ENTRY_COUNT || window_us!=1000000) return NULL;
    canids_detector *d=calloc(1,sizeof(*d));
    if (d==NULL) { if (error!=NULL) *error=CANIDS_NO_MEMORY; return NULL; }
    d->window_us=window_us;
    for (size_t i=0; i<count; ++i) {
        const canids_profile *p=&profiles[i];
        if (p->bus>=3 || p->can_id>=CANIDS_ID_COUNT || p->allowed_dlcs==0 ||
            (p->allowed_dlcs & UINT16_C(0xFE00))!=0 || p->min_interval_us < -1 || p->max_count_1s==0) {
            canids_destroy(d); return NULL;
        }
        entry *e=&d->entries[(size_t)p->bus*CANIDS_ID_COUNT+p->can_id];
        if (e->present) { canids_destroy(d); return NULL; }
        e->present=true; e->policy=*p;
    }
    if (error!=NULL) *error=CANIDS_OK;
    return d;
}

static int reserve_one(time_ring *q) {
    if (q->count < q->capacity) return CANIDS_OK;
    if (q->capacity > SIZE_MAX/2) return CANIDS_NO_MEMORY;
    size_t next=q->capacity ? q->capacity*2 : 8;
    if (next > SIZE_MAX/sizeof(*q->times)) return CANIDS_NO_MEMORY;
    int64_t *times=malloc(next*sizeof(*times));
    if (times==NULL) return CANIDS_NO_MEMORY;
    for (size_t i=0; i<q->count; ++i) times[i]=q->times[(q->head+i)%q->capacity];
    free(q->times);
    q->times=times; q->capacity=next; q->head=0;
    return CANIDS_OK;
}

static void add(canids_finding *out,size_t *n,uint32_t rule,uint64_t observed,uint64_t threshold) {
    out[*n]=(canids_finding){.rule=rule,.observed=observed,.threshold=threshold};
    ++*n;
}

int canids_observe(canids_detector *d,const canids_frame *f,canids_finding *out,size_t capacity,size_t *n) {
    if (n!=NULL) *n=0;
    if (d==NULL || f==NULL || out==NULL || n==NULL || capacity<CANIDS_MAX_FINDINGS ||
        f->bus>=3 || f->can_id>=CANIDS_ID_COUNT || f->dlc>8 || f->timestamp_us<0) return CANIDS_INVALID_ARGUMENT;
    if (d->have_time && f->timestamp_us<d->last_time) return CANIDS_TIME_REGRESSION;
    entry *e=&d->entries[(size_t)f->bus*CANIDS_ID_COUNT+f->can_id];
    if (!e->present) {
        d->have_time=true; d->last_time=f->timestamp_us;
        add(out,n,CANIDS_ID_UNSEEN,f->can_id,0);
        return CANIDS_OK;
    }
    time_ring *q=&e->window;
    const int64_t cutoff=f->timestamp_us-(int64_t)d->window_us;
    while (q->count && q->times[q->head]<=cutoff) {
        q->head=(q->head+1)%q->capacity; --q->count;
    }
    int status=reserve_one(q);
    if (status!=CANIDS_OK) return status;
    d->have_time=true; d->last_time=f->timestamp_us;
    if ((e->policy.allowed_dlcs & (uint16_t)(1u<<f->dlc))==0)
        add(out,n,CANIDS_DLC_UNSEEN,f->dlc,e->policy.allowed_dlcs);
    if (e->have_previous && e->policy.min_interval_us>=0) {
        int64_t delta=f->timestamp_us-e->previous;
        if (delta<e->policy.min_interval_us)
            add(out,n,CANIDS_INTERVAL_SHORT,(uint64_t)delta,(uint64_t)e->policy.min_interval_us);
    }
    e->previous=f->timestamp_us; e->have_previous=true;
    q->times[(q->head+q->count)%q->capacity]=f->timestamp_us; ++q->count;
    if (q->count>e->policy.max_count_1s)
        add(out,n,CANIDS_RATE_HIGH,(uint64_t)q->count,e->policy.max_count_1s);
    return CANIDS_OK;
}
