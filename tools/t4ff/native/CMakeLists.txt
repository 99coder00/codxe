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
  src/core/zone_cache.cpp
  src/convert/record_map.cpp
  src/convert/converter.cpp
  src/convert/assets.cpp
  src/convert/xanim.cpp
  src/convert/scripts.cpp
  src/convert/menus.cpp
  src/convert/loadscreen.cpp
  src/convert/stream.cpp
  src/core/dump.cpp
  src/core/progress.cpp
  src/app/convert.cpp
  src/app/usermap.cpp
  src/app/deps.cpp
  src/convert/usermaps_menu.cpp
  src/convert/library.cpp
  src/convert/merge.cpp
  src/convert/techsets.cpp
  src/core/process.cpp
  src/core/sha256.cpp
  src/core/pyre.cpp
  src/core/pystr.cpp
  src/audio/audio.cpp
  src/audio/soundbudget.cpp
)
target_include_directories(t4ffcore PUBLIC src)
target_link_libraries(t4ffcore PUBLIC zlibstatic)
if(MSVC)
  target_compile_options(t4ffcore PRIVATE /W4 /permissive-)
endif()

# the command line tool
add_executable(t4ff-cli src/cli/main.cpp src/cli/t4ff_cli.cpp src/core/resources.rc)
target_link_libraries(t4ff-cli PRIVATE t4ffcore)
set_target_properties(t4ff-cli PROPERTIES OUTPUT_NAME t4ff-cli)
if(MSVC)
  target_compile_options(t4ff-cli PRIVATE /W4 /permissive-)
  # the zone tree is walked recursively, as the game's loader does
  target_link_options(t4ff-cli PRIVATE /STACK:67108864)
endif()

# Dear ImGui (third_party/imgui, MIT) with its Win32 and Direct3D 11 backends
set(IMGUI_DIR third_party/imgui)
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
  ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
  ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR})
if(MSVC)
  target_compile_options(imgui PRIVATE /W0)
endif()

# the window (t4ff.exe); started with --worker it runs a command of t4ff-cli's for the window
add_executable(t4ff WIN32
  src/gui/main.cpp
  src/gui/app.cpp
  src/gui/settings.cpp
  src/gui/worker.cpp
  src/gui/theme.cpp
  src/gui/shell.cpp
  src/cli/t4ff_cli.cpp
  src/core/resources.rc
)
target_link_libraries(t4ff PRIVATE t4ffcore imgui d3d11 dxgi d3dcompiler dwmapi shell32 ole32)
if(MSVC)
  target_compile_options(t4ff PRIVATE /W4 /permissive-)
  target_link_options(t4ff PRIVATE /STACK:67108864)
endif()
