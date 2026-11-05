#!/usr/bin/env python3
"""Compare native CBC directly against execution of the APK's CBC + block IL."""
from pathlib import Path
import re,shutil,subprocess,tempfile,zipfile
root=Path(__file__).resolve().parents[1];project=root/'tests/rijndael_cbc'
dotnet=shutil.which('dotnet') or '/usr/local/share/dotnet/dotnet'
subprocess.run([dotnet,'build',str(project),'--configfile',str(project/'NuGet.Config'),'-p:UseSharedCompilation=false','--disable-build-servers','--verbosity','quiet'],check=True)
with tempfile.TemporaryDirectory() as d:
 p=Path(d)
 with zipfile.ZipFile(root.parent/'srr224.apk') as z:(p/'mscorlib.dll').write_bytes(z.read('assets/bin/Data/Managed/mscorlib.dll'))
 for filename,names in [('rijndael256_il.h',['rijndael256_il']),('rijndael_cbc_il.h',['rijndael_cbc_il','rijndael_ecb_il'])]:
  header=(root/'src/util'/filename).read_text()
  for name in names:
   text=header.split(name+'[] = {')[1].split('};')[0]
   (p/name).write_bytes(bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})',text)))
 subprocess.run([dotnet,str(project/'bin/Debug/net10.0/rijndael_cbc.dll'),str(p/'mscorlib.dll'),str(p/'rijndael256_il'),str(p/'cases.bin'),'System.Security.Cryptography',str(p/'rijndael_cbc_il'),str(p/'rijndael_ecb_il')],check=True)
 (p/'test.c').write_text(r'''
#include <assert.h>
#include <stdio.h>
#include "util/rijndael256_block.h"
int main(int argc,char **argv){
 FILE *f=fopen(argv[1],"rb");assert(f);unsigned count=0,alias;
 uint8_t input[40],output[40],iv[40],scratch[40],eo[40],ei[40],es[40];
 uint32_t keys[120];rijndael256_tables t;
 while(fread(&alias,4,1,f)==1){
  assert(fread(input,40,1,f)==1 && fread(output,40,1,f)==1 && fread(iv,40,1,f)==1 && fread(scratch,40,1,f)==1);
  assert(fread(eo,40,1,f)==1 && fread(ei,40,1,f)==1 && fread(es,40,1,f)==1);
  assert(fread(keys,sizeof(keys),1,f)==1 && fread(&t,sizeof(t),1,f)==1);
  uint8_t *out=alias?input:output;
  rijndael256_cbc_block(input,out,iv,scratch,keys,&t);
  assert(!memcmp(out,eo,40) && !memcmp(iv,ei,40) && !memcmp(scratch,es,40));count++;
 }
 assert(count==4096);fclose(f);
 puts("PASS: native CBC matches original IL for 4096 chained blocks, in-place, full IV/scratch state and tail sentinels (ASan/UBSan)");
}
''')
 subprocess.run(['cc','-O2','-fsanitize=address,undefined','-I',str(root/'src'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test'),str(p/'cases.bin')],check=True)
