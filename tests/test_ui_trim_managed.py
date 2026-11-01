#!/usr/bin/env python3
"""Execute original and patched APK IL in a managed runtime; no packages required."""
from pathlib import Path
import re,shutil,subprocess,tempfile,zipfile
root=Path(__file__).resolve().parents[1]
dotnet=shutil.which('dotnet') or '/usr/local/share/dotnet/dotnet'
project=root/'tests/ui_trim'
subprocess.run([dotnet,'build',str(project),'--configfile',str(project/'NuGet.Config'),
                '-p:UseSharedCompilation=false','--disable-build-servers','--verbosity','quiet'],check=True)
with tempfile.TemporaryDirectory() as d:
    p=Path(d);header=(root/'src/util/ui_trim.h').read_text()
    for field,out in [('ui_trim_original','original.il'),('ui_trim_copy','copy.il')]:
        a=header.index('=',header.index(field));b=header.index('};',a)
        data=bytes(int(x,16) for x in re.findall('0x([0-9a-f]{2})',header[a:b]))
        if field=='ui_trim_copy':data+=bytes(42-len(data))
        (p/out).write_bytes(data)
    with zipfile.ZipFile(root.parent/'srr224.apk') as z:
        (p/'Assembly-CSharp.dll').write_bytes(z.read('assets/bin/Data/Managed/Assembly-CSharp.dll'))
    subprocess.run([dotnet,str(project/'bin/Debug/net10.0/trim.dll'),str(p/'Assembly-CSharp.dll'),
                    str(p/'original.il'),str(p/'copy.il')],check=True)
