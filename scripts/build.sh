#!/bin/sh
# Builds a new VPK. Every run bumps the build number in VERSION
# (0.0.01 -> 0.0.02 -> ...); pass --rebuild to rebuild the current number.
set -e
cd "$(dirname "$0")/.."

if [ "$1" != "--rebuild" ]; then
  n=$(($(cat VERSION) + 1))
  echo "$n" > VERSION
fi

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
if ! cmake --build build -j8 2>&1 | tee build/last_build.log; then
  exit 1
fi
# vita-elf-create fails when text ends within ~4 KB of the data segment; pad
# the text segment and retry (see src/text_pad.c).
if grep -q "Cannot allocate .* SCE data at end of segment" build/last_build.log; then
  pad=$(sed -n 's/^SRR_TEXT_PAD:STRING=//p' build/CMakeCache.txt)
  pad=$(( ${pad:-0} == 0 ? 8192 : 0 ))
  echo "Retrying with SRR_TEXT_PAD=$pad"
  cmake -S . -B build -DSRR_TEXT_PAD=$pad
  cmake --build build -j8
fi

# Only the VPK of this build goes to build/VPK/ (older ones there are the
# user's to keep or delete); stale VPKs of earlier builds are removed from
# build/ so they can't be picked up again.
n=$(cat VERSION)
vpk="SRR-Vita-v0.0.$(printf '%02d' "$n").vpk"
mkdir -p build/VPK
cp "build/$vpk" build/VPK/
find build -maxdepth 1 -name 'SRR-Vita-v*.vpk' ! -name "$vpk" -delete
echo "build/VPK/$vpk"
