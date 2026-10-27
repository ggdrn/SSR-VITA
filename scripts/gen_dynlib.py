#!/usr/bin/env python3
"""Generates src/loader/dynlib.c: the import table used to link the APK's
native libraries against the Vita bridges.

Every symbol in data/apk_imports.txt is mapped, in order of preference, to:
  1. an explicit alias below,
  2. <name>_sfp or <name>_bridge defined somewhere in src/,
  3. the VitaSDK/vitaGL symbol of the same name (pass-through),
or left out when another APK library exports it (mono_*, libmain helpers).

Usage: scripts/gen_dynlib.py   (run from the repo root, needs $VITASDK)
"""
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VITASDK = os.environ.get("VITASDK", "/usr/local/vitasdk")

# Symbols whose bridge has a different name, or that need a neutral stub.
ALIASES = {
    "__errno": "__errno_bionic",
    "lstat": "stat_bridge",
    "geteuid": "getuid_bridge",
    "_exit": "exit_bridge",
    "putc": "fputc_bridge",
    "pthread_sigmask": "sigprocmask_bridge",
    "g_free": "free_bridge",
    # Defined in assembly (libc_bridge.c), invisible to the scan below.
    "sigsetjmp": "sigsetjmp_bridge",
    "siglongjmp": "siglongjmp_bridge",
    "__aeabi_atexit": "ret0_bridge",
    # libmain C++ helpers imported by libunity: ours replace them.
    "_Z28ANativeActivity_feedWatchdogv": "ANativeActivity_feedWatchdog_bridge",
    "_Z30ANativeActivity_enableWatchdogb": "ANativeActivity_enableWatchdog_bridge",
    "_Z29ANativeActivity_getInputQueuev": "ANativeActivity_getInputQueue_bridge",
    "_Z42ANativeActivity_setFirstLevelHasBeenLoadedb": "ANativeActivity_setFirstLevelHasBeenLoaded_bridge",
    "_Z42ANativeActivity_forwardMotionEventToDalvikP11AInputEventP7_JNIEnv":
        "ANativeActivity_forwardMotionEventToDalvik_bridge",
    # Processes, users and signals that have no Vita meaning.
    "setitimer": "ret0_bridge",
    "getpwnam": "ret0_bridge",
    "getpwuid": "ret0_bridge",
    "getgrgid": "ret0_bridge",
    "getgrnam": "ret0_bridge",
    "getresuid": "ret0_bridge",
    "setresuid": "ret0_bridge",
    "popen": "ret0_bridge",
    "gethostbyname": "ret0_bridge",
    "gethostbyaddr": "ret0_bridge",
    "freeaddrinfo": "ret0_bridge",
    "pclose": "fail_bridge",
    "fork": "fail_bridge",
    "execl": "fail_bridge",
    "execv": "fail_bridge",
    "execve": "fail_bridge",
    "waitpid": "fail_bridge",
    "system": "fail_bridge",
    "ptrace": "fail_bridge",
    "sendfile": "fail_bridge",
    "epoll_create": "fail_bridge",
    "epoll_ctl": "fail_bridge",
    "epoll_wait": "fail_bridge",
}

# Data symbols: the table holds their address.
DATA = {
    "__sF": "__sF_bionic",
    "_ctype_": "_ctype_bionic",
    "_tolower_tab_": "_tolower_tab_bionic",
    "_toupper_tab_": "_toupper_tab_bionic",
    "__stack_chk_guard": "__stack_chk_guard_bridge",
    "__page_size": "__page_size_bridge",
    "environ": "environ_bridge",
    "data_start": "data_start_bridge",
    "__data_start": "data_start_bridge",
    "SL_IID_ENGINE": "SL_IID_ENGINE_bridge",
    "SL_IID_PLAY": "SL_IID_PLAY_bridge",
    "SL_IID_BUFFERQUEUE": "SL_IID_BUFFERQUEUE_bridge",
    "SL_IID_VOLUME": "SL_IID_VOLUME_bridge",
    "_ZSt7nothrow": "_ZSt7nothrow",
    # libmain exports these as function-pointer *variables* (it dlsym()s them
    # from libandroid); libunity loads the pointer and calls through it.
    "_MotionEvent_getAxisValue": "_MotionEvent_getAxisValue_bridge",
    "_MotionEvent_getButtonState": "_MotionEvent_getButtonState_bridge",
    "_MotionEvent_getToolType": "_MotionEvent_getToolType_bridge",
}

# BSD sockets need constant/struct translation that is not written yet.
NETWORK = """accept bind connect getpeername getsockname getsockopt listen recv recvfrom
recvmsg send sendmsg sendto setsockopt shutdown socket select poll pipe getaddrinfo""".split()

# Provided by the APK's own libraries.
FROM_MODULES = re.compile(r"^(mono_|unity_mono_|GC_)")


def sdk_symbols():
    libs = [f"{VITASDK}/arm-vita-eabi/lib/{x}" for x in
            ("libc.a", "libm.a", "libstdc++.a", "libsupc++.a", "libz.a", "libpthread.a")]
    libs += glob.glob(f"{VITASDK}/lib/gcc/arm-vita-eabi/*/libgcc*.a")
    libs.append(os.path.join(ROOT, "..", "vitaGL", "libvitaGL.a"))
    syms = set()
    for lib in libs:
        out = subprocess.run([f"{VITASDK}/bin/arm-vita-eabi-nm", "-g", "--defined-only", lib],
                             capture_output=True, text=True).stdout
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 3:
                syms.add(parts[2])
    return syms


def bridge_symbols():
    found = set()
    pattern = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*_(?:bridge|sfp|bionic))\b\s*[\(\[=;]")
    # math_bridge.c defines most shims through D1(sin), F2(powf), ... macros.
    macro = re.compile(r"\b[DF][12]\((\w+)\)")
    for path in glob.glob(os.path.join(ROOT, "src", "**", "*.c"), recursive=True):
        text = open(path).read()
        found.update(pattern.findall(text))
        found.update(name + "_sfp" for name in macro.findall(text))
    return found


# Functions that pass or return float/double in registers: on the hard-float
# Vita they must go through a soft-float shim, never straight to newlib.
FLOAT_ABI = set("""acos acosf asin asinf atan atan2 atan2f atanf ceil ceilf cos cosf cosh exp exp2f expf floor
floorf fmod fmodf frexp ldexp log log10 log10f logf modf modff pow powf rint round sin sinf sinh sqrt
sqrtf tan tanf tanh trunc strtod isnan __fpclassifyf""".split())


def main():
    imports = open(os.path.join(ROOT, "data", "apk_imports.txt")).read().split()
    sdk = sdk_symbols()
    bridges = bridge_symbols()

    funcs, data, unmapped = [], [], []
    for name in sorted(set(imports)):
        if FROM_MODULES.match(name):
            continue
        if name in DATA:
            data.append((name, DATA[name]))
        elif name in ALIASES:
            funcs.append((name, ALIASES[name]))
        elif name + "_sfp" in bridges:
            funcs.append((name, name + "_sfp"))
        elif name + "_bridge" in bridges:
            funcs.append((name, name + "_bridge"))
        elif name in NETWORK:
            funcs.append((name, "net_unavailable_bridge"))
        elif name in sdk:
            funcs.append((name, name))
        else:
            unmapped.append(name)

    wrong = [n for n, t in funcs if n in FLOAT_ABI and not t.endswith("_sfp")]
    if wrong:
        sys.exit("soft-float shim missing for: " + " ".join(wrong))

    targets_f = sorted({t for _, t in funcs})
    targets_d = sorted({t for _, t in data})
    out = []
    out.append("// Generated by scripts/gen_dynlib.py - do not edit by hand.")
    out.append("// Maps every import of the APK's native libraries to a Vita implementation.\n")
    # No libc headers here: the dummy declarations below would clash with them.
    out.append('#include "so_util.h"\n')
    out.append("// Declared with dummy types: only the addresses matter here.")
    for t in targets_f:
        out.append(f"extern void {t}(void);")
    for t in targets_d:
        out.append(f"extern char {t}[];" if t not in ("_ZSt7nothrow",) else f"extern const char {t};")
    out.append("\nconst so_default_dynlib default_dynlib[] = {")
    for name, t in funcs:
        out.append(f'  {{"{name}", (uintptr_t)&{t}}},')
    for name, t in data:
        out.append(f'  {{"{name}", (uintptr_t)&{t}}},')
    out.append("};\n")
    out.append("const int default_dynlib_len = sizeof(default_dynlib) / sizeof(default_dynlib[0]);\n")
    out.append("static int same(const char *a, const char *b) {")
    out.append("  while (*a && *a == *b)")
    out.append("    a++, b++;")
    out.append("  return *a == *b;")
    out.append("}\n")
    out.append("uintptr_t dynlib_lookup(const char *name) {")
    out.append("  for (int i = 0; i < default_dynlib_len; i++) {")
    out.append("    if (same(default_dynlib[i].symbol, name))")
    out.append("      return default_dynlib[i].func;")
    out.append("  }")
    out.append("  return 0;")
    out.append("}")

    dst = os.path.join(ROOT, "src", "loader", "dynlib.c")
    open(dst, "w").write("\n".join(out) + "\n")
    print(f"{dst}: {len(funcs)} functions, {len(data)} data symbols")
    if unmapped:
        print("unmapped (will hit the logging stub at runtime):", " ".join(unmapped))


if __name__ == "__main__":
    sys.exit(main())
