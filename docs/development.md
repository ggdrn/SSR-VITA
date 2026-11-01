# Development guide

The port loads the APK's ARM native libraries directly and provides the Android interfaces they need through VitaSDK, vitaGL, and compatibility bridges. It does not rebuild the game from its original source code.

## Supported APK and data layout

The APK and the full game data are not included. The supported APK used for development has SHA-256:

```text
50b91eaa208d46b21289a967b1d8eb71fee5a6c5861c62f732cd1447ddcf10d0
```

The loader can locate an APK with another filename in the same directory, but the managed-code optimizations target the supported build. Compatibility checks keep the original code when a guarded patch does not match.

No OBB extraction is required for this APK. The loader extracts the native libraries into memory, and Unity reads its packaged data from the APK.

```text
ux0:data/srr/
├── srr224.apk
├── srr_log.txt          # Runtime diagnostics
├── files/              # Downloaded content and game data
│   └── shared_prefs/    # PlayerPrefs / local save preferences
├── cache/
└── sdcard/
```

## Build

The project uses **VitaSDK**, CMake, and a locally built **vitaGL** checkout. Set `VITASDK` to your SDK directory and ensure its tools are available in your shell.

Required link dependencies include vitaGL, vitaShaRK, SceShaccCgExt, mathneon, kubridge, taiHEN, pthread, and zlib. mbedTLS is vendored in `third_party/mbedtls`.

CMake expects vitaGL next to this repository:

```text
workspace/
├── SSR-VITA/           # This repository; the directory name may differ
└── vitaGL/
    ├── source/
    └── libvitaGL.a
```

Prepare vitaGL with the compatible changes in [patches/](../patches/), including shader compilation failure handling, shader caching, and texture allocation failure handling. The development checkout was based on vitaGL commit `76f43e6`; applying these patches to another revision may require adjustments. Build that checkout with:

```sh
make -C ../vitaGL HAVE_SHARK_LOG=1 HAVE_SHADER_CACHE=1
```

To build the public release from the repository root:

```sh
./scripts/build.sh --release
```

Release versions are configured in `cmake/ReleaseVersion.cmake`. Release `0.1` produces
`build/VPK/SRR-Vita-v0.01.00.vpk`, with Vita `APP_VER=00.10`. The internal development
counter in `VERSION` is preserved. Plain CMake builds can select this mode with
`-DSRR_RELEASE_BUILD=ON`.

To rebuild the current development package instead:

```sh
./scripts/build.sh --rebuild
```

This preserves `VERSION` and produces the current package in:

```text
build/VPK/SRR-Vita-v0.0.64.vpk
```

To increment the build number and create a new version:

```sh
./scripts/build.sh
```

The Vita application version is displayed as `00.NN`, while the project uses `0.0.NN`. Build outputs, APKs, VPKs, runtime logs, and crash dumps are ignored by Git.

When changing the import table, regenerate it with:

```sh
python3 scripts/gen_dynlib.py
```

## Development and validation

Host tests compile selected production code with a C compiler and AddressSanitizer/UndefinedBehaviorSanitizer. Examples:

```sh
python3 tests/test_http_send.py
python3 tests/test_cache_timing.py
python3 tests/test_metadata_timing.py
python3 tests/test_pthread_lifetime.py
python3 tests/test_ui_geometry_wrapper.py
```

Tests that compare against game assets expect the supported APK at `../srr224.apk`. Some tests and generators also need **.NET 10**, **Pillow**, or **UnityPy**. For example, the managed regression test executes the original and patched UI array-copy method from the APK:

```sh
python3 tests/test_ui_trim_managed.py
```

Host tests validate behavior and data integrity. Frame rate, visuals, sound, and long-session stability must also be checked on real hardware.

### Performance logs

The application writes `ux0:data/srr/srr_log.txt`. To generate a report from a copied log:

```sh
python3 scripts/report_observation.py /path/to/srr_log.txt --out-dir build/report
```

The report includes observed scenes, frame windows, asset operations, and capture-health counters. Loading states can include user interaction, and nested timings must not be added as if they were independent waits.

Version-by-version investigations are available in [docs/](.). These historical diagnostics are primarily in Portuguese. See [the 0.0.64 investigation](diagnostico-0.0.64.md) for the current startup and networking findings.

## Architecture

| Component | Implementation |
|---|---|
| APK / ELF loading | `src/util/apk.c`, `src/loader/so_util.c` |
| Unity player lifecycle | `src/unity_boot.c` |
| EGL / GLES2 | `src/bridges/gl_bridge.c`, backed by vitaGL |
| Input | `src/bridges/input_bridge.c` |
| OpenSL ES / Java audio | `src/bridges/audio_bridge.c` |
| Bionic threads and synchronization | `src/bridges/pthread_bridge.c` |
| Cooperative signals and CPU exceptions | `src/bridges/signal_bridge.c` |
| JIT memory | `src/bridges/mem_bridge.c`, backed by kubridge |
| Android paths and libc | `src/bridges/fs_bridge.c`, `src/bridges/libc_bridge.c` |
| JNI / Java compatibility | `src/jni/` |
| HTTP/HTTPS | `src/net/http.c`, using sceNet and mbedTLS |
| Managed-code diagnostics and guarded patches | `src/mono_probe.c`, `src/util/` |

The supported game uses **Unity 4.7.2f1 with Mono**. The port provides a GLES2 renderer path, cooperative cross-thread signal handling for Mono's garbage collector, and HTTPS certificate verification using embedded CA roots. CPU affinity probes whether the system core is available and falls back to the normal user cores when necessary.

Recent changes include bounded native copies for UI geometry, runtime array copies for UI buffer trimming, conservative UI mip-read skipping, JIT preparation, corrected thread-record lifetimes, and improved HTTP request transport. Some changes remain experimental and are documented with their validation limits.

## LiveArea and splash artwork

The circular icon and layered startup logo are derived from the supported APK. The supplied fan artwork is used for the LiveArea background and both splash paths, with proportional fitting. Generated resources and provenance are stored in `assets/` and `sce_sys/`.

To regenerate these resources, install Pillow and UnityPy and run:

```sh
python3 scripts/make_livearea.py ../srr224.apk
```

The in-game splash is applied through a guarded read-time overlay; the APK on storage remains unchanged.

## License and credits

The loader code is provided under the project's MIT licensing terms. Vendored mbedTLS has its own [license](../third_party/mbedtls/LICENSE). External libraries retain their respective licenses.

Game code, trademarks, and artwork belong to their respective owners. The loader's code license does not grant rights to those materials. This repository contains packaged presentation artwork, but does not distribute the game APK or the complete game data.

Thanks to the Sonic Runners Revival fan development team, VitaSDK, vitaGL, kubridge, and the PS Vita homebrew community.
