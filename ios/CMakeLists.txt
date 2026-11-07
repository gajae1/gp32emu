# Reproducible Xcode project preparation for the iPhone/iPad frontend.
#
# This file is an out-of-tree consumer of the existing core: it never edits or
# duplicates a core source. All host services the frontend needs (display,
# audio, input, persistence paths) are implemented in the app itself, so every
# optional core frontend and the worker threads stay off and the core is built
# exactly once. Device JIT activation is coordinated by the app.
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

# The developer option includes A64 code generation. The app requests the
# split-mapping handshake before starting a game, including in Release builds.
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
        app/GP32JITService.m
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

    option(GP32EMU_IOS_BUILTIN_JIT "Embed the StikJIT activation helper" ON)
    if(GP32EMU_IOS_BUILTIN_JIT AND GP32EMU_IOS_DEVELOPER_JIT
       AND NOT CMAKE_OSX_SYSROOT MATCHES "simulator")
        enable_language(Swift)
        set(GP32EMU_STIKJIT_ROOT "" CACHE PATH "Existing StikJIT 2.0.1 XCFramework directory")
        if(NOT GP32EMU_STIKJIT_ROOT)
            set(stik_archive "${CMAKE_CURRENT_BINARY_DIR}/_deps/StikJIT-2.0.1.zip")
            if(EXISTS "${stik_archive}")
                file(SHA256 "${stik_archive}" stik_digest)
            endif()
            if(NOT stik_digest STREQUAL "c376687326a785f5ed1da8caca08adc709b56919019dd751bca059b92b175d95")
                file(DOWNLOAD
                "https://github.com/StikDebug/StikJIT/releases/download/2.0.1/StikJIT.xcframework.zip"
                "${stik_archive}"
                EXPECTED_HASH SHA256=c376687326a785f5ed1da8caca08adc709b56919019dd751bca059b92b175d95
                TLS_VERIFY ON)
            endif()
            file(ARCHIVE_EXTRACT INPUT "${stik_archive}" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/_deps")
            set(GP32EMU_STIKJIT_ROOT "${CMAKE_CURRENT_BINARY_DIR}/_deps/StikJIT.xcframework")
        endif()
        set(stik_framework "${GP32EMU_STIKJIT_ROOT}/ios-arm64/StikJIT.framework")
        if(NOT EXISTS "${stik_framework}/StikJIT")
            message(FATAL_ERROR "StikJIT 2.0.1 device framework is missing")
        endif()
        add_executable(gp32emu_jit_helper MACOSX_BUNDLE helper/GP32JITHelper.swift)
        target_link_libraries(gp32emu_jit_helper PRIVATE "${stik_framework}" "-framework Foundation")
        target_link_options(gp32emu_jit_helper PRIVATE "-e" "_NSExtensionMain")
        set_target_properties(gp32emu_jit_helper PROPERTIES
            XCODE_PRODUCT_TYPE "com.apple.product-type.app-extension"
            BUNDLE_EXTENSION appex
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/helper/Info.plist"
            MACOSX_BUNDLE_GUI_IDENTIFIER "org.gp32emu.ios.JITHelper"
            XCODE_ATTRIBUTE_PRODUCT_NAME "GP32JITHelper"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "org.gp32emu.ios.JITHelper"
            XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "17.4"
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
            XCODE_ATTRIBUTE_APPLICATION_EXTENSION_API_ONLY "YES"
            XCODE_ATTRIBUTE_SKIP_INSTALL "YES"
            XCODE_ATTRIBUTE_SWIFT_VERSION "5.0"
            XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
            XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS "@executable_path/../../Frameworks")
        add_dependencies(gp32emu_ios gp32emu_jit_helper)
        set_property(TARGET gp32emu_ios PROPERTY XCODE_EMBED_APP_EXTENSIONS gp32emu_jit_helper)
        set_property(TARGET gp32emu_ios PROPERTY XCODE_EMBED_FRAMEWORKS "${stik_framework}")
        set_property(TARGET gp32emu_ios PROPERTY XCODE_EMBED_FRAMEWORKS_CODE_SIGN_ON_COPY YES)
        target_compile_definitions(gp32emu_ios PRIVATE GP32EMU_IOS_BUILTIN_JIT=1)
        set_source_files_properties("${GP32EMU_SOURCE_ROOT}/licenses/MPL-2.0-StikJIT.txt"
            PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
        target_sources(gp32emu_ios PRIVATE "${GP32EMU_SOURCE_ROOT}/licenses/MPL-2.0-StikJIT.txt")
    endif()

    message(STATUS "gp32emu ios: iPhone/iPad app target gp32emu_ios (deployment target ${CMAKE_OSX_DEPLOYMENT_TARGET})")
    message(STATUS "gp32emu ios: automatic device JIT=${GP32EMU_IOS_DEVELOPER_JIT}; no BIOS or games in bundle")
endif()
