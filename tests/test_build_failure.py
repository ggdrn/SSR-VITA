#!/usr/bin/env python3
"""A compiler failure must never publish an earlier VPK as a fresh build."""
from pathlib import Path
import tempfile,subprocess,os
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'scripts').mkdir();(p/'build/VPK').mkdir(parents=True);(p/'bin').mkdir()
 (p/'scripts/build.sh').write_text((root/'scripts/build.sh').read_text());(p/'VERSION').write_text('48\n')
 (p/'build/SRR-Vita-v0.0.48.vpk').write_text('STALE')
 cm=p/'bin/cmake';cm.write_text('#!/bin/sh\nif [ "$1" = "--build" ]; then echo "compile error"; exit 7; fi\nexit 0\n');cm.chmod(0o755)
 env=dict(os.environ);env['PATH']=str(p/'bin')+':'+env['PATH']
 r=subprocess.run(['sh',str(p/'scripts/build.sh'),'--rebuild'],env=env,capture_output=True,text=True)
 assert r.returncode!=0 and not (p/'build/VPK/SRR-Vita-v0.0.48.vpk').exists()
print('PASS: compiler failure stops packaging; stale VPK is never published')
