/* 资源源后端实现: dir / mem */
#include "efml4a.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

void efml4a_source_free(efml4a_source *s){
    if(!s) return;
    if(s->close) s->close(s);
    free(s);
}

/* ---------------- 目录后端 ---------------- */
typedef struct {
    char dir[1024];
    char **names;
    long  *lens;
    long   n;
} DirCtx;

static int dir_open(efml4a_source *s){
    DirCtx *c = (DirCtx*)s->ctx;
    DIR *d = opendir(c->dir);
    if(!d) return -1;
    struct dirent *e;
    long cap=0;
    while((e = readdir(d))){
        if(e->d_name[0]=='.') continue;
        if(c->n >= cap){
            cap = cap? cap*2 : 64;
            c->names = realloc(c->names, cap*sizeof(char*));
            c->lens  = realloc(c->lens,  cap*sizeof(long));
        }
        char full[2048];
        snprintf(full,sizeof full,"%s/%s",c->dir,e->d_name);
        struct stat st;
        if(stat(full,&st)!=0 || !S_ISREG(st.st_mode)) continue;
        c->names[c->n] = strdup(e->d_name);
        c->lens[c->n]  = (long)st.st_size;
        c->n++;
    }
    closedir(d);
    return 0;
}
static void dir_close(efml4a_source *s){
    DirCtx *c = (DirCtx*)s->ctx;
    for(long i=0;i<c->n;i++) free(c->names[i]);
    free(c->names); free(c->lens);
}
static long dir_count(efml4a_source *s){ return ((DirCtx*)s->ctx)->n; }
static int dir_stat(efml4a_source *s, long idx, char *name, size_t namesz,
                    long *off, long *len){
    DirCtx *c = (DirCtx*)s->ctx;
    if(idx<0 || idx>=c->n) return -1;
    if(name && namesz){ strncpy(name, c->names[idx], namesz-1); name[namesz-1]=0; }
    if(off) *off = idx;              /* 目录后端没有全局偏移，用索引 */
    if(len) *len = c->lens[idx];
    return 0;
}
static long dir_find(efml4a_source *s, const char *name){
    DirCtx *c = (DirCtx*)s->ctx;
    for(long i=0;i<c->n;i++) if(!strcmp(c->names[i],name)) return i;
    return -1;
}
static long dir_read(efml4a_source *s, long off, long len, uint8_t *out){
    DirCtx *c = (DirCtx*)s->ctx;
    if(off<0 || off>=c->n) return -1;
    char full[2048];
    snprintf(full,sizeof full,"%s/%s",c->dir,c->names[off]);
    FILE *f = fopen(full,"rb");
    if(!f) return -1;
    if(len > c->lens[off]) len = c->lens[off];
    size_t n = fread(out,1,(size_t)len,f);
    fclose(f);
    return (long)n;
}
static int dir_can_write(efml4a_source *s){ (void)s; return 1; }

efml4a_source *efml4a_source_dir_new(const char *dirpath){
    efml4a_source *s = calloc(1,sizeof(*s));
    DirCtx *c = calloc(1,sizeof(*c));
    if(!s||!c){ free(s); free(c); return NULL; }
    strncpy(c->dir, dirpath, sizeof(c->dir)-1);
    s->name="dir"; s->ctx=c;
    s->open=dir_open; s->close=dir_close;
    s->count=dir_count; s->stat=dir_stat; s->find=dir_find;
    s->read=dir_read; s->can_write=dir_can_write;
    return s;
}

/* ---------------- 内存后端 ---------------- */
typedef struct {
    uint8_t *data;
    long len;
    int own;
} MemCtx;

static int mem_open(efml4a_source *s){ (void)s; return 0; }
static void mem_close(efml4a_source *s){
    MemCtx *c = (MemCtx*)s->ctx;
    if(c->own) free(c->data);
}
static long mem_count(efml4a_source *s){ (void)s; return 1; }
static int mem_stat(efml4a_source *s, long idx, char *name, size_t namesz,
                    long *off, long *len){
    MemCtx *c = (MemCtx*)s->ctx;
    if(idx!=0) return -1;
    if(name && namesz){ strncpy(name,"<memory>",namesz-1); name[namesz-1]=0; }
    if(off) *off=0;
    if(len) *len=c->len;
    return 0;
}
static long mem_read(efml4a_source *s, long off, long len, uint8_t *out){
    MemCtx *c = (MemCtx*)s->ctx;
    if(off<0 || off>c->len) return -1;
    if(off+len > c->len) len = c->len - off;
    memcpy(out, c->data+off, (size_t)len);
    return len;
}
static int mem_can_write(efml4a_source *s){ (void)s; return 1; }
static long mem_write(efml4a_source *s, long off, long len, const uint8_t *in){
    MemCtx *c = (MemCtx*)s->ctx;
    if(off<0 || off+len > c->len) return -1;
    memcpy(c->data+off, in, (size_t)len);
    return len;
}

efml4a_source *efml4a_source_mem_new(uint8_t *data, long len, int own){
    efml4a_source *s = calloc(1,sizeof(*s));
    MemCtx *c = calloc(1,sizeof(*c));
    if(!s||!c){ free(s); free(c); return NULL; }
    c->data=data; c->len=len; c->own=own;
    s->name="mem"; s->ctx=c;
    s->open=mem_open; s->close=mem_close;
    s->count=mem_count; s->stat=mem_stat;
    s->read=mem_read; s->can_write=mem_can_write; s->write=mem_write;
    return s;
}
