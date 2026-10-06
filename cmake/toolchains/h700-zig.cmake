# Cross-build H700 Linux from Windows/macOS/Linux with Zig 0.13.0.
# The target's glibc must be >= 2.17. This is not an Android toolchain.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
find_program(GP32_ZIG zig REQUIRED)
set(CMAKE_C_COMPILER "${GP32_ZIG}")
set(CMAKE_C_COMPILER_ARG1 "cc -target aarch64-linux-gnu.2.17 -mcpu=cortex_a53")
set(CMAKE_CXX_COMPILER "${GP32_ZIG}")
set(CMAKE_CXX_COMPILER_ARG1 "c++ -target aarch64-linux-gnu.2.17 -mcpu=cortex_a53")
set(CMAKE_AR "${GP32_ZIG}")
set(CMAKE_RANLIB "${GP32_ZIG}")
# CMake 4.x drives LTO through the compiler-specific archive tools instead of
# CMAKE_AR, and the fallback check fails when they are empty. Point both at the
# same zig wrapper so IPO/LTO stays usable on this toolchain.
set(CMAKE_C_COMPILER_AR "${GP32_ZIG}")
set(CMAKE_C_COMPILER_RANLIB "${GP32_ZIG}")
set(CMAKE_CXX_COMPILER_AR "${GP32_ZIG}")
set(CMAKE_CXX_COMPILER_RANLIB "${GP32_ZIG}")
set(CMAKE_C_ARCHIVE_CREATE "<CMAKE_AR> ar qc <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_FINISH "<CMAKE_RANLIB> ranlib <TARGET>")
set(CMAKE_CXX_ARCHIVE_CREATE "${CMAKE_C_ARCHIVE_CREATE}")
set(CMAKE_CXX_ARCHIVE_FINISH "${CMAKE_C_ARCHIVE_FINISH}")
