# Build Halo on Windows with Visual Studio 2022 (Desktop C++ workload):
#   cmake -S . -B build -A x64
#   cmake --build build --config Release
# Or cross-compile from Linux/macOS with ./build.sh (Zig).
cmake_minimum_required(VERSION 3.20)
project(Halo LANGUAGES CXX RC)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT, no redistributable needed

file(GLOB HALO_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(Halo WIN32 ${HALO_SOURCES} res/halo.rc)
target_compile_definitions(Halo PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
if(MSVC)
  target_compile_options(Halo PRIVATE /W4 /utf-8 /permissive-)
  target_link_options(Halo PRIVATE /MANIFEST:NO /ENTRY:wWinMainCRTStartup)   # manifest comes from halo.rc
endif()
target_link_libraries(Halo PRIVATE d2d1 dwrite ole32 oleaut32 uuid dwmapi shcore shell32 user32 gdi32 advapi32 shlwapi windowscodecs comctl32 uxtheme winhttp bcrypt version comdlg32 hid setupapi)
set_target_properties(Halo PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/dist)
