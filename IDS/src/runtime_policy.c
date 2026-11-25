#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include "runtime.h"
#include <openssl/evp.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

uint64_t rt_clock_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)!=0) abort();
    return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
}
void rt_wall_time(char out[40]) {
    struct timespec t; struct tm tm;
    clock_gettime(CLOCK_REALTIME,&t); gmtime_r(&t.tv_sec,&tm);
    strftime(out,40,"%Y-%m-%dT%H:%M:%S",&tm);
    size_t n=strlen(out); snprintf(out+n,40-n,".%06ldZ",t.tv_nsec/1000);
}
const char *rt_bus(unsigned bus) {
    static const char *names[]={"C-CAN","P-CAN","B-CAN"};
    return bus<3 ? names[bus] : NULL;
}
static void hex_digest(const unsigned char *data,unsigned n,char out[65]) {
    for (unsigned i=0;i<n;++i) snprintf(out+2*i,3,"%02x",data[i]);
}
int rt_hash_file(const char *path,char out[65]) {
    FILE *f=fopen(path,"rb"); if (!f) return -1;
    EVP_MD_CTX *h=EVP_MD_CTX_new(); unsigned char buf[BUFSIZ],digest[EVP_MAX_MD_SIZE];
    unsigned n=0; int ok=h && EVP_DigestInit_ex(h,EVP_sha256(),NULL)==1;
    size_t got;
    while (ok && (got=fread(buf,1,sizeof(buf),f))>0) ok=EVP_DigestUpdate(h,buf,got)==1;
    ok=ok && !ferror(f) && EVP_DigestFinal_ex(h,digest,&n)==1;
    if (fclose(f)!=0) ok=0;
    EVP_MD_CTX_free(h); if (!ok) return -1;
    hex_digest(digest,n,out); return 0;
}
int rt_utf8(const unsigned char *s,size_t len,size_t *used,uint32_t *cp) {
    if (!len) return -1;
    unsigned n; uint32_t c=s[0],minimum;
    if (c<128) { *used=1; *cp=c; return 0; }
    if (c>=0xc2 && c<=0xdf) { n=2; c&=31; minimum=128; }
    else if (c>=0xe0 && c<=0xef) { n=3; c&=15; minimum=2048; }
    else if (c>=0xf0 && c<=0xf4) { n=4; c&=7; minimum=65536; }
    else return -1;
    if (len<n) return -1;
    for (unsigned i=1;i<n;++i) { if ((s[i]&0xc0)!=0x80) return -1; c=(c<<6)|(s[i]&63); }
    if (c<minimum || c>0x10ffff || (c>=0xd800 && c<=0xdfff)) return -1;
    *cp=c; *used=n; return 0;
}
json_object *rt_read_json(const char *path,char hash[65]) {
    FILE *f=fopen(path,"rb"); if (!f) return NULL;
    if (fseek(f,0,SEEK_END)!=0) { fclose(f); return NULL; }
    long size=ftell(f);
    if (size<=0 || (uint64_t)size>(uint64_t)(INT_MAX-1)/6 || fseek(f,0,SEEK_SET)!=0) { fclose(f); return NULL; }
    size_t n=(size_t)size;
    unsigned char *raw=malloc(n); char *ascii=malloc(n*6+1);
    if (!raw || !ascii) { free(raw); free(ascii); fclose(f); return NULL; }
    int ok=fread(raw,1,n,f)==n && fgetc(f)==EOF && !ferror(f);
    fclose(f); size_t out=0;
    /* Normalize valid UTF-8 to equivalent JSON ASCII escapes. This preserves
     * strict syntax with the linked json-c, which rejects literal Korean in
     * strict mode. The digest remains over the exact original file bytes. */
    for (size_t i=0;ok && i<n;) {
        size_t used; uint32_t cp;
        if (rt_utf8(raw+i,n-i,&used,&cp)<0 || !cp) { ok=0; break; }
        i+=used;
        if (cp<128) ascii[out++]=(char)cp;
        else if (cp<=0xffff) out+=(size_t)snprintf(ascii+out,n*6+1-out,"\\u%04x",(unsigned)cp);
        else { cp-=0x10000; out+=(size_t)snprintf(ascii+out,n*6+1-out,"\\u%04x\\u%04x",(unsigned)(0xd800+(cp>>10)),(unsigned)(0xdc00+(cp&1023))); }
    }
    ascii[out]=0; json_object *o=NULL; json_tokener *tok=json_tokener_new();
    unsigned char bytes[EVP_MAX_MD_SIZE]; unsigned length;
    if (ok && tok && EVP_Digest(raw,n,bytes,&length,EVP_sha256(),NULL)==1) {
        json_tokener_set_flags(tok,JSON_TOKENER_STRICT);
        o=json_tokener_parse_ex(tok,ascii,(int)out);
        if (json_tokener_get_error(tok)!=json_tokener_success) { json_object_put(o); o=NULL; }
        else {
            for (size_t i=json_tokener_get_parse_end(tok);i<out;++i)
                if (!strchr(" \t\r\n",ascii[i])) ok=0;
            if (!ok) { json_object_put(o); o=NULL; }
            else hex_digest(bytes,length,hash);
        }
    }
    json_tokener_free(tok); free(raw); free(ascii); return o;
}
int rt_write_json(const char *path,json_object *o) {
    size_t n=strlen(path)+5; char *tmp=malloc(n); if (!tmp) return -1;
    snprintf(tmp,n,"%s.tmp",path);
    int fd=open(tmp,O_WRONLY|O_CREAT|O_EXCL,0600); if (fd<0) { free(tmp); return -1; }
    const char *s=json_object_to_json_string_ext(o,JSON_C_TO_STRING_PRETTY|JSON_C_TO_STRING_NOSLASHESCAPE);
    size_t left=strlen(s); const char *at=s; int ok=1;
    while (left) { ssize_t got=write(fd,at,left); if (got<0 && errno==EINTR) continue;
        if (got<=0) { ok=0; break; } at+=got; left-=(size_t)got; }
    if (ok && write(fd,"\n",1)!=1) ok=0;
    if (close(fd)!=0) ok=0;
    if (ok && rename(tmp,path)!=0) ok=0;
    if (!ok) unlink(tmp);
    free(tmp); return ok ? 0 : -1;
}
static json_object *get(json_object *o,const char *k) {
    if (!o || !json_object_is_type(o,json_type_object)) return NULL;
    json_object *v=NULL; json_object_object_get_ex(o,k,&v); return v;
}
static int type(json_object *o,enum json_type t) { return json_object_is_type(o,t); }
static int text(json_object *o,const char *s) {
    return type(o,json_type_string) && strcmp(json_object_get_string(o),s)==0 &&
        (size_t)json_object_get_string_len(o)==strlen(s);
}
static int integer(json_object *o,int64_t low,int64_t high,int64_t *value) {
    if (!type(o,json_type_int)) return 0;
    const char *s=json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN);
    errno=0; char *end; int64_t n=strtoll(s,&end,10);
    if (errno || *end || n<low || n>high) return 0;
    if (value) *value=n;
    return 1;
}
static int digest(json_object *o) {
    if (!type(o,json_type_string) || json_object_get_string_len(o)!=64) return 0;
    const char *s=json_object_get_string(o);
    for (int i=0;i<64;++i) if (!s[i] || !strchr("0123456789abcdef",s[i])) return 0;
    return 1;
}
static int cmp_keys(const void *a,const void *b) { return strcmp(*(const char *const *)a,*(const char *const *)b); }
static int update(EVP_MD_CTX *h,const char *s) { return EVP_DigestUpdate(h,s,strlen(s))==1 ? 0 : -1; }
static int quoted(EVP_MD_CTX *h,const char *s,size_t len) {
    if (update(h,"\"")<0) return -1;
    for (size_t i=0;i<len;) {
        uint32_t c; size_t used; char esc[16];
        if (rt_utf8((const unsigned char *)s+i,len-i,&used,&c)<0) return -1;
        i+=used;
        const char *short_escape=NULL;
        switch (c) {
          case '"': short_escape="\\\""; break; case '\\': short_escape="\\\\"; break;
          case '\b': short_escape="\\b"; break; case '\f': short_escape="\\f"; break;
          case '\n': short_escape="\\n"; break; case '\r': short_escape="\\r"; break;
          case '\t': short_escape="\\t"; break;
        }
        if (short_escape) { if (update(h,short_escape)<0) return -1; }
        else if (c<32 || c>=127) {
            if (c<=0xffff) snprintf(esc,sizeof(esc),"\\u%04x",(unsigned)c);
            else { c-=0x10000; snprintf(esc,sizeof(esc),"\\u%04x\\u%04x",(unsigned)(0xd800+(c>>10)),(unsigned)(0xdc00+(c&1023))); }
            if (update(h,esc)<0) return -1;
        } else { char v=(char)c; if (EVP_DigestUpdate(h,&v,1)!=1) return -1; }
    }
    return update(h,"\"");
}
static int canonical(EVP_MD_CTX *h,json_object *o,int root) {
    if (type(o,json_type_double) && !isfinite(json_object_get_double(o))) return -1;
    if (type(o,json_type_object)) {
        size_t n=json_object_object_length(o),count=0; const char **keys=calloc(n ? n:1,sizeof(*keys));
        if (!keys) return -1;
        json_object_object_foreach(o,k,v) { (void)v; if (!root || strcmp(k,"policy_hash")) keys[count++]=k; }
        qsort(keys,count,sizeof(*keys),cmp_keys); int ok=update(h,"{");
        for (size_t i=0;ok==0 && i<count;++i) {
            if (i) ok=update(h,",");
            if (!ok) ok=quoted(h,keys[i],strlen(keys[i]));
            if (!ok) ok=update(h,":");
            if (!ok) ok=canonical(h,get(o,keys[i]),0);
        }
        free(keys); return ok ? -1 : update(h,"}");
    }
    if (type(o,json_type_array)) {
        if (update(h,"[")<0) return -1;
        for (size_t i=0;i<json_object_array_length(o);++i) {
            if ((i && update(h,",")<0) || canonical(h,json_object_array_get_idx(o,i),0)<0) return -1;
        }
        return update(h,"]");
    }
    if (type(o,json_type_string)) return quoted(h,json_object_get_string(o),(size_t)json_object_get_string_len(o));
    return update(h,json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN));
}
void rt_policy_free(rt_policy *p) { json_object_put(p->document); free(p->profiles); memset(p,0,sizeof(*p)); }
int rt_policy_load(const char *path,rt_policy *out) {
    memset(out,0,sizeof(*out)); json_object *o=rt_read_json(path,out->file_hash); out->document=o;
    if (!type(o,json_type_object) || json_object_object_length(o)!=7 ||
        !integer(get(o,"schema_version"),1,1,NULL) || !text(get(o,"kind"),"normal_observed_envelope") ||
        !integer(get(o,"window_us"),1000000,1000000,NULL) || !digest(get(o,"policy_hash"))) goto bad;
    const char *rules[]={"CAN_ID_UNSEEN","CAN_DLC_UNSEEN","CAN_INTERVAL_SHORT","CAN_RATE_HIGH"};
    json_object *rs=get(o,"rule_ids"),*training=get(o,"training"),*profiles=get(o,"profiles");
    if (!type(rs,json_type_array) || json_object_array_length(rs)!=4) goto bad;
    for (size_t i=0;i<4;++i) if (!text(json_object_array_get_idx(rs,i),rules[i])) goto bad;
    int64_t seen,normal,rows;
    if (!type(training,json_type_object) || json_object_object_length(training)!=3 ||
        !integer(get(training,"seen_frames"),1,INT64_MAX,&seen) ||
        !integer(get(training,"normal_frames"),1,seen,&normal)) goto bad;
    json_object *source=get(training,"source"),*member=get(source,"member"),*input=get(source,"input");
    json_object *complete=get(source,"complete"),*labels=get(source,"has_labels");
    if (!type(source,json_type_object) || !type(input,json_type_string) || !json_object_get_string_len(input) ||
        !type(complete,json_type_boolean) || !json_object_get_boolean(complete) ||
        !type(labels,json_type_boolean) || !json_object_get_boolean(labels) ||
        !integer(get(source,"rows"),1,INT64_MAX,&rows) || rows!=seen ||
        !digest(get(source,"input_sha256")) || !digest(get(source,"member_sha256")) || !digest(get(source,"frames_sha256"))) goto bad;
    json_object *member_present;
    if (!json_object_object_get_ex(source,"member",&member_present) ||
        (!type(member,json_type_null) && (!type(member,json_type_string) || !json_object_get_string_len(member)))) goto bad;
    if (!type(profiles,json_type_array) || !(out->count=json_object_array_length(profiles)) || out->count>6144) goto bad;
    out->profiles=calloc(out->count,sizeof(*out->profiles)); if (!out->profiles) goto bad;
    uint64_t total=0; unsigned char exists[6144]={0};
    for (size_t i=0;i<out->count;++i) {
        json_object *e=json_object_array_get_idx(profiles,i),*dlcs=get(e,"dlcs"); canids_profile *p=&out->profiles[i];
        int64_t id,n,pairs,max_count,min=-1;
        if (!type(e,json_type_object) || json_object_object_length(e)!=7 ||
            !integer(get(e,"can_id"),0,2047,&id) || !integer(get(e,"normal_frames"),1,INT64_MAX,&n) ||
            !integer(get(e,"interval_pairs"),0,n-1,&pairs) || !integer(get(e,"max_count_1s"),1,n,&max_count)) goto bad;
        unsigned bus=0; while (bus<3 && !text(get(e,"bus"),rt_bus(bus))) ++bus;
        if (bus==3 || exists[bus*2048+(unsigned)id]++) goto bad;
        json_object *minimum;
        if (!json_object_object_get_ex(e,"min_interval_us",&minimum)) goto bad;
        if ((pairs==0 && !type(minimum,json_type_null)) ||
            (pairs>0 && !integer(get(e,"min_interval_us"),0,INT64_MAX,&min))) goto bad;
        if (!type(dlcs,json_type_array) || !json_object_array_length(dlcs) || json_object_array_length(dlcs)>9) goto bad;
        int64_t previous=-1;
        for (size_t j=0;j<json_object_array_length(dlcs);++j) {
            int64_t dlc; if (!integer(json_object_array_get_idx(dlcs,j),previous+1,8,&dlc)) goto bad;
            p->allowed_dlcs|=(uint16_t)(1u<<dlc); previous=dlc;
        }
        p->bus=(uint8_t)bus; p->can_id=(uint16_t)id; p->min_interval_us=min; p->max_count_1s=(uint64_t)max_count;
        if ((uint64_t)n>(uint64_t)normal-total) goto bad;
        total+=(uint64_t)n;
    }
    if (total!=(uint64_t)normal) goto bad;
    EVP_MD_CTX *h=EVP_MD_CTX_new(); unsigned char data[EVP_MAX_MD_SIZE]; unsigned size=0;
    int ok=h && EVP_DigestInit_ex(h,EVP_sha256(),NULL)==1 && canonical(h,o,1)==0 && EVP_DigestFinal_ex(h,data,&size)==1;
    EVP_MD_CTX_free(h); if (!ok) goto bad;
    hex_digest(data,size,out->hash);
    char current[65];
    if (strcmp(out->hash,json_object_get_string(get(o,"policy_hash")))) {
        fprintf(stderr,"policy hash mismatch: computed %s, declared %s\n",out->hash,json_object_get_string(get(o,"policy_hash")));
        goto bad;
    }
    if (rt_hash_file(path,current)<0 || strcmp(current,out->file_hash)) goto bad;
    return 0;
bad:
    fprintf(stderr,"invalid policy, provenance or policy hash: %s\n",path); rt_policy_free(out); return -1;
}
