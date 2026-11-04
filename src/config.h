#ifndef SRR_CONFIG_H
#define SRR_CONFIG_H

// Game folder on the Vita. Only the original APK lives here; everything the
// loader needs is pulled out of it in memory at boot.
#define DATA_PATH        "ux0:data/srr"
#define LOG_PATH         DATA_PATH "/srr_log.txt"
// Writable folders created at boot (Unity save data / caches).
#define FILES_PATH       DATA_PATH "/files"
#define CACHE_PATH       DATA_PATH "/cache"

#define PACKAGE_NAME     "com.sonicrunners.revival"

// Virtual Android paths handed to the game. fs_bridge maps them back to ux0.
#define ANDROID_APK_DIR  "/vita"
#define ANDROID_DATA_DIR "/data/data/" PACKAGE_NAME
#define ANDROID_SDCARD   "/sdcard"

// Full Vita screen; the port settings can render smaller (SD/HD), which the
// display scaler stretches to the screen.
#define SCREEN_MAX_W 960
#define SCREEN_MAX_H 544
extern int port_screen_w, port_screen_h;
#define SCREEN_W port_screen_w
#define SCREEN_H port_screen_h

// Native libraries loaded from lib/armeabi-v7a/ inside the APK, in load order.
#define LIB_DIR "lib/armeabi-v7a/"

// Memory budget (with ATTRIBUTE2=12 extended memory in param.sfo).
#define NEWLIB_HEAP_SIZE (220 * 1024 * 1024)
// RAM vitaGL leaves untouched for everything allocated after it: Mono JIT code
// blocks (kubridge), thread stacks, audio ports.
#define VGL_RAM_THRESHOLD (48 * 1024 * 1024)

#endif
