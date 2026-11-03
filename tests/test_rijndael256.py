#!/usr/bin/env python3
"""Compare native block routine against execution of actual original Mono IL."""
from pathlib import Path
import ctypes,os,re,shutil,subprocess,tempfile,zipfile
root=Path(__file__).resolve().parents[1];project=root/'tests/rijndael256'
dotnet=shutil.which('dotnet') or '/usr/local/share/dotnet/dotnet'
subprocess.run([dotnet,'build',str(project),'--configfile',str(project/'NuGet.Config'),
                '-p:UseSharedCompilation=false','--disable-build-servers','--verbosity','quiet'],check=True)
with tempfile.TemporaryDirectory() as d:
    p=Path(d)
    with zipfile.ZipFile(os.environ.get('SRR_TEST_APK',root.parent/'srr224.apk')) as z:
        (p/'mscorlib.dll').write_bytes(z.read('assets/bin/Data/Managed/mscorlib.dll'))
    header=(root/'src/util/rijndael256_il.h').read_text().split('rijndael256_il[] = {')[1]
    (p/'original.il').write_bytes(bytes(int(x,16) for x in re.findall('0x([0-9a-f]{2})',header)))
    source=(root/'src/util/rijndael256.c').read_text()
    namespace=re.search(r'klass\(image, "([^"]+)", "RijndaelTransform"\)',source).group(1)
    subprocess.run([dotnet,str(project/'bin/Debug/net10.0/rijndael256.dll'),str(p/'mscorlib.dll'),str(p/'original.il'),str(p/'cases.bin'),namespace],check=True)
    (p/'test.c').write_text(r'''
#include <assert.h>
#include <stdio.h>
#include "util/rijndael256_block.h"
int main(int argc,char **argv) {
 FILE *f=fopen(argv[1],"rb");assert(f);unsigned cases=0;
 uint8_t input[32],expected[32],output[40];uint32_t keys[120];rijndael256_tables t;
 while(fread(input,32,1,f)==1) {
  assert(fread(expected,32,1,f)==1 && fread(keys,sizeof(keys),1,f)==1 && fread(&t,sizeof(t),1,f)==1);
  memset(output,0xa5,sizeof(output));
  rijndael256_block(input,output,keys,&t);assert(!memcmp(output,expected,32));
  for(int i=32;i<40;i++)assert(output[i]==0xa5);
  memcpy(output,input,32);rijndael256_block(output,output,keys,&t);assert(!memcmp(output,expected,32));cases++;
 }
 assert(cases==4096);fclose(f);puts("PASS: 4096 native blocks match original IL byte-for-byte; in-place and output bounds (ASan/UBSan)");
}
''')
    subprocess.run(['cc','-O2','-fsanitize=address,undefined','-I',str(root/'src'),str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test'),str(p/'cases.bin')],check=True)
