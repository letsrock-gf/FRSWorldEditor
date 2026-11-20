cmake_minimum_required(VERSION 3.21)
project(EditorFastTest LANGUAGES C CXX)

# editor-fast-test.asi: the editor's end in Need for Speed World for Launch
# Game / Fast test. The editor leaves a request in <game>\scripts\
# editor-fast-test.txt; this plugin passes the title and the driver select by
# code and puts the car at the editor's camera (see the top of the source).
#
# The game is a 32-bit process, so it is built for Win32:
#
#   cmake -S fasttest-nfsw -B fasttest-nfsw/build -G "Visual Studio 18 2026" -A Win32
#   cmake --build fasttest-nfsw/build --config Release
#
# The build runs the plugin's own self-test. MinHook (TsudaKageyu, BSD
# 2-Clause) is downloaded at its 1.3.4 tag, or taken from a copy on disk with
# -DFETCHCONTENT_SOURCE_DIR_MINHOOK=<folder>.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# One C runtime, linked in, for the plugin and for MinHook (whose own CMake
# file is older than the policy that lets this setting reach it).
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(minhook
    URL https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.zip)
FetchContent_MakeAvailable(minhook)

add_library(editor-fast-test SHARED editor-fast-test.cpp)
set_target_properties(editor-fast-test PROPERTIES PREFIX "" SUFFIX ".asi")
target_link_libraries(editor-fast-test PRIVATE minhook bcrypt)
target_compile_definitions(editor-fast-test PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)

# The same source without the hooks: the pose maths and the request file.
add_executable(editor-fast-test-test editor-fast-test.cpp)
target_compile_definitions(editor-fast-test-test PRIVATE EDITOR_FAST_TEST_TEST _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_link_libraries(editor-fast-test-test PRIVATE bcrypt)
add_custom_command(TARGET editor-fast-test-test POST_BUILD COMMAND editor-fast-test-test)
