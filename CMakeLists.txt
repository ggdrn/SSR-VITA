cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(srr_vita C ASM)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

# --- Versioning -------------------------------------------------------------
# VERSION holds the build counter N. The VPK is "0.0.NN"; param.sfo only takes
# "XX.YY", so the Vita shows it as "00.NN".
file(READ "${CMAKE_SOURCE_DIR}/VERSION" SRR_BUILD)
string(STRIP "${SRR_BUILD}" SRR_BUILD)
if(SRR_BUILD LESS 10)
  set(SRR_BUILD_2 "0${SRR_BUILD}")
else()
  set(SRR_BUILD_2 "${SRR_BUILD}")
endif()
set(SRR_VERSION "0.0.${SRR_BUILD_2}")
set(SRR_SFO_VERSION "00.${SRR_BUILD_2}")
option(SRR_RELEASE_BUILD "Build the public release package" OFF)
if(SRR_RELEASE_BUILD)
  include("${CMAKE_SOURCE_DIR}/cmake/ReleaseVersion.cmake")
  set(SRR_VERSION "${SRR_RELEASE_VERSION}")
  set(SRR_SFO_VERSION "${SRR_RELEASE_APP_VERSION}")
endif()
string(TIMESTAMP SRR_BUILD_STAMP "%Y%m%dT%H%M%SZ" UTC)
configure_file(src/version.h.in ${CMAKE_BINARY_DIR}/generated/version.h @ONLY)

set(VITA_APP_NAME "Sonic Runners Revival")
set(VITA_TITLEID  "SRRV00001")
# Extended memory mode (ATTRIBUTE2=12) gives the game ~365 MB.
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

# Local checkouts next to this repo (see README).
set(LOCAL_VITAGL "${CMAKE_SOURCE_DIR}/../vitaGL")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -O2 -g -mcpu=cortex-a9 -mfpu=neon -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable")

# mbedTLS (third_party/mbedtls, 3.6 LTS) for HTTPS to the Revival servers.
file(GLOB MBEDTLS_SOURCES ${CMAKE_SOURCE_DIR}/third_party/mbedtls/library/*.c)
add_library(mbedtls_vita STATIC ${MBEDTLS_SOURCES})
target_include_directories(mbedtls_vita PUBLIC
  ${CMAKE_SOURCE_DIR}/third_party/mbedtls/include
  ${CMAKE_SOURCE_DIR}/src/net)
target_compile_definitions(mbedtls_vita PUBLIC MBEDTLS_USER_CONFIG_FILE="mbedtls_user_config.h")
target_compile_options(mbedtls_vita PRIVATE -w)

add_executable(${PROJECT_NAME}
  src/main.c
  src/unity_boot.c
  src/patches.c
  src/ui_mesh.c
  src/mono_probe.c
  src/net/http.c
  src/net/ca_roots.c
  src/util/observe.c
  src/util/native_heap.c
  src/util/work_trace.c
  src/util/managed_trace.c
  src/util/ui_geometry.c
  src/util/title_layout.c
  src/util/splash_art.c
  src/util/gc_trace.c
  src/util/gc_policy.c
  src/util/bundle_mips.c
  src/util/log.c
  src/util/apk.c
  src/loader/so_util.c
  src/loader/dynlib.c
  src/bridges/libc_bridge.c
  src/bridges/pthread_bridge.c
  src/bridges/math_bridge.c
  src/bridges/mem_bridge.c
  src/bridges/signal_bridge.c
  src/bridges/fs_bridge.c
  src/bridges/proc_bridge.c
  src/bridges/gl_bridge.c
  src/bridges/android_bridge.c
  src/bridges/input_bridge.c
  src/bridges/audio_bridge.c
  src/bridges/misc_bridge.c
  src/bridges/format_bridge.c
  src/bridges/watchdog.c
  src/bridges/ime_bridge.c
  src/jni/jni.c
  src/jni/java_impl.c
  src/text_pad.c
)

set(SRR_TEXT_PAD 0 CACHE STRING "Bytes of padding in the text segment (see src/text_pad.c)")
set_source_files_properties(src/text_pad.c PROPERTIES COMPILE_DEFINITIONS "SRR_TEXT_PAD=${SRR_TEXT_PAD}")

# dynlib.c declares every import with a dummy prototype on purpose.
set_source_files_properties(src/loader/dynlib.c PROPERTIES COMPILE_FLAGS
  "-fno-builtin -Wno-builtin-declaration-mismatch")

target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_BINARY_DIR}/generated
  ${LOCAL_VITAGL}/source
)

target_link_libraries(${PROJECT_NAME}
  mbedtls_vita
  SceNet_stub
  SceNetCtl_stub
  ${LOCAL_VITAGL}/libvitaGL.a
  vitashark
  SceShaccCgExt
  mathneon
  kubridge_stub
  taihen_stub
  SceShaccCg_stub
  SceKernelDmacMgr_stub
  SceGxm_stub
  SceDisplay_stub
  SceCommonDialog_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceVshBridge_stub
  SceSysmodule_stub
  SceCtrl_stub
  SceTouch_stub
  SceAudio_stub
  SceRtc_stub
  ScePower_stub
  SceLibKernel_stub
  SceIofilemgr_stub
  pthread
  z
  stdc++
  m
  c
)

vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE)
# Packaging is done by hand instead of vita_create_vpk(): that helper doesn't
# quote FILE paths, and this repo lives under a folder with spaces.
set(VPK_NAME "SRR-Vita-v${SRR_VERSION}.vpk")
file(WRITE "${CMAKE_BINARY_DIR}/package-name.txt" "${VPK_NAME}\n")
set(SCE_SYS "${CMAKE_SOURCE_DIR}/sce_sys")
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/param.sfo
  COMMAND $ENV{VITASDK}/bin/vita-mksfoex -d ATTRIBUTE2=12 -s APP_VER=${SRR_SFO_VERSION}
          -s TITLE_ID=${VITA_TITLEID} ${VITA_APP_NAME} ${CMAKE_BINARY_DIR}/param.sfo
  DEPENDS ${CMAKE_SOURCE_DIR}/VERSION ${CMAKE_SOURCE_DIR}/cmake/ReleaseVersion.cmake
          ${CMAKE_BINARY_DIR}/generated/version.h
  VERBATIM)
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/${VPK_NAME}
  COMMAND $ENV{VITASDK}/bin/vita-pack-vpk
          -s ${CMAKE_BINARY_DIR}/param.sfo
          -b ${CMAKE_BINARY_DIR}/eboot.bin
          -a ${SCE_SYS}/icon0.png=sce_sys/icon0.png
          -a ${SCE_SYS}/pic0.png=sce_sys/pic0.png
          -a ${CMAKE_SOURCE_DIR}/assets/game-splash.rgb=assets/game-splash.rgb
          -a ${SCE_SYS}/livearea/contents/bg.png=sce_sys/livearea/contents/bg.png
          -a ${SCE_SYS}/livearea/contents/startup.png=sce_sys/livearea/contents/startup.png
          -a ${SCE_SYS}/livearea/contents/template.xml=sce_sys/livearea/contents/template.xml
          ${CMAKE_BINARY_DIR}/${VPK_NAME}
  DEPENDS eboot.bin ${CMAKE_BINARY_DIR}/param.sfo
          ${SCE_SYS}/icon0.png
          ${SCE_SYS}/pic0.png
          ${CMAKE_SOURCE_DIR}/assets/game-splash.rgb
          ${SCE_SYS}/livearea/contents/bg.png
          ${SCE_SYS}/livearea/contents/startup.png
          ${SCE_SYS}/livearea/contents/template.xml
  VERBATIM)
add_custom_target(vpk ALL DEPENDS ${CMAKE_BINARY_DIR}/${VPK_NAME})
add_dependencies(vpk eboot.bin-self)
