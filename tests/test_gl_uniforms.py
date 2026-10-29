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
harness += r'''
int main(void) {
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
  puts("PASS: array [0] alias, per-program locations, character shader selection, original mode");
}
'''
with tempfile.TemporaryDirectory() as d:
    p = Path(d)
    (p / 'test.c').write_text(harness)
    subprocess.run(['cc', '-fsanitize=address,undefined', str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
