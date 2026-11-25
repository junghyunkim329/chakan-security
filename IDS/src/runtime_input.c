#define _XOPEN_SOURCE 700
#include "runtime.h"
#include <archive.h>
#include <archive_entry.h>
#include <openssl/evp.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct rt_input {
    FILE *file;
    struct archive *zip;
    char *path, *member, *record;
    size_t capacity, length, at, available;
    unsigned char buffer[BUFSIZ];
    int pending, eof, columns;
    uint64_t line, rows, growths;
    struct stat initial;
    EVP_MD_CTX *member_hash, *frames_hash;
    json_object *metadata;
};
static int failure(rt_input *s,const char *why) {
    fprintf(stderr,"%s:line %" PRIu64 ": %s\n",s->member ? s->member:s->path,s->line,why); return -1;
}
static int byte(rt_input *s) {
    if (s->pending>=0) { int c=s->pending; s->pending=-1; return c; }
    if (s->at==s->available) {
        if (s->eof) return EOF;
        ssize_t got=s->zip ? archive_read_data(s->zip,s->buffer,sizeof(s->buffer)):
            (ssize_t)fread(s->buffer,1,sizeof(s->buffer),s->file);
        if (got<0 || (!s->zip && ferror(s->file))) return -2;
        if (!got) { s->eof=1; return EOF; }
        if (EVP_DigestUpdate(s->member_hash,s->buffer,(size_t)got)!=1) return -2;
        s->at=0; s->available=(size_t)got;
    }
    return s->buffer[s->at++];
}
/* One reusable record. Quotes may contain commas, escaped quotes and newlines. */
static int record(rt_input *s,char *fields[6],size_t *count) {
    enum { START, PLAIN, QUOTED, AFTER } state=START;
    s->length=0; int any=0;
    for (;;) {
        int c=byte(s);
        if (c==-2) return failure(s,"read/decompression/CRC error");
        if (c==EOF) {
            if (!any) return 0;
            if (state==QUOTED) return failure(s,"unterminated quoted CSV field");
            ++s->line; break;
        }
        any=1;
        if (!c) return failure(s,"NUL in CSV");
        if (state!=QUOTED && (c=='\n' || c=='\r')) {
            if (c=='\r') { int next=byte(s); if (next==-2) return failure(s,"read error"); if (next!=EOF && next!='\n') s->pending=next; }
            ++s->line; break;
        }
        if (c=='\n') ++s->line;
        if (state==START) state=c=='"' ? QUOTED : c==',' ? START : PLAIN;
        else if (state==PLAIN) { if (c==',') state=START; }
        else if (state==QUOTED) { if (c=='"') state=AFTER; }
        else if (state==AFTER) {
            if (c=='"') state=QUOTED;
            else if (c==',') state=START;
            else return failure(s,"unexpected text after CSV closing quote");
        }
        if (s->length+1>=s->capacity) {
            size_t n=s->capacity ? s->capacity*2:BUFSIZ;
            if (n<=s->capacity) return failure(s,"CSV record size overflow");
            char *v=realloc(s->record,n); if (!v) return failure(s,"CSV memory exhausted");
            s->record=v; s->capacity=n; ++s->growths;
        }
        s->record[s->length++]=(char)c;
    }
    if (!s->record) { s->record=malloc(1); if (!s->record) return -1; s->capacity=1; }
    s->record[s->length]='\0';
    for (size_t i=0;i<s->length;) {
        size_t used; uint32_t cp;
        if (rt_utf8((unsigned char *)s->record+i,s->length-i,&used,&cp)<0) return failure(s,"invalid UTF-8");
        i+=used;
    }
    char *read=s->record,*write=s->record; *count=0;
    for (;;) {
        if (*count>=6) return failure(s,"too many CSV fields");
        fields[(*count)++]=write;
        if (*read=='"') {
            ++read;
            while (*read) {
                if (*read=='"') { ++read; if (*read!='"') break; }
                *write++=*read++;
            }
        } else while (*read && *read!=',') *write++=*read++;
        int more=*read==','; if (more) ++read;
        *write++='\0'; if (!more) break;
    }
    return 1;
}
static int unsigned_decimal(const char *s,uint64_t max,uint64_t *out) {
    if (!*s) return -1;
    uint64_t n=0;
    for (;*s;++s) {
        if (*s<'0' || *s>'9' || (unsigned)(*s-'0')>max || n>(max-(unsigned)(*s-'0'))/10) return -1;
        n=n*10+(unsigned)(*s-'0');
    }
    *out=n; return 0;
}
static int stamp(const char *s,int64_t *out) {
    if (*s<'0' || *s>'9') return -1;
    uint64_t whole=0,fraction=0; unsigned digits=0;
    while (*s && *s!='.') {
        if (*s<'0' || *s>'9' || whole>((uint64_t)INT64_MAX/1000000-(unsigned)(*s-'0'))/10) return -1;
        whole=whole*10+(unsigned)(*s++-'0');
    }
    if (*s=='.') {
        ++s; if (!*s) return -1;
        for (;*s;++s) {
            if (*s<'0' || *s>'9' || (digits>=6 && *s!='0')) return -1;
            if (digits<6) fraction=fraction*10+(unsigned)(*s-'0');
            if (digits<6) ++digits;
        }
    }
    while (digits++<6) fraction*=10;
    if (whole>((uint64_t)INT64_MAX-fraction)/1000000) return -1;
    *out=(int64_t)(whole*1000000+fraction); return 0;
}
static int nibble(char c) {
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    if (c>='A' && c<='F') return c-'A'+10;
    return -1;
}
rt_input *rt_input_open(const char *path,const char *member) {
    rt_input *s=calloc(1,sizeof(*s)); if (!s) return NULL;
    s->pending=-1; s->path=realpath(path,NULL); s->member=member ? strdup(member):NULL;
    s->metadata=json_object_new_object();
    if (!s->path || (member && !s->member) || !s->metadata || stat(s->path,&s->initial)!=0 || !S_ISREG(s->initial.st_mode)) goto bad;
    char hash[65]; if (rt_hash_file(s->path,hash)<0) goto bad;
    rt_jstr(s->metadata,"input",s->path); rt_jstr(s->metadata,"member",member);
    rt_jstr(s->metadata,"input_sha256",hash); rt_jint(s->metadata,"rows",0);
    json_object_object_add(s->metadata,"complete",json_object_new_boolean(0));
    s->member_hash=EVP_MD_CTX_new(); s->frames_hash=EVP_MD_CTX_new();
    if (!s->member_hash || !s->frames_hash || EVP_DigestInit_ex(s->member_hash,EVP_sha256(),NULL)!=1 || EVP_DigestInit_ex(s->frames_hash,EVP_sha256(),NULL)!=1) goto bad;
    if (member) {
        s->zip=archive_read_new(); if (!s->zip) goto bad;
        archive_read_support_format_zip(s->zip);
        if (archive_read_open_filename(s->zip,s->path,BUFSIZ)!=ARCHIVE_OK) goto bad;
        struct archive_entry *entry; int found=0,status;
        while ((status=archive_read_next_header(s->zip,&entry))==ARCHIVE_OK) {
            if (!strcmp(archive_entry_pathname(entry),member)) {
                if (archive_entry_filetype(entry)!=AE_IFREG) goto bad;
                found=1; break;
            }
            if (archive_read_data_skip(s->zip)!=ARCHIVE_OK) goto bad;
        }
        if (!found) goto bad;
    } else { s->file=fopen(s->path,"rb"); if (!s->file) goto bad; }
    char *fields[6]; size_t count;
    if (record(s,fields,&count)!=1 || (count!=5 && count!=6)) goto bad;
    const char *expected[]={"Interface","Timestamp","Arbitration_ID","DLC","Data","Label"};
    if (!strncmp(fields[0],"\xef\xbb\xbf",3)) fields[0]+=3;
    for (size_t i=0;i<count;++i) if (strcmp(fields[i],expected[i])) goto bad;
    s->columns=(int)count;
    json_object_object_add(s->metadata,"has_labels",json_object_new_boolean(count==6));
    return s;
bad:
    fprintf(stderr,"cannot open valid AutoHack CSV/ZIP member: %s\n",path); rt_input_close(s); return NULL;
}
int rt_input_next(rt_input *s,canids_frame *f,uint64_t *line) {
    char *fields[6]; size_t count; int status=record(s,fields,&count);
    if (status<=0) return status;
    if (count!=(size_t)s->columns || (count==6 && !*fields[5])) return failure(s,"CSV field count or empty label");
    memset(f,0,sizeof(*f)); unsigned bus=0;
    while (bus<3 && strcmp(fields[0],rt_bus(bus))) ++bus;
    if (bus==3 || stamp(fields[1],&f->timestamp_us)<0) return failure(s,"invalid bus or exact microsecond timestamp");
    f->bus=(uint8_t)bus; uint64_t dlc;
    if (unsigned_decimal(fields[3],8,&dlc)<0) return failure(s,"invalid DLC");
    f->dlc=(uint8_t)dlc;
    const char *id=fields[2]; if (id[0]=='0' && (id[1]=='x' || id[1]=='X')) id+=2;
    if (!*id) return failure(s,"invalid hexadecimal ID");
    unsigned value=0;
    for (;*id;++id) { int n=nibble(*id); if (n<0 || value>(2047u-(unsigned)n)/16) return failure(s,"ID outside 11-bit range"); value=value*16+(unsigned)n; }
    f->can_id=(uint16_t)value;
    const char *data=fields[4]; unsigned bytes=0;
    while (*data) {
        while (*data && strchr(" \t\r\n\v\f",*data)) ++data;
        if (!*data) break;
        if (bytes==8 || !data[1] || nibble(data[0])<0 || nibble(data[1])<0 || (data[2] && !strchr(" \t\r\n\v\f",data[2]))) return failure(s,"invalid HEX payload bytes");
        f->data[bytes++]=(uint8_t)(nibble(data[0])*16+nibble(data[1])); data+=2;
    }
    if (bytes!=f->dlc) return failure(s,"DLC differs from payload length");
    char hex[17],canonical[128];
    for (unsigned i=0;i<f->dlc;++i) snprintf(hex+i*2,3,"%02x",f->data[i]);
    hex[2*f->dlc]=0;
    int n=snprintf(canonical,sizeof(canonical),"%s:%" PRId64 ":%u:%u:%s\n",rt_bus(f->bus),f->timestamp_us,f->can_id,f->dlc,hex);
    if (n<0 || (size_t)n>=sizeof(canonical) || EVP_DigestUpdate(s->frames_hash,canonical,(size_t)n)!=1) return failure(s,"frame digest failure");
    ++s->rows; *line=s->line; return 1;
}
json_object *rt_input_metadata(rt_input *s) {
    if (!s) return NULL;
    rt_jint(s->metadata,"rows",s->rows); rt_jint(s->metadata,"record_buffer_bytes",s->capacity);
    rt_jint(s->metadata,"record_buffer_growths",s->growths);
    return json_object_get(s->metadata);
}
int rt_input_finish(rt_input *s) {
    if (!s->eof) return failure(s,"source not consumed to EOF");
    if (s->zip) {
        struct archive_entry *entry; int status;
        while ((status=archive_read_next_header(s->zip,&entry))==ARCHIVE_OK) {
            if (!strcmp(archive_entry_pathname(entry),s->member) || archive_read_data_skip(s->zip)!=ARCHIVE_OK) return failure(s,"duplicate member or ZIP failure");
        }
        if (status!=ARCHIVE_EOF) return failure(s,"ZIP did not complete");
    }
    struct stat current;
    if (stat(s->path,&current)!=0 || s->initial.st_size!=current.st_size ||
        s->initial.st_mtim.tv_sec!=current.st_mtim.tv_sec || s->initial.st_mtim.tv_nsec!=current.st_mtim.tv_nsec ||
        s->initial.st_ino!=current.st_ino || s->initial.st_dev!=current.st_dev) return failure(s,"source changed while reading");
    EVP_MD_CTX *hashes[]={s->member_hash,s->frames_hash}; const char *keys[]={"member_sha256","frames_sha256"};
    for (int i=0;i<2;++i) {
        unsigned char bytes[EVP_MAX_MD_SIZE]; unsigned n; char hex[65];
        if (EVP_DigestFinal_ex(hashes[i],bytes,&n)!=1) return -1;
        for (unsigned j=0;j<n;++j) snprintf(hex+2*j,3,"%02x",bytes[j]);
        rt_jstr(s->metadata,keys[i],hex);
    }
    json_object_object_add(s->metadata,"complete",json_object_new_boolean(1)); return 0;
}
void rt_input_close(rt_input *s) {
    if (!s) return;
    if (s->file) fclose(s->file);
    if (s->zip) archive_read_free(s->zip);
    EVP_MD_CTX_free(s->member_hash); EVP_MD_CTX_free(s->frames_hash);
    json_object_put(s->metadata); free(s->path); free(s->member); free(s->record); free(s);
}
