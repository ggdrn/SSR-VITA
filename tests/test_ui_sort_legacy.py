#!/usr/bin/env python3
"""Conservative legacy-JIT gate: no evaluation stack carried across back edges.

This is a source acceptance rule, not an emulation of Unity's Mono JIT.
The rejected 0.0.83 program passes modern .NET execution tests; keep it as
an explicit negative control so those tests cannot alone approve it again.
"""
from pathlib import Path
import json,re,struct
root=Path(__file__).resolve().parents[1]

def check(body):
    instructions={}; off=0
    while off<len(body):
        op=body[off]
        if op in (0x6f,0x7b,0xa3,0xa4) or 0x38<=op<=0x44:n=5
        elif 0x2b<=op<=0x37:n=2
        else:n=1
        instructions[off]=(op,n);off+=n
    assert off==len(body)
    seen={};todo=[(0,0)];back=[]
    while todo:
        off,depth=todo.pop()
        if off in seen:
            assert seen[off]==depth,('inconsistent stack',off)
            continue
        seen[off]=depth;op,n=instructions[off]
        if op in (0x02,0x03,0x06,0x07,0x08,0x16,0x17):pop,push=0,1
        elif op in (0x0a,0x0b,0x0c,0x26):pop,push=1,0
        elif op==0x25:pop,push=1,2
        elif op==0x7b:pop,push=1,1
        elif op==0xa3:pop,push=2,1
        elif op==0xa4:pop,push=3,0
        elif op==0x6f:
            assert struct.unpack_from('<I',body,off+1)[0]==0x0a000183
            pop,push=3,1 # Comparison<T>.Invoke(this, left, right)
        elif op in (0x58,0x59):pop,push=2,1
        elif op in (0x2c,0x2d,0x39,0x3a):pop,push=1,0
        elif 0x2e<=op<=0x37 or 0x3b<=op<=0x44:pop,push=2,0
        elif op in (0x00,0x2a,0x2b,0x38):pop,push=0,0
        else:raise AssertionError(('unsupported opcode',hex(op)))
        assert depth>=pop,('stack underflow',off)
        depth+=push-pop;nextoff=off+n
        if op==0x2a:
            assert depth==0
            continue
        if 0x2b<=op<=0x44:
            delta=struct.unpack_from('<b' if n==2 else '<i',body,off+1)[0]
            target=nextoff+delta
            if target<=off:back.append((off,target,depth))
            todo.append((target,depth))
            if op in (0x2b,0x38):continue
        todo.append((nextoff,depth))
    return all(depth==0 for _,_,depth in back)

header=(root/'src/util/ui_sort.h').read_text()
for name in ('ui_sort_original','ui_sort_adjacent'):
    body=bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})',header.split(name+'[] = {')[1].split('};')[0]))
    assert check(body),name
    if name=='ui_sort_adjacent':
        assert body==bytes.fromhex(json.loads((root/'tests/fixtures/ui_sort_gnome.json').read_text())['il'])
rejected=bytes.fromhex(json.loads((root/'tests/fixtures/ui_sort_rejected_83.json').read_text())['il'])
assert not check(rejected)
print('PASS: exact prior sort restored; empty back-edge stacks; rejected 0.0.83 IL fails compatibility gate')
