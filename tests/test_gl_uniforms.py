#!/usr/bin/env python3
"""Check actual GLES location remapping and shader selection against the APK."""
from pathlib import Path
import json
import subprocess
import tempfile
import zipfile
root = Path(__file__).resolve().parents[1]
src = (root / 'src/bridges/gl_bridge.c').read_text()
harness = r'''
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
typedef int GLint;
typedef unsigned GLuint;
typedef unsigned GLenum;
typedef char GLchar;
#define GL_VERTEX_SHADER 0x8b31
#define GL_FRAGMENT_SHADER 0x8b30
#define L_INFO(...) ((void)0)
static GLint glGetUniformLocation(GLuint prog, const GLchar *name) {
  if (!strcmp(name, "unity_LightColor")) return 100;
  if (!strcmp(name, "_DifCol")) return 42;
  return -1;
}
'''
a=src.index('static int character_compat =')
b=src.index('void glShaderSource_bridge',a)
harness += src[a:b]
a=src.index('#define MAX_PROGRAMS')
b=src.index('static GLint real_location',a)
harness += src[a:b]
with zipfile.ZipFile(root.parent / 'srr224.apk') as z:
    assets = z.read('assets/bin/Data/sharedassets12.assets')
def shader(name):
    a=assets.index(('Shader "' + name + '"').encode())
    return assets[a:assets.index(b'\0',a)].decode()
harness += 'static const char original_character[] = ' + json.dumps(shader('Custom/ykChr_dme1')) + ';\n'
harness += 'static const char original_outline[] = ' + json.dumps(shader('Custom/ykOutLine')) + ';\n'
# Verify selection using separate GLES stages, as Unity actually submits them.
# The downloaded cache is optional; the APK character and outline are mandatory.
import re
shader_texts = [shader('Custom/ykChr_dme1'), shader('Custom/ykOutLine')]
cache = root.parent / 'data do jogo/files/UnityCache/Shared'
for path in cache.rglob('*.sharedAssets'):
    raw = path.read_bytes()
    for match in re.finditer(rb'Shader "([^"\0]+)"', raw):
        end = raw.index(b'\0', match.start())
        text = raw[match.start():end].decode()
        if text not in shader_texts:
            shader_texts.append(text)
checks = []
expected = {'Custom/ykChr_dme1': 'character_vertex',
            'Custom/ykChr_dmue1': 'scrolling_vertex',
            'Custom/ykCmn_dme1': 'common_vertex'}
for i, text in enumerate(shader_texts):
    name = re.search(r'Shader "([^"]+)"', text)[1]
    gles = re.search(r'"!!GLES\s+(.*?)#endif"', text, re.S)
    # Extract through the fragment #endif (the first ends the vertex).
    if not gles:
        continue
    start = gles.start()
    end = text.index('#endif', text.index('#ifdef FRAGMENT', start))
    body = text[start:end]
    vertex = body.split('#ifdef VERTEX',1)[1].split('#endif',1)[0]
    fragment = body.split('#ifdef FRAGMENT',1)[1]
    for stage, source in [('VERTEX', vertex), ('FRAGMENT', fragment)]:
        var = f'cache_{i}_{stage}'
        harness += 'static const char ' + var + '[] = ' + json.dumps(source) + ';\n'
        replacement = (expected[name] if stage == 'VERTEX' else 'character_fragment') if name in expected else var
        checks.append(f'material_shaders = 1; assert(character_shader_source(GL_{stage}_SHADER, {var}) == {replacement});')
        off = ('diffuse_vertex' if stage == 'VERTEX' else 'diffuse_fragment') if name in expected else var
        checks.append(f'material_shaders = 0; assert(character_shader_source(GL_{stage}_SHADER, {var}) == {off});')
harness += 'static void check_cached_stages(void) {\n' + '\n'.join(checks) + '\nmaterial_shaders = 1;\n}\n'
harness += r'''
int main(void) {
  check_cached_stages();
  assert(glGetUniformLocation_bridge(1, "unity_LightColor[0]") == 0);
  assert(glGetUniformLocation_bridge(1, "unity_LightColor") == 0);
  assert(glGetUniformLocation_bridge(1, "_DifCol") == 1);
  assert(glGetUniformLocation_bridge(2, "_DifCol") == 0);
  assert(glGetUniformLocation_bridge(1, "missing[0]") == -1);
  assert(character_shader_source(GL_VERTEX_SHADER, original_character) == character_vertex);
  assert(character_shader_source(GL_FRAGMENT_SHADER, original_character) == character_fragment);
  assert(character_shader_source(GL_VERTEX_SHADER, original_outline) == original_outline);
  character_compat = 0;
  assert(character_shader_source(GL_VERTEX_SHADER, original_character) == original_character);
  locations_clear(1); locations_clear(2);
  puts("PASS: array [0] alias, per-program locations, separate shader stages, cache variants and glass/falloff exclusions, original mode");
}
'''
with tempfile.TemporaryDirectory() as d:
    p = Path(d)
    (p / 'test.c').write_text(harness)
    subprocess.run(['cc', '-fsanitize=address,undefined', str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
