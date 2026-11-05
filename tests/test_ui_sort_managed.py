#!/usr/bin/env python3
"""Execute the original and patched BetterList sort from the supported APK."""
from pathlib import Path
import os,re,shutil,subprocess,tempfile,zipfile,json
root=Path(__file__).resolve().parents[1]
project=root/'tests/ui_sort'
dotnet=shutil.which('dotnet') or '/usr/local/share/dotnet/dotnet'
subprocess.run([dotnet,'build',str(project),'--configfile',str(project/'NuGet.Config'),
                '-p:UseSharedCompilation=false','--disable-build-servers','--verbosity','quiet'],check=True)
with tempfile.TemporaryDirectory() as d:
    p=Path(d);header=(root/'src/util/ui_sort.h').read_text()
    for name in ['ui_sort_original','ui_sort_adjacent']:
        block=header.split(name+'[] = {',1)[1].split('};',1)[0]
        (p/name).write_bytes(bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})',block)))
    with zipfile.ZipFile(os.environ.get('SRR_TEST_APK',root.parent/'srr224.apk')) as z:
        (p/'game.dll').write_bytes(z.read('assets/bin/Data/Managed/Assembly-CSharp.dll'))
    (p/'previous').write_bytes(bytes.fromhex(json.loads((root/'tests/fixtures/ui_sort_gnome.json').read_text())['il']))
    subprocess.run([dotnet,str(project/'bin/Debug/net10.0/ui_sort.dll'),str(p/'game.dll'),
                    str(p/'ui_sort_original'),str(p/'ui_sort_adjacent'),str(p/'previous')],check=True)
