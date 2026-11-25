#define _XOPEN_SOURCE 700
#include "runtime.h"
#include "canids_build.h"
#include <archive.h>
#include <openssl/crypto.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

volatile sig_atomic_t rt_interrupted=0;
static void interrupt(int sig) { (void)sig; rt_interrupted=1; }
static char *join(const char *directory,const char *name) {
    size_t n=strlen(directory)+strlen(name)+2; char *s=malloc(n);
    if (s) snprintf(s,n,"%s/%s",directory,name);
    return s;
}
static int new_directory(const char *path) {
    char *copy=strdup(path); if (!copy || !*copy) { free(copy); return -1; }
    for (char *p=copy+1;*p;++p) if (*p=='/') {
        *p=0;
        if (mkdir(copy,0700)!=0 && errno!=EEXIST) { free(copy); return -1; }
        *p='/';
    }
    int result=mkdir(copy,0700); free(copy); return result;
}
static void add_bool(json_object *o,const char *key,int value) {
    json_object_object_add(o,key,json_object_new_boolean(value));
}
static void use(void) {
    puts("Usage: canids --input AutoHack.csv|data.zip [--member exact.csv] --policy policy.json --output-dir NEW_DIRECTORY\n"
         "Standalone passive C11 file IDS; no Python, CAN transmission or prevention.\n"
         "Python fit and evaluate are separate preparation/analysis tools.");
}
int main(int argc,char **argv) {
    const char *input=NULL,*member=NULL,*policy_path=NULL,*out=NULL;
    static const struct option options[]={{"input",required_argument,NULL,'i'}, {"member",required_argument,NULL,'m'},
        {"policy",required_argument,NULL,'p'},{"output-dir",required_argument,NULL,'o'}, {"help",no_argument,NULL,'h'}, {NULL,0,NULL,0}};
    int option;
    while ((option=getopt_long(argc,argv,"",options,NULL))!=-1) {
        if (option=='i') input=optarg; else if (option=='m') member=optarg;
        else if (option=='p') policy_path=optarg; else if (option=='o') out=optarg;
        else if (option=='h') { use(); return 0; } else { use(); return 2; }
    }
    if (!input || !policy_path || !out || optind!=argc) { use(); return 2; }
    struct sigaction action={0}; action.sa_handler=interrupt; sigemptyset(&action.sa_mask);
    sigaction(SIGTERM,&action,NULL); sigaction(SIGINT,&action,NULL);
    signal(SIGPIPE,SIG_IGN); signal(SIGXFSZ,SIG_IGN);
    uint64_t started=rt_clock_ns(); char wall[40],run[33],binary[65]; unsigned char random[16];
    if (getrandom(random,sizeof(random),0)!=(ssize_t)sizeof(random) || rt_hash_file("/proc/self/exe",binary)<0) {
        perror("executable identity"); return 2;
    }
    for (size_t i=0;i<sizeof(random);++i) snprintf(run+2*i,3,"%02x",random[i]);
    rt_policy policy;
    if (rt_policy_load(policy_path,&policy)<0) return 2;
    if (new_directory(out)!=0) { perror("output must be a new directory"); rt_policy_free(&policy); return 2; }
    char *manifest_path=join(out,"manifest.json"),*event_path=join(out,"ids-events.jsonl"),
         *runtime_path=join(out,"runtime.json"),*summary_path=join(out,"summary.md");
    if (!manifest_path || !event_path || !runtime_path || !summary_path) {
        rt_policy_free(&policy); free(manifest_path); free(event_path); free(runtime_path); free(summary_path); return 2;
    }
    json_object *manifest=json_object_new_object(),*backend=json_object_new_object(),*command=json_object_new_array();
    rt_jint(manifest,"schema_version",1); rt_jstr(manifest,"experiment_id",run);
    rt_jstr(manifest,"status","running"); rt_jstr(manifest,"mode","detect");
    rt_wall_time(wall); rt_jstr(manifest,"started_at",wall);
    rt_jstr(manifest,"policy_hash",policy.hash); rt_jstr(manifest,"policy_file_sha256",policy.file_hash);
    rt_jstr(manifest,"git_commit",CANIDS_BUILD_COMMIT);
    json_object_object_add(manifest,"implementation_sha256",json_tokener_parse(CANIDS_BUILD_SOURCES));
    for (int i=0;i<argc;++i) json_object_array_add(command,json_object_new_string(argv[i]));
    json_object_object_add(manifest,"command",command);
    add_bool(manifest,"can_transmission_executed",0); add_bool(manifest,"prevention_enabled",0);
    rt_jstr(backend,"backend","c11_standalone"); rt_jstr(backend,"executable_sha256",binary);
    rt_jstr(backend,"json_c",json_c_version()); rt_jstr(backend,"libarchive",archive_version_string());
    rt_jstr(backend,"openssl",OpenSSL_version(OPENSSL_VERSION));
    json_object_object_add(manifest,"backend",backend);
    struct utsname system; if (uname(&system)==0) rt_jstr(manifest,"kernel",system.release);
    int error; canids_detector *detector=canids_create(policy.profiles,policy.count,1000000,&error);
    rt_input *source=detector ? rt_input_open(input,member):NULL;
    json_object *metadata=rt_input_metadata(source);
    json_object_object_add(manifest,"source",metadata);
    json_object *path_node=NULL; if (metadata) json_object_object_get_ex(metadata,"input",&path_node);
    rt_log *log=source ? rt_log_open(event_path,run,policy.hash,json_object_get_string(path_node),member):NULL;
    int failed=!detector || !source || !log;
    uint64_t frames=0,events=0,alerted=0;
    const char *reason=failed ? "initialization failed":NULL;
    if (rt_write_json(manifest_path,manifest)<0) { failed=1; reason="initial manifest write failed"; }
    while (!failed && !rt_interrupted) {
        uint64_t received=rt_clock_ns(),line; canids_frame frame;
        int got=rt_input_next(source,&frame,&line);
        if (!got) break;
        if (got<0) { failed=1; reason="input validation/read failure"; break; }
        canids_finding hits[CANIDS_MAX_FINDINGS]; size_t count=0;
        int status=canids_observe(detector,&frame,hits,CANIDS_MAX_FINDINGS,&count);
        if (status!=CANIDS_OK) { failed=1; reason=status==CANIDS_TIME_REGRESSION ? "timestamp regression":"detector allocation/input failure"; break; }
        ++frames; alerted+=count!=0;
        for (size_t i=0;i<count;++i) {
            if (rt_log_event(log,&frame,line,&hits[i],received)<0) { failed=1; reason="event recording failed"; break; }
            ++events;
        }
    }
    if (log && rt_log_flush(log)<0) { failed=1; reason="event flush failed"; }
    if (!failed && !rt_interrupted && rt_input_finish(source)<0) { failed=1; reason="source integrity/EOF failure"; }
    char current[65];
    int unchanged=rt_hash_file(policy_path,current)==0 && !strcmp(current,policy.file_hash);
    add_bool(manifest,"policy_unchanged",unchanged);
    if (!unchanged) { failed=1; reason="policy changed during execution"; }
    json_object_object_add(manifest,"source",rt_input_metadata(source));
    rt_jint(manifest,"frames",frames); rt_jint(manifest,"events",events);
    int result=failed ? 2 : rt_interrupted ? 130 : 0;
    if (rt_interrupted) { result=130; reason="interrupted"; }
    if (!result) {
        json_object *runtime=json_object_new_object(),*stats=json_object_new_object();
        canids_stats cs; canids_get_stats(detector,&cs);
        rt_jint(stats,"initial_capacity_slots",cs.initial_capacity_slots); rt_jint(stats,"capacity_slots",cs.capacity_slots);
        rt_jint(stats,"window_grows",cs.window_grows); rt_jint(stats,"largest_active_id_window",cs.largest_active_id_window);
        rt_jint(stats,"window_capacity_bytes",cs.capacity_slots*sizeof(int64_t));
        rt_jint(runtime,"frames",frames); rt_jint(runtime,"events",events); rt_jint(runtime,"alerted_frames",alerted);
        json_object_object_add(runtime,"detector",stats); json_object_object_add(runtime,"logging",rt_log_stats(log));
        uint64_t duration=rt_clock_ns()-started; rt_jint(runtime,"processing_duration_ns",duration);
        json_object_object_add(runtime,"processing_frames_per_s",duration ? json_object_new_double((double)frames*1e9/(double)duration):NULL);
        struct rusage usage;
        if (getrusage(RUSAGE_SELF,&usage)==0) {
            json_object_object_add(runtime,"cpu_user_s",json_object_new_double(usage.ru_utime.tv_sec+usage.ru_utime.tv_usec/1e6));
            json_object_object_add(runtime,"cpu_system_s",json_object_new_double(usage.ru_stime.tv_sec+usage.ru_stime.tv_usec/1e6));
            rt_jint(runtime,"max_rss_kib",(uint64_t)usage.ru_maxrss);
        }
        rt_jint(runtime,"file_frames_skipped",0); json_object_object_add(runtime,"socketcan_rx_drops",NULL);
        if (rt_write_json(runtime_path,runtime)<0) { result=2; reason="runtime report write failed"; }
        json_object_put(runtime);
        FILE *summary=result ? NULL:fopen(summary_path,"wx");
        if (summary) {
            int wrote=fprintf(summary,"# Standalone C CAN IDS\n\nExperiment: %s\nPolicy: %s\nFrames: %" PRIu64 "; alerted frames: %" PRIu64 "; events: %" PRIu64 "\n\nPython is not used in detection. Evaluate truth separately with the external analysis tool.\nFile counts do not establish live CAN loss, real-time performance or prevention readiness.\n",run,policy.hash,frames,alerted,events);
            int closed=fclose(summary);
            if (wrote<0 || closed!=0) { result=2; reason="summary write failed"; }
        } else if (!result) { result=2; reason="summary open failed"; }
        if (!result) {
            json_object *outputs=json_object_new_object(); const char *names[]={"runtime.json","ids-events.jsonl","summary.md"};
            const char *paths[]={runtime_path,event_path,summary_path};
            for (size_t i=0;i<3;++i) {
                if (rt_hash_file(paths[i],current)<0) { result=2; reason="output digest failed"; break; }
                rt_jstr(outputs,names[i],current);
            }
            json_object_object_add(manifest,"outputs_sha256",outputs);
        }
    }
    if (result) { unlink(runtime_path); unlink(summary_path); rt_jstr(manifest,"error",reason); }
    rt_jstr(manifest,"status",result==130 ? "interrupted":result ? "failed":"complete");
    rt_wall_time(wall); rt_jstr(manifest,"completed_at",wall);
    if (rt_write_json(manifest_path,manifest)<0) { fprintf(stderr,"cannot persist final manifest\n"); result=2; }
    if (result) fprintf(stderr,"canids: %s\n",reason ? reason:"failed");
    else {
        json_object *name=json_object_new_string(out);
        printf("{\"output_dir\":%s,\"frames\":%" PRIu64 ",\"events\":%" PRIu64 "}\n",json_object_to_json_string_ext(name,JSON_C_TO_STRING_PLAIN),frames,events);
        json_object_put(name);
    }
    rt_log_close(log); rt_input_close(source); canids_destroy(detector); rt_policy_free(&policy);
    json_object_put(manifest); free(manifest_path); free(event_path); free(runtime_path); free(summary_path);
    return result;
}
