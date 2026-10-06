# t4ff native: the converter of PC World at War usermaps for the Xbox 360, in C++.
#
#   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Release
#
# The structure layouts and zone code commands are generated from OpenAssetTools' definitions by
# tools/gen_schema.py into data/ and compiled into the executables (see README.md).
cmake_minimum_required(VERSION 3.20)
project(t4ff_native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
  add_compile_options(/utf-8 /MP)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# zlib (third_party/zlib, zlib license)
file(GLOB ZLIB_SOURCES third_party/zlib/*.c)
add_library(zlibstatic STATIC ${ZLIB_SOURCES})
target_include_directories(zlibstatic PUBLIC third_party/zlib)
if(MSVC)
  target_compile_options(zlibstatic PRIVATE /W0)
endif()

# the core library
add_library(t4ffcore STATIC
  src/core/json.cpp
  src/core/layout.cpp
  src/core/commands.cpp
  src/core/zone.cpp
  src/core/fastfile.cpp
  src/core/platforms.cpp
  src/core/resources.cpp
  src/core/threads.cpp
  src/core/xenos.cpp
  src/core/dxt.cpp
  src/core/wavelet.cpp
  src/core/zip.cpp
  src/core/image.cpp
)
target_include_directories(t4ffcore PUBLIC src)
target_link_libraries(t4ffcore PUBLIC zlibstatic)
if(MSVC)
  target_compile_options(t4ffcore PRIVATE /W4 /permissive-)
endif()

# the command line tool
add_executable(t4ff-cli src/cli/main.cpp src/core/resources.rc)
target_link_libraries(t4ff-cli PRIVATE t4ffcore)
set_target_properties(t4ff-cli PROPERTIES OUTPUT_NAME t4ff-cli)
if(MSVC)
  target_compile_options(t4ff-cli PRIVATE /W4 /permissive-)
  # the zone tree is walked recursively, as the game's loader does
  target_link_options(t4ff-cli PRIVATE /STACK:67108864)
endif()
