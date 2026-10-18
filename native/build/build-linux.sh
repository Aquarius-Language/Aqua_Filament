#!/usr/bin/env bash
# Builds runtimes/linux/libaqua_filament.so from the official Filament 1.78.0 Linux release.
# Requirements: clang 16+, lld (optional), libX11 headers, and libc++ 18 (headers + static libs)
#   FILAMENT_LINUX=/path/to/filament-v1.78.0-linux/filament   (release "filament" folder)
#   LIBCXX=/path/to/llvm (contains include/c++/v1, include/x86_64-unknown-linux-gnu/c++/v1,
#                         lib/x86_64-unknown-linux-gnu/libc++.a, libc++abi.a)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
: "${FILAMENT_LINUX:?set FILAMENT_LINUX to the extracted filament-v1.78.0-linux/filament folder}"
: "${LIBCXX:?set LIBCXX to an LLVM 18 folder with libc++ headers and static libraries}"
out="$root/runtimes/linux"; obj="$here/obj-linux"; mkdir -p "$out" "$obj"
CXX="${CXX:-clang++}"; CC="${CC:-clang}"
inc=(-I"$FILAMENT_LINUX/include" -nostdinc++ -isystem "$LIBCXX/include/x86_64-unknown-linux-gnu/c++/v1" -isystem "$LIBCXX/include/c++/v1")
flags=(-std=c++20 -O2 -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -stdlib=libc++ -DNDEBUG -Wno-deprecated-declarations)
for f in aqua_filament window_x11; do
  "$CXX" "${flags[@]}" "${inc[@]}" -c "$root/native/src/$f.cpp" -o "$obj/$f.o"
done
"$CC" -O2 -fPIC -c "$root/native/src/compat_glibc.c" -o "$obj/compat_glibc.o"
L="$FILAMENT_LINUX/lib/x86_64"
libs=(gltfio_core filameshio camutils ktxreader filament backend bluegl bluevk filabridge filaflat geometry
      image ibl-lite utils basis_transcoder dracodec meshoptimizer zstd uberzlib uberarchive smol-v mikktspace
      stb abseil perfetto webgpu_dawn)
args=()
for l in "${libs[@]}"; do [ -f "$L/lib$l.a" ] && args+=("$L/lib$l.a"); done
"$CXX" -shared -fuse-ld=lld -o "$out/libaqua_filament.so" "$obj"/*.o \
  -Wl,--start-group "${args[@]}" -Wl,--end-group \
  "$LIBCXX/lib/x86_64-unknown-linux-gnu/libc++.a" "$LIBCXX/lib/x86_64-unknown-linux-gnu/libc++abi.a" \
  -nostdlib++ -static-libgcc -Wl,--exclude-libs,ALL -Wl,--gc-sections -Wl,-z,defs -Wl,--version-script="$here/exports.map" \
  -lX11 -lpthread -ldl -lm
strip --strip-unneeded "$out/libaqua_filament.so"
echo "Built $out/libaqua_filament.so ($(du -h "$out/libaqua_filament.so" | cut -f1))"
objdump -T "$out/libaqua_filament.so" | grep -o "GLIBC_[0-9.]*" | sort -Vu | tail -1 | sed 's/^/Highest glibc symbol version: /'
