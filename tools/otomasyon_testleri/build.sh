#!/bin/sh
# Uçtan uca düzeneği build/ altındaki güncel statik kütüphanelerle derler.
# Önce: cmake --build build (radios_core, radios_playout, radios_audio).
# RADIOS_BUILD_DIR=build-xx ile başka bir yapı dizini kullanılır (çıktı da oraya gider).
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd -P)
BUILD="${RADIOS_BUILD_DIR:-build}"
OUT="$ROOT/$BUILD/otomasyon_testleri"
mkdir -p "$OUT"
PKGS="json-glib-1.0 gio-2.0 libavformat libavcodec libavutil libswresample libavfilter libswscale libpulse fontconfig libcurl"
cd "$ROOT"
cc -std=c11 -O1 -g -Iinclude -I"$BUILD/generated" $(pkg-config --cflags $PKGS) "$HERE/e2e.c" \
  -Wl,--start-group "$BUILD/libradios_playout.a" "$BUILD/libradios_core.a" "$BUILD/libradios_audio.a" -Wl,--end-group \
  $(pkg-config --libs $PKGS) -lm -o "$OUT/e2e"
echo "$OUT/e2e"
