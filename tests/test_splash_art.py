#!/usr/bin/env python3
"""Exercise real overlay against APK splits, including boundary reads and rejection."""
from pathlib import Path
import subprocess, tempfile, zipfile
from PIL import Image
root = Path(__file__).resolve().parents[1]
src = '\n'.join(l for l in (root/'src/util/splash_art.c').read_text().splitlines() if not l.startswith('#include'))
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/splash_art_data.h"
typedef struct { uint32_t size; } apk_entry;
static apk_entry entries[5];
static uint8_t *original;
#define GAME_SPLASH_PATH "art.rgb"
#define L_WARN(...) ((void)0)
#define L_INFO(...) ((void)0)
static const apk_entry *apk_find(const char *name) {
 for (int i=0;i<5;i++) if (!strcmp(name,game_splash_parts[i].name)) return entries+i;
 return NULL;
}
static void *apk_extract(const apk_entry *e,size_t *size) {
 *size=e->size;void *p=malloc(*size);memcpy(p,original+(e-entries)*1048576,*size);return p;
}
'''+src+r'''
int main(void) {
 size_t n=5*1048576; original=malloc(n);FILE *f=fopen("original","rb");assert(f);
 assert(fread(original,1,n,f)==n);fclose(f);
 for(int i=0;i<5;i++) entries[i].size=1048576;
 splash_art_prepare();assert(payload);
 uint8_t *expected=malloc(n),*got=malloc(n);memcpy(expected,original,n);
 memcpy(expected+GAME_SPLASH_START,payload,GAME_SPLASH_SIZE);
 uint32_t steps[]={1,43,1024,1048576};
 for(int k=0;k<4;k++) {
  memcpy(got,original,n);
  for(int part=0;part<5;part++) for(uint32_t at=0;at<1048576;) {
   uint32_t amount=steps[k];if(amount>1048576-at)amount=1048576-at;
   splash_art_apply(entries+part,at,got+part*1048576+at,amount);at+=amount;
  }
  assert(!memcmp(got,expected,n));
 }
 original[GAME_SPLASH_HEADER_AT]^=1;splash_art_prepare();assert(!payload);
 original[GAME_SPLASH_HEADER_AT]^=1;
 f=fopen("art.rgb","r+b");assert(f);fputc(0,f);fclose(f);
 splash_art_prepare();assert(!payload);
 memcpy(got,original,n);splash_art_apply(entries,0,got,1048576);assert(!memcmp(got,original,n));
 free(original);free(expected);free(got);
 puts("PASS: fan art full/partial reads, split boundaries, untouched other bytes, header/hash rejection");
}
'''
with tempfile.TemporaryDirectory() as d:
    p = Path(d)
    with zipfile.ZipFile(root.parent/'srr224.apk') as z:
        (p/'original').write_bytes(b''.join(z.read(f'assets/bin/Data/sharedassets0.assets.split{i}') for i in range(5)))
    art = (root/'assets/game-splash.rgb').read_bytes()
    assert len(art) == 4194309 and art[0] != 0
    # Decode every mip footprint: Unity stores RGB rows bottom to top.
    image = Image.open(root/'assets/livearea/fan-background.png').convert('RGB')
    at, width, height = 0, 2048, 512
    while True:
        expected = image.resize((width,height),Image.Resampling.LANCZOS).transpose(Image.Transpose.FLIP_TOP_BOTTOM).tobytes()
        assert art[at:at+len(expected)] == expected
        at += len(expected)
        if width == height == 1: break
        width,height = max(1,width//2),max(1,height//2)
    assert at == len(art)
    (p/'art.rgb').write_bytes(art)
    (p/'test.c').write_text(harness)
    subprocess.run(['cc','-fsanitize=address,undefined','-I',str(root/'src'),str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],cwd=p,check=True)
