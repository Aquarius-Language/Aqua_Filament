#!/usr/bin/env bash
# Cross-builds runtimes/windows/aqua_filament.dll on Linux with clang-cl + lld-link.
#   FILAMENT_WINDOWS=/path/to/filament-v1.78.0-windows     (official release, uses lib/x86_64/md)
#   MSVC=/path/to/VC/Tools/MSVC/14.xx                      (include + lib/x64: msvcrt, vcruntime, msvcprt, oldnames)
#   WINSDK=/path/to/Windows Kits/10                         (Include/<ver>/{ucrt,um,shared}, Lib/<ver>/{ucrt,um}/x64)
#   WINSDK_VERSION=10.0.26100.0
# On Windows itself use build-windows.ps1 (MSVC) instead.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; root="$(cd "$here/../.." && pwd)"
: "${FILAMENT_WINDOWS:?}" "${MSVC:?}" "${WINSDK:?}"; V="${WINSDK_VERSION:-10.0.26100.0}"
out="$root/runtimes/windows"; obj="$here/obj-windows"; mkdir -p "$out" "$obj"
CL=(clang --driver-mode=cl --target=x86_64-pc-windows-msvc -fms-compatibility-version=19.44)
inc=(/imsvc "$MSVC/include" /imsvc "$WINSDK/Include/$V/ucrt" /imsvc "$WINSDK/Include/$V/um" /imsvc "$WINSDK/Include/$V/shared"
     /I "$FILAMENT_WINDOWS/include")
flags=(/nologo /std:c++17 /O2 /MD /EHsc /GS- /DNDEBUG /DWIN32 /D_WIN32_WINNT=0x0A00 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS
       /Zc:__cplusplus /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH "/D__builtin_verbose_trap(a,b)=__builtin_trap()" -Wno-deprecated-declarations -Wno-microsoft-include -Wno-unused-command-line-argument)
for f in aqua_filament window_win32; do
  "${CL[@]}" "${flags[@]}" "${inc[@]}" /c "$root/native/src/$f.cpp" /Fo"$obj/$f.obj"
done
L="$FILAMENT_WINDOWS/lib/x86_64/md"
libs=(gltfio_core filameshio camutils ktxreader filament backend bluegl bluevk filabridge filaflat geometry image ibl-lite
      utils basis_transcoder dracodec meshoptimizer zstd uberzlib uberarchive smol-v mikktspace stb abseil perfetto)
args=(); for l in "${libs[@]}"; do [ -f "$L/$l.lib" ] && args+=("$L/$l.lib"); done
lld-link /nologo /DLL /OUT:"$out/aqua_filament.dll" /OPT:REF /OPT:ICF /INCREMENTAL:NO /MACHINE:X64 \
  "$obj/aqua_filament.obj" "$obj/window_win32.obj" "${args[@]}" \
  /LIBPATH:"$MSVC/lib/x64" /LIBPATH:"$WINSDK/Lib/$V/ucrt/x64" /LIBPATH:"$WINSDK/Lib/$V/um/x64" \
  msvcrt.lib vcruntime.lib msvcprt.lib ucrt.lib oldnames.lib kernel32.lib user32.lib gdi32.lib opengl32.lib shell32.lib \
  advapi32.lib ole32.lib winmm.lib shlwapi.lib dbghelp.lib bcrypt.lib synchronization.lib version.lib \
  /NODEFAULTLIB:libcmt /NODEFAULTLIB:libcpmt /NODEFAULTLIB:uuid.lib
rm -f "$out/aqua_filament.lib" "$out/aqua_filament.exp"
echo "Built $out/aqua_filament.dll ($(du -h "$out/aqua_filament.dll" | cut -f1))"
