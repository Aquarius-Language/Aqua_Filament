# Aqua_Filament native runtime (aqua_filament.dll / libaqua_filament.so).
# Windows (Visual Studio 2022+ / MSVC):
#   cmake -S native -B native/build/msvc -G "Visual Studio 17 2022" -A x64 -DFILAMENT_DIR=C:/path/filament-v1.78.0-windows
#   cmake --build native/build/msvc --config Release
# Linux: prefer native/build/build-linux.sh (Filament's Linux release is built with libc++).
cmake_minimum_required(VERSION 3.20)
project(aqua_filament LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT FILAMENT_DIR)
  message(FATAL_ERROR "Set FILAMENT_DIR to the extracted official Filament 1.78.0 release")
endif()
set(SOURCES src/aqua_filament.cpp)
if(WIN32)
  list(APPEND SOURCES src/window_win32.cpp)
  set(LIBDIR ${FILAMENT_DIR}/lib/x86_64/md)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
else()
  list(APPEND SOURCES src/window_x11.cpp src/compat_glibc.c)
  set(LIBDIR ${FILAMENT_DIR}/lib/x86_64)
endif()
add_library(aqua_filament SHARED ${SOURCES})
target_include_directories(aqua_filament PRIVATE ${FILAMENT_DIR}/include)
set(FILAMENT_LIBS gltfio_core filameshio camutils ktxreader filament backend bluegl bluevk filabridge filaflat geometry
    image ibl-lite utils basis_transcoder dracodec meshoptimizer zstd uberzlib uberarchive smol-v mikktspace stb abseil perfetto
    webgpu_dawn)
foreach(lib ${FILAMENT_LIBS})
  find_library(FL_${lib} NAMES ${lib} lib${lib} PATHS ${LIBDIR} NO_DEFAULT_PATH)
  if(FL_${lib})
    target_link_libraries(aqua_filament PRIVATE ${FL_${lib}})
  endif()
endforeach()
if(WIN32)
  target_compile_definitions(aqua_filament PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0A00)
  target_link_libraries(aqua_filament PRIVATE opengl32 gdi32 user32 shell32 advapi32 ole32 winmm shlwapi dbghelp bcrypt synchronization version)
  set_target_properties(aqua_filament PROPERTIES OUTPUT_NAME aqua_filament
      RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/../runtimes/windows)
else()
  set_target_properties(aqua_filament PROPERTIES CXX_VISIBILITY_PRESET hidden C_VISIBILITY_PRESET hidden
      LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../runtimes/linux)
  target_link_options(aqua_filament PRIVATE -Wl,--exclude-libs,ALL -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/build/exports.map)
  target_link_libraries(aqua_filament PRIVATE X11 pthread dl m)
endif()
