#!/usr/bin/env python3
"""Validate the real read overlay against original APK bytes and partial reads."""
from pathlib import Path
import zipfile,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
src='\n'.join(l for l in (root/'src/util/title_layout.c').read_text().splitlines() if not l.startswith('#include'))
harness=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/title_layout_data.h"
typedef struct { uint32_t size; } apk_entry;
static apk_entry entry={TITLE_MAIN_DATA_SIZE};
static uint8_t *blob;
#define L_WARN(...) ((void)0)
#define L_INFO(...) ((void)0)
static const apk_entry *apk_find(const char *name) { assert(!strcmp(name,"assets/bin/Data/mainData")); return &entry; }
static void *apk_extract(const apk_entry *e,size_t *size) { *size=TITLE_MAIN_DATA_SIZE;void *out=malloc(*size);memcpy(out,blob,*size);return out; }
'''
harness+=src
harness+=r'''
int main(int argc,char **argv) {
 FILE *f=fopen(argv[1],"rb");assert(f);
 blob=malloc(TITLE_MAIN_DATA_SIZE);assert(fread(blob,1,TITLE_MAIN_DATA_SIZE,f)==TITLE_MAIN_DATA_SIZE);fclose(f);
 title_layout_prepare();assert(title_enabled);
 uint8_t *expected=malloc(TITLE_MAIN_DATA_SIZE);memcpy(expected,blob,TITLE_MAIN_DATA_SIZE);
 for(unsigned i=0;i<sizeof(title_layout_objects)/sizeof(*title_layout_objects);i++)
  memcpy(expected+title_layout_objects[i].offset,title_layout_objects[i].replacement,title_layout_objects[i].size);
 uint32_t steps[]={1,7,43,1024,65536};uint8_t *got=malloc(TITLE_MAIN_DATA_SIZE);
 for(unsigned k=0;k<5;k++) {
  memcpy(got,blob,TITLE_MAIN_DATA_SIZE);
  for(uint32_t pos=0;pos<TITLE_MAIN_DATA_SIZE;) {
   uint32_t n=steps[k];if(n>TITLE_MAIN_DATA_SIZE-pos)n=TITLE_MAIN_DATA_SIZE-pos;
   title_layout_apply(&entry,pos,got+pos,n);pos+=n;
  }
  assert(!memcmp(got,expected,TITLE_MAIN_DATA_SIZE));
 }
 blob[title_layout_objects[1].offset+4]^=1;title_layout_prepare();assert(!title_enabled);
 memcpy(got,blob,TITLE_MAIN_DATA_SIZE);title_layout_apply(&entry,0,got,TITLE_MAIN_DATA_SIZE);assert(!memcmp(got,blob,TITLE_MAIN_DATA_SIZE));
 puts("PASS: title layout matches original APK objects; all chunk sizes identical; mismatch disables all edits");
 free(blob);free(expected);free(got);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)
 with zipfile.ZipFile(root.parent/'srr224.apk') as z:
  (p/'mainData').write_bytes(z.read('assets/bin/Data/mainData'))
 (p/'test.c').write_text(harness)
 subprocess.run(['cc','-fsanitize=address,undefined','-I',str(root/'src'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test'),str(p/'mainData')],check=True)
