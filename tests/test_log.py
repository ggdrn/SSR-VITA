#!/usr/bin/env python3
"""Run the production logger with host I/O stubs; no Vita is needed."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'src/util/log.c').read_text()
start = source.index('void log_vprintf(')
end = source.index('void fatal_error(', start)
harness = r'''
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>
static int log_fd = 1, log_lock, writes;
static char captured[65536];
static void sceKernelLockLwMutex(int *p, int n, void *t) {}
static void sceKernelUnlockLwMutex(int *p, int n) {}
static void sceClibPrintf(const char *fmt, ...) {}
static void sceIoWrite(int fd, const char *s, int n) {
  assert(strlen(captured) + n < sizeof(captured));
  strncat(captured, s, n);
  writes++;
}
'''
harness += source[start:end]
harness += r'''
int main(void) {
  log_printf("");
  assert(strcmp(captured, "\n") == 0);
  for (int i = 0; i < 10000; ++i) log_printf("repeated exception");
  log_printf("next distinct message");
  assert(writes < 20);
  assert(strstr(captured, "previous message total repeats 9999\n"));
  assert(strstr(captured, "next distinct message\n"));
  char large[4096]; memset(large, 'x', sizeof(large)-1); large[4095] = 0;
  log_printf("%s", large);
  assert(captured[strlen(captured)-1] == '\n');
  puts("PASS: empty message, bounded duplicate writes, summary, truncation");
}
'''
with tempfile.TemporaryDirectory() as d:
    p = Path(d)
    (p / 'test.c').write_text(harness)
    subprocess.run(['cc', '-fsanitize=address,undefined', str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
