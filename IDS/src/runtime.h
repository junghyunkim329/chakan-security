#ifndef CANIDS_RUNTIME_H
#define CANIDS_RUNTIME_H
#include "canids.h"
#include <signal.h>
#include <json-c/json.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    json_object *document;
    canids_profile *profiles;
    size_t count;
    char hash[65], file_hash[65];
} rt_policy;
typedef struct rt_input rt_input;
typedef struct rt_log rt_log;

extern volatile sig_atomic_t rt_interrupted;
uint64_t rt_clock_ns(void);
void rt_wall_time(char out[40]);
int rt_hash_file(const char *path, char out[65]);
int rt_utf8(const unsigned char *s, size_t len, size_t *used, uint32_t *cp);
int rt_write_json(const char *path, json_object *object);
json_object *rt_read_json(const char *path,char digest[65]);
int rt_policy_load(const char *path, rt_policy *out);
void rt_policy_free(rt_policy *p);
const char *rt_bus(unsigned bus);

rt_input *rt_input_open(const char *path, const char *member);
int rt_input_next(rt_input *s, canids_frame *frame, uint64_t *line);
json_object *rt_input_metadata(rt_input *s);
int rt_input_finish(rt_input *s);
void rt_input_close(rt_input *s);

rt_log *rt_log_open(const char *path, const char *run, const char *policy,
                    const char *input, const char *member);
int rt_log_event(rt_log *log, const canids_frame *frame, uint64_t line,
                 const canids_finding *finding, uint64_t received_ns);
int rt_log_flush(rt_log *log);
json_object *rt_log_stats(rt_log *log);
void rt_log_close(rt_log *log);

static inline void rt_jint(json_object *o,const char *k,uint64_t v) {
    json_object_object_add(o,k,json_object_new_uint64(v));
}
static inline void rt_jstr(json_object *o,const char *k,const char *v) {
    json_object_object_add(o,k,v ? json_object_new_string(v) : NULL);
}
#endif
