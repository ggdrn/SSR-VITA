#!/usr/bin/env python3
"""Run real boot enumeration/progress against temporary shader cache trees."""
from pathlib import Path
import subprocess,tempfile,os
root=Path(__file__).resolve().parents[1]
main=(root/'src/main.c').read_text()
assert main.index('gl_bridge_init();')<main.index('shader_cache_warmup();')<main.index('int r = unity_run();')
s=(root/'src/util/shader_warmup.c').read_text()
s='\n'.join(l for l in s.splitlines() if not (l.startswith('#include "') or l=='#include <vitaGL.h>'))
h=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static unsigned watchdog_frames,loaded,calls,done_count,total_count,prepared_count,draws,phase;
static uint64_t clock_us;
static uint64_t observe_now(void){clock_us+=60000;return clock_us;}
#define OBS_SHADER 1
#define L_INFO(...) ((void)0)
static void observe_span(unsigned k,const char *n,uint64_t t,uint64_t b,int st,uintptr_t obj){}
static const char *vglGetShaderCachePath(void){return "cache";}
static int vglPreloadShaderCacheFile(const char *p,size_t bytes){
 calls++;assert(bytes==32);
 // Newest entries are processed first, with deterministic path tie breaks.
 if(calls==1)assert(strstr(p,"BBBBBBBBBBBBBBBB.gxp"));
 if(strstr(p,"CCCCCCCCCCCCCCCC.gxp"))return -1;
 if(strstr(p,"DDDDDDDDDDDDDDDD.gxp"))return 0;
 loaded++;return 1;
}
static void port_shader_progress(unsigned p,unsigned done,unsigned total,unsigned prepared){
 assert(done<=total && prepared<=done);phase=p;draws++;
 if(p==1){assert(done>=done_count);done_count=done;total_count=total;prepared_count=prepared;}
}
'''+s+r'''
int main(int argc,char **argv){
 shader_cache_warmup();assert(phase==3 && draws>=3);
 if(argc==1){assert(!calls && !total_count);puts("PASS: empty cache before Unity");}
 else {assert(calls==4 && loaded==2 && total_count==4 && done_count==4 && prepared_count==2 && watchdog_frames>=4);puts("PASS: real file scan, filename validation, recent-first order, invalid/budget skips, real progress and boot order");}
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(h)
 subprocess.run(['cc','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],cwd=p,check=True)
 for stage,name,stamp in [('v','AAAAAAAAAAAAAAAA',10),('f','BBBBBBBBBBBBBBBB',20),('f','CCCCCCCCCCCCCCCC',5),('v','DDDDDDDDDDDDDDDD',4)]:
  q=p/'cache'/stage/name[:2]/(name+'.gxp');q.parent.mkdir(parents=True,exist_ok=True);q.write_bytes(bytes(32));os.utime(q,(stamp,stamp))
 q=p/'cache/v/AA/not-a-shader.gxp';q.write_bytes(bytes(32))
 (p/'cache/v/AA/AAAAAAAAAAAAAAAA.gxp.tmp').write_bytes(bytes(32))
 subprocess.run([str(p/'test'),'cached'],cwd=p,check=True)
