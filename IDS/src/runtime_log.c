#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

struct rt_log {
    FILE *file;
    char *run,*policy,*input,*member,*line;
    size_t line_capacity,used;
    char buffer[BUFSIZ];
    uint64_t sequence,written,flushes,bytes,oversized,failures,pending;
    uint64_t first,last,min_latency,max_latency,start;
    long double pending_sum,total_latency;
};
static const char *format="{\"schema_version\":1,\"event_id\":\"%s:%" PRIu64 "\",\"experiment_id\":\"%s\","
    "\"recorded_at\":\"%s\",\"processing_monotonic_ns\":%" PRIu64 ",\"received_monotonic_ns\":%" PRIu64 ","
    "\"timestamp_domain\":\"dataset_relative\",\"policy_hash\":\"%s\",\"severity\":\"warning\",\"action\":\"log_only\","
    "\"source\":{\"input\":%s,\"member\":%s,\"line\":%" PRIu64 "},\"dlc\":%u,\"data\":\"%s\","
    "\"rule_id\":\"%s\",\"bus\":\"%s\",\"can_id\":\"0x%03X\",\"source_timestamp_us\":%" PRId64 ","
    "\"observed\":%s,\"threshold\":%s,\"window_us\":%s}\n";
static char *escape(const char *s) {
    json_object *o=s ? json_object_new_string(s):NULL;
    char *v=strdup(json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN|JSON_C_TO_STRING_NOSLASHESCAPE));
    json_object_put(o); return v;
}
static void committed(rt_log *l,uint64_t now) {
    uint64_t low=now-l->last,high=now-l->first;
    if (!l->written || low<l->min_latency) l->min_latency=low;
    if (high>l->max_latency) l->max_latency=high;
    l->total_latency+=(long double)l->pending*(long double)(now-l->start)-l->pending_sum;
    l->written+=l->pending; l->pending=0; l->pending_sum=0;
}
int rt_log_flush(rt_log *l) {
    if (!l || !l->used) return 0;
    if (fwrite(l->buffer,1,l->used,l->file)!=l->used || fflush(l->file)!=0) { ++l->failures; return -1; }
    committed(l,rt_clock_ns()); ++l->flushes; l->bytes+=l->used; l->used=0; return 0;
}
rt_log *rt_log_open(const char *path,const char *run,const char *policy,const char *input,const char *member) {
    rt_log *l=calloc(1,sizeof(*l)); if (!l) return NULL;
    l->start=rt_clock_ns(); l->run=strdup(run); l->policy=strdup(policy);
    l->input=escape(input); l->member=escape(member);
    if (!l->run || !l->policy || !l->input || !l->member) goto bad;
    /* Size the reusable formatter from the maximum representable fields. */
    int cap=snprintf(NULL,0,format,run,UINT64_MAX,run,"9999-12-31T23:59:59.999999Z",UINT64_MAX,UINT64_MAX,
        policy,l->input,l->member,UINT64_MAX,8u,"ffffffffffffffff","CAN_INTERVAL_SHORT","C-CAN",0x7ffu,
        INT64_MAX,"18446744073709551615","\"ID absent from this bus normal profile\"","1000000");
    if (cap<0) goto bad;
    l->line_capacity=(size_t)cap+1; l->line=malloc(l->line_capacity); if (!l->line) goto bad;
    l->file=fopen(path,"wx"); if (!l->file) goto bad;
    if (setvbuf(l->file,NULL,_IONBF,0)!=0) goto bad;
    return l;
bad: rt_log_close(l); return NULL;
}
int rt_log_event(rt_log *l,const canids_frame *f,uint64_t line,const canids_finding *hit,uint64_t received) {
    const char *rules[]={"CAN_ID_UNSEEN","CAN_DLC_UNSEEN","CAN_INTERVAL_SHORT","CAN_RATE_HIGH"};
    char observed[32],threshold[64],hex[17],wall[40];
    snprintf(observed,sizeof(observed),"%" PRIu64,hit->observed);
    snprintf(threshold,sizeof(threshold),"%" PRIu64,hit->threshold);
    if (hit->rule==CANIDS_ID_UNSEEN) {
        snprintf(observed,sizeof(observed),"\"0x%03X\"",f->can_id);
        strcpy(threshold,"\"ID absent from this bus normal profile\"");
    } else if (hit->rule==CANIDS_DLC_UNSEEN) {
        size_t n=0; threshold[n++]='[';
        for (unsigned d=0;d<=8;++d) if (hit->threshold & (1u<<d)) {
            if (n>1) threshold[n++]=',';
            threshold[n++]=(char)('0'+d);
        }
        threshold[n++]=']'; threshold[n]=0;
    }
    for (unsigned i=0;i<f->dlc;++i) snprintf(hex+i*2,3,"%02x",f->data[i]);
    hex[f->dlc*2]=0; rt_wall_time(wall);
    int n=snprintf(l->line,l->line_capacity,format,l->run,l->sequence+1,l->run,wall,rt_clock_ns(),received,
        l->policy,l->input,l->member,line,(unsigned)f->dlc,hex,rules[hit->rule-1],rt_bus(f->bus),(unsigned)f->can_id,
        f->timestamp_us,observed,threshold,hit->rule==CANIDS_RATE_HIGH ? "1000000":"null");
    if (n<0 || (size_t)n>=l->line_capacity) return -1;
    if ((size_t)n>sizeof(l->buffer)-l->used && rt_log_flush(l)<0) return -1;
    if (!l->pending) l->first=received;
    l->last=received; ++l->pending; l->pending_sum+=(long double)(received-l->start);
    if ((size_t)n>sizeof(l->buffer)) {
        ++l->oversized;
        if (fwrite(l->line,1,(size_t)n,l->file)!=(size_t)n || fflush(l->file)!=0) { ++l->failures; return -1; }
        committed(l,rt_clock_ns()); ++l->flushes; l->bytes+=(uint64_t)n;
    } else { memcpy(l->buffer+l->used,l->line,(size_t)n); l->used+=(size_t)n; }
    ++l->sequence; return 0;
}
json_object *rt_log_stats(rt_log *l) {
    json_object *o=json_object_new_object();
    rt_jint(o,"buffer_bytes",BUFSIZ); rt_jint(o,"events_written",l->written);
    rt_jint(o,"events_pending",l->pending); rt_jint(o,"events_dropped",0);
    rt_jint(o,"flushes",l->flushes); rt_jint(o,"bytes_written",l->bytes);
    rt_jint(o,"oversized_records",l->oversized); rt_jint(o,"write_failures",l->failures);
    rt_jint(o,"latency_samples",l->written); rt_jint(o,"read_to_write_min_ns",l->min_latency);
    rt_jint(o,"read_to_write_max_ns",l->max_latency);
    json_object_object_add(o,"read_to_write_mean_ns",l->written ? json_object_new_double((double)(l->total_latency/l->written)):NULL);
    rt_jstr(o,"latency_boundary","process record-read start -> completed buffered kernel write; not fsync or live CAN receive");
    return o;
}
void rt_log_close(rt_log *l) {
    if (!l) return;
    if (l->file) fclose(l->file);
    free(l->run); free(l->policy); free(l->input); free(l->member); free(l->line); free(l);
}
