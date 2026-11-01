#!/bin/sh
# Downloads header-only libraries and aarch64 glibc headers into third_party/
set -e
cd "$(dirname "$0")/.."
mkdir -p third_party && cd third_party
[ -d stb ] || git clone -q --depth 1 https://github.com/nothings/stb stb
[ -d dr_libs ] || git clone -q --depth 1 https://github.com/mackron/dr_libs dr_libs
[ -d SDL2 ] || git clone -q --depth 1 -b SDL2 https://github.com/libsdl-org/SDL SDL2
if [ ! -d sysroot ]; then
	[ -d zig ] || { git clone -q --depth 1 --filter=blob:none --sparse https://github.com/ziglang/zig zig; \
		(cd zig && git sparse-checkout set lib/libc/include/generic-glibc lib/libc/include/aarch64-linux-gnu \
			lib/libc/include/aarch64-linux-any lib/libc/include/any-linux-any); }
	mkdir -p sysroot/usr/include
	for d in any-linux-any aarch64-linux-any generic-glibc aarch64-linux-gnu; do
		cp -r zig/lib/libc/include/$d/. sysroot/usr/include/
	done
fi
echo "deps ready"
