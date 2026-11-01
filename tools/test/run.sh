#!/bin/sh
# Native (x86_64/any) headless test: generates a fake SD card, renders all screens to PNG.
# Needs gcc and ffmpeg (for test music), python3 + Pillow.
set -e
cd "$(dirname "$0")/../.."
sh tools/fetch_deps.sh
TP=third_party
mkdir -p build/test
gcc -O2 -D_GNU_SOURCE -w -I$TP/stb -I$TP/dr_libs \
	src/util.c src/gfx.c src/tags.c src/library.c src/audio.c src/covers.c src/ui.c tools/test/test_main.c \
	-o build/test/kasseta_test -lm -lpthread
[ -d build/test/sd ] || python3 tools/test/make_testsd.py build/test/sd
rm -rf build/test/data && mkdir -p build/test/data build/test/shots
OUT=build/test/shots ./build/test/kasseta_test build/test/sd build/test/data 1024 768 ""
