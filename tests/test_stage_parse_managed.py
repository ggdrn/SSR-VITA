#!/usr/bin/env python3
"""Run the actual original and batched APK parsing IL with XML/Unity fixtures."""
from pathlib import Path
import os,re,shutil,subprocess,tempfile,zipfile
root=Path(__file__).resolve().parents[1];project=root/'tests/stage_parse'
dotnet=shutil.which('dotnet') or '/usr/local/share/dotnet/dotnet'
subprocess.run([dotnet,'build',str(project),'--configfile',str(project/'NuGet.Config'),
                '-p:UseSharedCompilation=false','--disable-build-servers','--verbosity','quiet'],check=True)
with tempfile.TemporaryDirectory() as d:
    p=Path(d)
    with zipfile.ZipFile(os.environ.get('SRR_TEST_APK',root.parent/'srr224.apk')) as z:
        (p/'game.dll').write_bytes(z.read('assets/bin/Data/Managed/Assembly-CSharp.dll'))
    header=(root/'src/util/stage_parse_il.h').read_text()
    for name in ['stage_objects_il','stage_layer_il','stage_layer_batched_il']:
        body=header.split(name+'[] = {',1)[1].split('};',1)[0]
        (p/name).write_bytes(bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})',body)))
    dll=project/'bin/Debug/net10.0/Assembly-CSharp.dll'
    subprocess.run([dotnet,str(dll),str(p/'game.dll'),str(p/'stage_layer_batched_il'),str(p/'stage_objects_il'),str(p/'stage_layer_il')],check=True)
    # The maintained assembler must reproduce the checked-in replacement.
    subprocess.run([dotnet,str(dll),str(p/'game.dll'),'--export',str(p/'original')],check=True)
    subprocess.run(['python3',str(root/'scripts/gen_stage_parse.py'),str(p/'original/ReadObjects.il'),str(p/'original/ReadLayer.il'),str(p/'generated.h')],check=True)
    assert (p/'generated.h').read_text()==header
