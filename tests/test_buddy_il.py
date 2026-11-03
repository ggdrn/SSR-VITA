#!/usr/bin/env python3
"""Prove native idle policy reaches return without writes/calls in original APK IL."""
from pathlib import Path
import base64,itertools,json,math,os,re,shutil,struct,subprocess,tempfile,zipfile
root=Path(__file__).resolve().parents[1]
project=root/'tests/buddy_il'
dotnet=shutil.which('dotnet') or '/usr/local/share/dotnet/dotnet'
subprocess.run([dotnet,'build',str(project),'--configfile',str(project/'NuGet.Config'),
                '-p:UseSharedCompilation=false','--disable-build-servers','--verbosity','quiet'],check=True)
apk=Path(os.environ.get('SRR_TEST_APK',str(root.parent/'srr224.apk')))
with tempfile.TemporaryDirectory() as d:
    p=Path(d)/'Assembly-CSharp.dll'
    with zipfile.ZipFile(apk) as z:p.write_bytes(z.read('assets/bin/Data/Managed/Assembly-CSharp.dll'))
    data=json.loads(subprocess.check_output([dotnet,str(project/'bin/Debug/net10.0/buddy_il.dll'),str(p)],text=True))
il=base64.b64decode(data['il'])
header=(root/'src/util/buddy_cell_il.h').read_text()
expected=bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})',header.split('buddy_cell_il[] = {',1)[1]))
assert il==expected and len(il)==968
assert data['mvid'].upper() in header
fields={int(k):v for k,v in data['fields'].items()}
# Follow actual branch destinations in the original method. A call, write,
# allocation or unknown instruction disqualifies the original no-op path.
def original_returns_without_effects(state):
    stack=[];pc=0
    for _ in range(100):
        op=il[pc];pc+=1
        if op==0x02:stack.append('self') # ldarg.0
        elif op in (0x7b,0x7e): # ldfld / ldsfld
            tok=struct.unpack_from('<I',il,pc)[0];pc+=4
            if op==0x7b:
                assert stack.pop()=='self'
            if fields[tok] not in state:return False
            stack.append(state[fields[tok]])
        elif op==0x16:stack.append(0)
        elif op==0x22:
            stack.append(struct.unpack_from('<f',il,pc)[0]);pc+=4
        elif op in (0x38,0x39,0x3a,0x3d,0x3f,0x43):
            jump=struct.unpack_from('<i',il,pc)[0];pc+=4
            if op==0x38:take=True
            elif op in (0x39,0x3a):take=bool(stack.pop())==(op==0x3a)
            else:
                right,left=stack.pop(),stack.pop()
                take=left>right if op==0x3d else left<right if op==0x3f else not(left>right)
            if take:pc+=jump
        elif op==0x2a:return not stack # ret
        else:return False
    raise AssertionError('loop in idle path')
times=[-math.inf,-1.0,-0.0,0.0,0.00001,1.0,math.inf,math.nan]
checked=0
for loaded,drawn,draw,check,lock,ident in itertools.product((0,1),(0,1),times,times,(0,1),(-1,0,1234)):
    idle=loaded and drawn and not(draw>0) and not(check>0)
    state=dict(m_isLoadCmp=loaded,m_isDrawChao=drawn,m_drawDelay=draw,m_checkDelay=check,s_loadLock=lock,m_chaoId=ident)
    if idle:
        assert original_returns_without_effects(state),(state,il.hex())
        checked+=1
# Active texture loading and positive validation/draw timers cannot be bypassed.
assert not original_returns_without_effects(dict(m_isLoadCmp=0,m_isDrawChao=1,m_drawDelay=-1,m_checkDelay=0,s_loadLock=0,m_chaoId=1))
print(f'PASS: exact original APK IL/MVID and {checked} idle states (NaNs, load locks and IDs) reach return without a call or write')
