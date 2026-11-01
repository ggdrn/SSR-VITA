#!/bin/sh
# Builds a new VPK. Every run bumps the build number in VERSION
# (0.0.01 -> 0.0.02 -> ...). --rebuild preserves it; --release builds the
# public package configured in cmake/ReleaseVersion.cmake without bumping it.
set -e
cd "$(dirname "$0")/.."

release_build=OFF
case "${1-}" in
  --release) release_build=ON ;;
  --rebuild|"") ;;
  *) echo "Usage: $0 [--rebuild|--release]" >&2; exit 2 ;;
esac

if [ "$#" -eq 0 ]; then
  n=$(($(cat VERSION) + 1))
  echo "$n" > VERSION
fi

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSRR_RELEASE_BUILD="$release_build"
if cmake --build build -j8 > build/last_build.log 2>&1; then
  cat build/last_build.log
else
  cat build/last_build.log
  # vita-elf-create sometimes needs extra text padding. All other failures
  # stop here, so a stale package can never be copied as a successful build.
  if ! grep -q "Cannot allocate .* SCE data at end of segment" build/last_build.log; then
    exit 1
  fi
  pad=$(sed -n 's/^SRR_TEXT_PAD:STRING=//p' build/CMakeCache.txt)
  pad=$(( ${pad:-0} == 0 ? 8192 : 0 ))
  echo "Retrying with SRR_TEXT_PAD=$pad"
  cmake -S . -B build -DSRR_TEXT_PAD=$pad
  cmake --build build -j8
fi

# Only the VPK of this build goes to build/VPK/ (older ones there are the
# user's to keep or delete); stale VPKs of earlier builds are removed from
# build/ so they can't be picked up again.
vpk=$(cat build/package-name.txt)
mkdir -p build/VPK
cp "build/$vpk" build/VPK/
find build -maxdepth 1 -name 'SRR-Vita-v*.vpk' ! -name "$vpk" -delete
echo "build/VPK/$vpk"
