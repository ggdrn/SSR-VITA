#!/bin/sh
set -eu
srr_project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
srr_profile="$srr_project/build/vita3k-srr-clean"
cd "$srr_profile"
exec /Applications/Vita3K.app/Contents/MacOS/Vita3K \
  -c "$srr_profile/config.yml" -f -w \
  --log-active-shaders=false --log-uniforms=false \
  --frontend sdl -r SRRV00001
