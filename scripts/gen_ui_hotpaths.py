#!/usr/bin/env python3
"""Regenerate panel IL replacement from the guarded original bodies in the headers.

No APK is modified. Regression tests compare those originals with APK metadata
and execute the replacement. Run from any directory.
"""
from pathlib import Path
import re,struct
root=Path(__file__).resolve().parents[1]
# UpdateWidgets: retain original order and fallback; skip redundant owner getter/equality
# only when the cached owner is the exact live panel running this method.
header=(root/'src/util/ui_panel_il.h').read_text()
b=bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})',header.split('ui_panel_original[] = {')[1].split('};')[0]))
# Decode the limited opcode set in the fully guarded original UpdateWidgets.
ins=[];off=0
while off<len(b):
 op=b[off];ins.append((off,'br.s' if op==0x2b else ''))
 if op in (0x28,0x6f,0x7b,0x7e,0x80,0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f):off+=5
 elif op in (0x11,0x13,0x2b,0xfe):off+=2
 else:
  assert op in (0x02,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x14,0x16,0x17,0x2a,0x58),hex(op)
  off+=1
assert off==len(b)
longs={0x38:0x2b,0x39:0x2c,0x3a:0x2d,0x3b:0x2e,0x3c:0x2f,0x3d:0x30,0x3e:0x31,0x3f:0x32}
short=set();newpos={}
for _ in range(8):
 pos=0
 for i,(off,op) in enumerate(ins):
  newpos[off]=pos
  if off==0x5b:pos+=10
  end=ins[i+1][0] if i+1<len(ins) else len(b)
  pos+=2 if off in short else end-off
 newpos[len(b)]=pos
 for off,op in ins:
  if b[off] in longs:
   target=off+5+struct.unpack_from('<i',b,off+1)[0]
   if -128<=newpos[target]-(newpos[off]+2)<=127:short.add(off)
out=bytearray()
for i,(off,op) in enumerate(ins):
 assert len(out)==newpos[off]
 if off==0x5b:
  out.extend(bytes.fromhex('11047baf02000402')) # ldloc4; ldfld mPanel; ldarg0
  out.extend([0x2e,newpos[0x6d]-(len(out)+2)]) # beq.s -> geometry update
 end=ins[i+1][0] if i+1<len(ins) else len(b)
 instr=b[off:end]
 if b[off] in longs:
  target=off+5+struct.unpack_from('<i',b,off+1)[0]
  if off in short:out.extend([longs[b[off]],(newpos[target]-len(out)-2)&255])
  else:out.extend(bytes([b[off]])+struct.pack('<i',newpos[target]-len(out)-5))
 elif op.startswith('br.s'):
  target=off+2+struct.unpack_from('b',b,off+1)[0]
  out.extend([b[off],(newpos[target]-len(out)-2)&255])
 else:out.extend(instr)
assert len(out)<=len(b)
out=out[:-1]+bytes(len(b)-len(out))+out[-1:]
(root/'src/util/ui_panel_il.h').write_text('// Supported APK UIPanel.UpdateWidgets. Generated owner shortcut; original fallback intact.\n#pragma once\n#include <stdint.h>\n'+''.join('static const uint8_t '+name+'[] = {'+','.join(f'0x{x:02x}' for x in body)+'};\n' for name,body in [('ui_panel_original',b),('ui_panel_owned',out)]))
print('panel',len(out),'shortcut at',newpos[0x5b])
