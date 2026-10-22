# Cross-build AArch64 Linux from Windows/macOS/Linux with Zig 0.13.0.
# Retains the existing Cortex-A53 code-generation settings used on H700.
# Other ARM64 systems need compatible CPU features and a 64-bit frontend.
# The target's glibc must be >= 2.17. This is not an Android toolchain.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
# Preserve an explicit -DGP32_ZIG path in CMake's compiler probe projects.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES GP32_ZIG)
find_program(GP32_ZIG zig REQUIRED)
set(CMAKE_C_COMPILER "${GP32_ZIG}")
set(CMAKE_C_COMPILER_ARG1 "cc -target aarch64-linux-gnu.2.17 -mcpu=cortex_a53")
set(CMAKE_CXX_COMPILER "${GP32_ZIG}")
set(CMAKE_CXX_COMPILER_ARG1 "c++ -target aarch64-linux-gnu.2.17 -mcpu=cortex_a53")
set(CMAKE_AR "${GP32_ZIG}")
set(CMAKE_RANLIB "${GP32_ZIG}")
set(CMAKE_C_ARCHIVE_CREATE "<CMAKE_AR> ar qc <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_APPEND "<CMAKE_AR> ar q <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_FINISH "<CMAKE_RANLIB> ranlib <TARGET>")
set(CMAKE_CXX_ARCHIVE_CREATE "${CMAKE_C_ARCHIVE_CREATE}")
set(CMAKE_CXX_ARCHIVE_APPEND "${CMAKE_C_ARCHIVE_APPEND}")
set(CMAKE_CXX_ARCHIVE_FINISH "${CMAKE_C_ARCHIVE_FINISH}")

# Clang selects its IPO archive rules after the toolchain file is read and
# expects separate ar/ranlib executables. Zig exposes both as driver
# subcommands, so install the IPO rules after language detection using the
# same rules as normal archives.
set(_GP32_ZIG_IPO_ARCHIVE_HOOK
    "${CMAKE_CURRENT_LIST_DIR}/zig-ipo-archive.cmake")
# CMake before 3.29 accepts only one project include. Chain an existing hook
# explicitly rather than turning this variable into a list it cannot read.
if(CMAKE_PROJECT_INCLUDE AND
   NOT CMAKE_PROJECT_INCLUDE STREQUAL _GP32_ZIG_IPO_ARCHIVE_HOOK)
    set(_GP32_ZIG_PREVIOUS_PROJECT_INCLUDE "${CMAKE_PROJECT_INCLUDE}")
endif()
set(CMAKE_PROJECT_INCLUDE "${_GP32_ZIG_IPO_ARCHIVE_HOOK}")
unset(_GP32_ZIG_IPO_ARCHIVE_HOOK)
