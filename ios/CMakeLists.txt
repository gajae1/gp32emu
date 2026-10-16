# Reproducible Xcode project preparation for the iPhone/iPad frontend.
#
# This file is an out-of-tree consumer of the existing core: it never edits or
# duplicates a core source. All host services the frontend needs (display,
# audio, input, persistence paths) are implemented in the app itself, so every
# optional core frontend and the worker threads stay off and the core is built
# exactly once. Interpreter execution remains the default.
#
# The Xcode generator is the supported path; see ios/README.md for the exact
# command, the pending signing steps and the pending validation gates.

cmake_minimum_required(VERSION 3.21)

# The iOS deployment target must be known before the compiler check so the SDK
# probe uses it. Override with -DCMAKE_OSX_DEPLOYMENT_TARGET=<version>.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum iOS version" FORCE)
endif()

# The frontend version matches CFBundleShortVersionString in app/Info.plist, and
# CFBundleVersion there carries the build number; keep them in sync when the
# development version changes.
project(gp32emu_ios VERSION 1.1.0 LANGUAGES C)

set(GP32EMU_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Force every optional host off before the core is added: this app owns its
# event loop, audio device, input and save paths; the tests, benchmark and the
# other frontends are not part of an application bundle.
set(GP32EMU_BUILD_HEADLESS OFF CACHE BOOL "" FORCE)
set(GP32EMU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GP32EMU_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(GP32EMU_BUILD_SDL12 OFF CACHE BOOL "" FORCE)
set(GP32EMU_BUILD_SDL3 OFF CACHE BOOL "" FORCE)
set(GP32EMU_BUILD_QT6 OFF CACHE BOOL "" FORCE)
set(GP32EMU_BUILD_WIN64_FRONTEND OFF CACHE BOOL "" FORCE)
set(GP32EMU_BUILD_LIBRETRO OFF CACHE BOOL "" FORCE)
# The iOS app drains guest audio from its own render callback on the device's
# audio thread; the core's optional worker threads stay off so every gp32_*
# call still happens on the single owner queue.
set(GP32EMU_ENABLE_THREADS OFF CACHE BOOL "" FORCE)

# The core is created, run and destroyed by ios/bridge/gp32_ios.c. The root
# project still selects C23 when the toolchain advertises it and falls back to
# C11 otherwise (GP32EMU_REQUIRE_C23 stays OFF), so an AppleClang or CMake that
# predates c_std_23 builds the same sources as C11.
add_subdirectory("${GP32EMU_SOURCE_ROOT}" gp32emu-core)

# The developer option includes A64 code generation, but still starts with the
# interpreter. It requires the explicit split-mapping debugger handshake.
# Simulator builds keep executable-memory backends out. Device builds include
# the backend but never activate it without an explicit debugger handshake.
option(GP32EMU_IOS_DEVELOPER_JIT "Include experimental debugger-assisted device JIT" ON)
if(GP32EMU_IOS_DEVELOPER_JIT AND CMAKE_SYSTEM_NAME STREQUAL "iOS"
   AND NOT CMAKE_OSX_SYSROOT MATCHES "simulator")
    target_compile_definitions(gp32emu PRIVATE GP32EMU_DISABLE_JIT=0)
else()
    target_compile_definitions(gp32emu PRIVATE GP32EMU_DISABLE_JIT=1)
endif()

# The bridge is plain C11 with no Apple framework, so it is its own target and
# can be compiled and syntax-checked on a host without an iOS SDK. The Apple
# app links this same target instead of compiling a second copy.
add_library(gp32emu_ios_bridge STATIC bridge/gp32_ios.c)
target_compile_features(gp32emu_ios_bridge PRIVATE c_std_11)
target_include_directories(gp32emu_ios_bridge PUBLIC
    "${GP32EMU_SOURCE_ROOT}/include"
    "${GP32EMU_SOURCE_ROOT}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/bridge"
)
target_link_libraries(gp32emu_ios_bridge PUBLIC gp32emu)
if(NOT MSVC)
    target_compile_options(gp32emu_ios_bridge PRIVATE -Wall -Wextra)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    message(STATUS "gp32emu ios: host build; only the portable core and bridge target gp32emu_ios_bridge are configured.")
    message(STATUS "gp32emu ios: verify the bridge with: cmake --build <build-dir> --target gp32emu_ios_bridge")
    message(STATUS "gp32emu ios: use -DCMAKE_SYSTEM_NAME=iOS -G Xcode on macOS for the app project (ios/README.md).")
else()
    enable_language(OBJC)

    set(GP32EMU_IOS_APP_SOURCES
        app/main.m
        app/GP32Emulator.m
        app/GP32Audio.m
        app/GP32JIT.m
        app/GP32PadControls.m
        app/GP32EmulatorViewController.m
    )

    add_executable(gp32emu_ios MACOSX_BUNDLE ${GP32EMU_IOS_APP_SOURCES})
    # include/ is public on the core target; src/ and ios/bridge are needed by
    # the bridge translation unit (smc_direct.h, gp32_audio_resampler.h).
    target_include_directories(gp32emu_ios PRIVATE
        "${GP32EMU_SOURCE_ROOT}/include"
        "${GP32EMU_SOURCE_ROOT}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/bridge"
        "${CMAKE_CURRENT_SOURCE_DIR}/app"
    )
    target_link_libraries(gp32emu_ios PRIVATE gp32emu gp32emu_ios_bridge
        "-framework UIKit"
        "-framework Foundation"
        "-framework CoreGraphics"
        "-framework QuartzCore"
        "-framework AVFoundation"
        "-framework GameController"
        "-framework UniformTypeIdentifiers"
        "-framework Security"
    )
    if(NOT MSVC)
        target_compile_options(gp32emu_ios PRIVATE -Wall -Wextra)
    endif()
    if(GP32EMU_IOS_DEVELOPER_JIT AND NOT CMAKE_OSX_SYSROOT MATCHES "simulator")
        target_compile_definitions(gp32emu_ios PRIVATE GP32EMU_IOS_DEVELOPER_JIT=1)
    endif()

    set_target_properties(gp32emu_ios PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/app/Info.plist"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.gp32emu.ios"
        XCODE_ATTRIBUTE_PRODUCT_NAME "GP32emu"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "org.gp32emu.ios"
        XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        XCODE_GENERATE_SCHEME TRUE
        XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC "YES"
        # Signing is a local decision: the project keeps automatic signing on so
        # a personal team produces a runnable device build. See ios/README.md.
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
    )

    # License/attribution text ships with the app. No BIOS image and no game
    # image is ever copied into a bundle by this file.
    set(GP32EMU_IOS_LICENSES
        BSD-3-Clause-MAME-derived.txt
        miniz.txt
        MIT-BDMEmu-frontend.txt
        MIT-kuba-zip.txt
    )
    foreach(license_name IN LISTS GP32EMU_IOS_LICENSES)
        set(license_path "${GP32EMU_SOURCE_ROOT}/licenses/${license_name}")
        set_source_files_properties("${license_path}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
        target_sources(gp32emu_ios PRIVATE "${license_path}")
    endforeach()
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/CREDITS.md" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources")
    target_sources(gp32emu_ios PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/CREDITS.md")

    message(STATUS "gp32emu ios: iPhone/iPad app target gp32emu_ios (deployment target ${CMAKE_OSX_DEPLOYMENT_TARGET})")
    message(STATUS "gp32emu ios: interpreter by default; developer JIT=${GP32EMU_IOS_DEVELOPER_JIT}; no BIOS or games in bundle")
endif()
