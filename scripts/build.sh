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
cmake --build build -j8

mkdir -p releases
cp build/SRR-Vita-v*.vpk releases/ 2>/dev/null || true
ls -1 build/*.vpk
