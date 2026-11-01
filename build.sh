#!/bin/sh
# Builds Kasseta.pak for NextUI tg5040 (TrimUI Brick / Brick Pro / Smart Pro).
# Needs: clang, lld, llvm-nm, python3, git, zip.  No ARM toolchain or Docker required.
set -e
cd "$(dirname "$0")"
sh tools/fetch_deps.sh
TP=third_party
mkdir -p build
python3 toolchain/xbuild.py build/kasseta.elf src/*.c \
	-I $TP/stb -I $TP/dr_libs -I $TP/SDL2/include -w
PAK=dist/Tools/tg5040/Kasseta.pak
rm -rf dist && mkdir -p $PAK/res
cp build/kasseta.elf pak/launch.sh pak/skip.txt pak/README.txt $PAK/
cp res/* $PAK/res/
chmod +x $PAK/launch.sh $PAK/kasseta.elf
(cd dist && rm -f ../Kasseta-NextUI-tg5040.zip && zip -qr ../Kasseta-NextUI-tg5040.zip Tools)
echo "OK: Kasseta-NextUI-tg5040.zip"
