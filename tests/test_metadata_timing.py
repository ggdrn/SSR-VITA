#!/usr/bin/env python3
"""Metadata instrumentation must preserve stat/access results and errno."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];src=(root/'src/bridges/libc_bridge.c').read_text()
stat=src[src.index('int stat_bridge('):src.index('int fstat_bridge(')]
access=src[src.index('int access_bridge('):src.index('int rename_bridge(')]
h=r'''
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <sys/stat.h>
typedef struct {unsigned st_mode;long long st_size;} bionic_stat;
static int failure,bionic_errno,ended,stat_calls;
static int *__errno_bionic(void){return &bionic_errno;}
#define SET_ERRNO() (bionic_errno=errno)
#define WORK_META 7
static int is_random_device(const char *p){return !strcmp(p,"random");}
static const char *fs_translate(const char *p,char *b,size_t n){return p;}
static uint64_t work_trace_begin(void){return 100;}
static void work_trace_end(int k,uint64_t start,int n,uintptr_t caller){assert(k==WORK_META&&start==100);ended++;errno=EIO;}
static int fake_stat(const char *p,struct stat *s){stat_calls++;errno=failure?EACCES:123;s->st_size=987;s->st_mode=S_IFREG;return failure?-1:0;}
static void stat_to_bionic(const struct stat *s,bionic_stat *b){b->st_mode=s->st_mode;b->st_size=s->st_size;}
#define stat(p,s) fake_stat(p,s)
'''+stat+access+r'''
int main(void){
 bionic_stat result={0};
 assert(!stat_bridge("file",&result));assert(result.st_size==987&&result.st_mode==S_IFREG&&errno==123);
 failure=1;result.st_size=99;assert(stat_bridge("file",&result)==-1);assert(result.st_size==99&&errno==EACCES&&bionic_errno==EACCES);
 assert(access_bridge("file",0)==-1&&bionic_errno==ENOENT&&errno==EACCES);
 failure=0;assert(!access_bridge("file",0)&&errno==123);
 assert(ended==4&&stat_calls==4);
 assert(!stat_bridge("random",&result));assert(!access_bridge("random",0));assert(ended==4&&stat_calls==4);
 puts("PASS: stat/access results, output, native/bionic errno and random-device shortcuts preserved under timing");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(h)
 subprocess.run(['cc','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
