#!/usr/bin/env python3
"""Execute original/patched APK panel IL with instrumented engine boundaries."""
from pathlib import Path
import re,shutil,subprocess,tempfile,zipfile
root=Path(__file__).resolve().parents[1]
project=root/'tests/ui_panel'
dotnet=shutil.which('dotnet') or '/usr/local/share/dotnet/dotnet'
subprocess.run([dotnet,'build',str(project),'--configfile',str(project/'NuGet.Config'),'-p:UseSharedCompilation=false','--disable-build-servers','--verbosity','quiet'],check=True)
with tempfile.TemporaryDirectory() as d:
 p=Path(d);header=(root/'src/util/ui_panel_il.h').read_text()
 for name in ['ui_panel_original','ui_panel_owned']:
  block=header.split(name+'[] = {')[1].split('};')[0]
  (p/name).write_bytes(bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})',block)))
 with zipfile.ZipFile(root.parent/'srr224.apk') as z:(p/'game.dll').write_bytes(z.read('assets/bin/Data/Managed/Assembly-CSharp.dll'))
 subprocess.run([dotnet,str(project/'bin/Debug/net10.0/ui_panel.dll'),str(p/'game.dll'),str(p/'ui_panel_original'),str(p/'ui_panel_owned')],check=True)
