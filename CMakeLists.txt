cmake_minimum_required(VERSION 3.16)
project(gp32emu C CXX)

option(GP32EMU_BUILD_SDL12 "Build optional SDL 1.2 platform backend and frontend" OFF)
option(GP32EMU_BUILD_SDL3 "Build optional SDL3 platform backend and frontend" OFF)
option(GP32EMU_BUILD_QT6 "Build Qt6/Linux GP32emu UI" OFF)
option(GP32EMU_BUILD_WIN64_FRONTEND "Build Win64 GP32emu UI" OFF)
option(GP32EMU_ENABLE_THREADS "Enable optional worker threads for audio/recording" ON)
option(GP32EMU_BUILD_LIBRETRO "Build the portable RetroArch/libretro core" OFF)
option(GP32EMU_BUILD_HEADLESS "Build the headless runner" ON)
option(GP32EMU_BUILD_BENCHMARK "Build the deterministic core benchmark" OFF)
option(GP32EMU_BUILD_TESTS "Build focused regression tests" OFF)
option(GP32EMU_ENABLE_IPO "Enable interprocedural optimization in release builds" OFF)
option(GP32EMU_REQUIRE_C23 "Fail when C23 is unavailable instead of falling back to C11" OFF)
option(GP32EMU_CPU_PROFILE "Build optional CPU workload counters for diagnosis" OFF)

# CMake added C_STANDARD 23 support in 3.21. Keep the older configure-time
# baseline working, but make every downgrade visible (or fatal on request).
if(CMAKE_VERSION VERSION_LESS 3.21)
    set(GP32EMU_C_STANDARD 11)
    set(GP32EMU_C23_REASON "CMake ${CMAKE_VERSION} predates C_STANDARD 23 support")
else()
    list(FIND CMAKE_C_COMPILE_FEATURES c_std_23 GP32EMU_C23_INDEX)
    if(GP32EMU_C23_INDEX GREATER_EQUAL 0)
        set(GP32EMU_C_STANDARD 23)
    else()
        set(GP32EMU_C_STANDARD 11)
        set(GP32EMU_C23_REASON "compiler ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION} does not advertise c_std_23")
    endif()
endif()

if(GP32EMU_C_STANDARD EQUAL 23)
    message(STATUS "Requesting C23 dialect (${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION})")
else()
    if(GP32EMU_REQUIRE_C23)
        message(FATAL_ERROR "The C23 dialect is required, but ${GP32EMU_C23_REASON}")
    endif()
    message(WARNING "Falling back to C11: ${GP32EMU_C23_REASON}")
endif()
set(CMAKE_C_STANDARD ${GP32EMU_C_STANDARD})
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if (GP32EMU_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT gp32_ipo_supported OUTPUT gp32_ipo_error)
    if (NOT gp32_ipo_supported)
        message(FATAL_ERROR "IPO unavailable: ${gp32_ipo_error}")
    endif()
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

add_library(gp32emu
    src/gp32.c
    src/arm920t.c
    src/s3c2400.c
    src/gp32_codec.c
    src/smartmedia.c
    src/smc_direct.c
    src/fxe.c
    src/fpk.c
    src/gp32_zip.c
    src/third_party/zip/zip.c
    src/input_script.c
    src/platform/platform.c
    src/audio/gp32_audio_resampler.c
    src/media/gp32_media.c
    src/media/gp32_video_effects.c
    src/save_atomic.c
)
target_include_directories(gp32emu PUBLIC include PRIVATE src)
if(GP32EMU_CPU_PROFILE)
    target_compile_definitions(gp32emu PUBLIC GP32EMU_CPU_PROFILE=1)
endif()
if (GP32EMU_BUILD_LIBRETRO)
    set_target_properties(gp32emu PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if (GP32EMU_BUILD_TESTS)
    enable_testing()
    foreach(test_name libretro_audio libretro_persistence libretro_input input_script arm_poll arm_jit arm_exception s3c2400_timing s3c2400_iis_irq s3c2400_irq s3c2400_lcd s3c2400_scanout s3c2400_pwm s3c2400_gpio s3c2400_state gp32_pcm gp32_timer gp32_callback gp32_wait gp32_swi_1ff gp32_file gp32_codec gp32_eeprom gp32_shadow gp32_game_fix gp32_fast_load gp32_state smartmedia savestate_session atomic_save)
        add_executable(${test_name}_test tests/${test_name}_test.c)
        target_include_directories(${test_name}_test PRIVATE src src/libretro)
        target_link_libraries(${test_name}_test PRIVATE gp32emu)
        if (NOT MSVC)
            target_compile_options(${test_name}_test PRIVATE -Wall -Wextra)
        endif()
    endforeach()
    add_test(NAME libretro_audio COMMAND libretro_audio_test ${CMAKE_CURRENT_BINARY_DIR}/audio-state.tmp)
    add_test(NAME libretro_persistence COMMAND libretro_persistence_test ${CMAKE_CURRENT_BINARY_DIR}/persistence)
    add_test(NAME libretro_input COMMAND libretro_input_test)
    add_test(NAME input_script COMMAND input_script_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/input_script_order.txt)
    add_test(NAME arm_poll COMMAND arm_poll_test)
    add_test(NAME arm_jit COMMAND arm_jit_test)
    add_test(NAME arm_exception COMMAND arm_exception_test)
    add_test(NAME s3c2400_timing COMMAND s3c2400_timing_test)
    add_test(NAME s3c2400_iis_irq COMMAND s3c2400_iis_irq_test)
    add_test(NAME s3c2400_irq COMMAND s3c2400_irq_test)
    add_test(NAME s3c2400_lcd COMMAND s3c2400_lcd_test)
    add_test(NAME s3c2400_scanout COMMAND s3c2400_scanout_test)
    add_test(NAME s3c2400_pwm COMMAND s3c2400_pwm_test)
    add_test(NAME s3c2400_gpio COMMAND s3c2400_gpio_test)
    add_test(NAME s3c2400_state COMMAND s3c2400_state_test)
    add_test(NAME gp32_pcm COMMAND gp32_pcm_test)
    add_test(NAME gp32_timer COMMAND gp32_timer_test)
    add_test(NAME gp32_callback COMMAND gp32_callback_test ${CMAKE_CURRENT_BINARY_DIR}/callback-state.tmp)
    add_test(NAME gp32_swi_1ff COMMAND gp32_swi_1ff_test)
    add_test(NAME gp32_wait COMMAND gp32_wait_test ${CMAKE_CURRENT_BINARY_DIR}/wait-state.tmp)
    add_test(NAME gp32_file COMMAND gp32_file_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/dotted-entry.zip)
    add_test(NAME gp32_codec COMMAND gp32_codec_test)
    add_test(NAME gp32_eeprom COMMAND gp32_eeprom_test)
    add_test(NAME gp32_shadow COMMAND gp32_shadow_test)
    add_test(NAME gp32_game_fix COMMAND gp32_game_fix_test)
    add_test(NAME gp32_fast_load COMMAND gp32_fast_load_test)
    add_test(NAME gp32_state COMMAND gp32_state_test ${CMAKE_CURRENT_BINARY_DIR}/gp32-state.tmp)
    add_test(NAME smartmedia COMMAND smartmedia_test)
    add_test(NAME savestate_session COMMAND savestate_session_test)
    add_test(NAME atomic_save COMMAND atomic_save_test ${CMAKE_CURRENT_BINARY_DIR}/atomic-save)
    add_executable(arm_jit_recycle_test tests/arm_jit_recycle_test.c)
    target_include_directories(arm_jit_recycle_test PRIVATE src include)
    target_compile_definitions(arm_jit_recycle_test PRIVATE _XOPEN_SOURCE=700)
    add_test(NAME arm_jit_recycle COMMAND arm_jit_recycle_test)
    if (WIN32 AND GP32EMU_ENABLE_THREADS)
        add_executable(win64_audio_test tests/win64_audio_test.c)
        target_include_directories(win64_audio_test PRIVATE src)
        target_link_libraries(win64_audio_test PRIVATE gp32emu winmm ole32 uuid avrt)
        add_test(NAME win64_audio COMMAND win64_audio_test)
    endif()
endif()
if (WIN32)
    target_compile_definitions(gp32emu PRIVATE _WIN32_WINNT=0x0600)
else()
    target_link_libraries(gp32emu PUBLIC m)
endif()
target_compile_definitions(gp32emu PUBLIC GP32EMU_ENABLE_THREADS=$<BOOL:${GP32EMU_ENABLE_THREADS}>)
if (GP32EMU_ENABLE_THREADS AND NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(gp32emu PUBLIC Threads::Threads)
endif()
target_compile_definitions(gp32emu PRIVATE _XOPEN_SOURCE=700 _FILE_OFFSET_BITS=64 ZIP_ENABLE_DEFLATE=0 ZIP_HAVE_SYMLINK=0)

if (NOT MSVC)
    set_source_files_properties(src/third_party/zip/zip.c PROPERTIES COMPILE_OPTIONS "-w")
else()
    set_source_files_properties(src/third_party/zip/zip.c PROPERTIES COMPILE_OPTIONS "/w")
endif()

if (GP32EMU_BUILD_SDL3)
    find_package(SDL3 REQUIRED CONFIG)
    add_library(gp32emu_sdl3
        src/platform/sdl3/common.c
        src/platform/sdl3/video.c
        src/platform/sdl3/audio.c
        src/platform/sdl3/input.c
        src/input/gp32_sdl3_input_shared.c
    )
    target_include_directories(gp32emu_sdl3 PUBLIC include PRIVATE src)
    target_link_libraries(gp32emu_sdl3 PUBLIC gp32emu SDL3::SDL3)
    if (MSVC)
        target_compile_options(gp32emu_sdl3 PRIVATE /W4)
    else()
        target_compile_options(gp32emu_sdl3 PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_executable(gp32_sdl3 src/sdl3_main.c)
    target_include_directories(gp32_sdl3 PRIVATE src)
    target_link_libraries(gp32_sdl3 PRIVATE gp32emu gp32emu_sdl3 SDL3::SDL3)
endif()

if (GP32EMU_BUILD_QT6)
    find_package(Qt6 REQUIRED COMPONENTS Widgets)
    find_package(SDL3 CONFIG)
    if (NOT TARGET gp32emu_sdl3)
        if (SDL3_FOUND)
            add_library(gp32emu_sdl3
                src/platform/sdl3/common.c
                src/platform/sdl3/video.c
                src/platform/sdl3/audio.c
                src/platform/sdl3/input.c
                src/input/gp32_sdl3_input_shared.c
            )
            target_include_directories(gp32emu_sdl3 PUBLIC include PRIVATE src)
            target_link_libraries(gp32emu_sdl3 PUBLIC gp32emu SDL3::SDL3)
        endif()
    endif()
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    add_executable(gp32emu_qt6
        src/qt/main.cpp
        src/qt/GP32Engine.cpp
        src/qt/GP32MainWindow.cpp
        src/qt/GP32VideoWidget.cpp
        resources/gp32emu.qrc
    )
    target_include_directories(gp32emu_qt6 PRIVATE include src)
    target_link_libraries(gp32emu_qt6 PRIVATE gp32emu Qt6::Widgets)
    if (TARGET gp32emu_sdl3)
        target_compile_definitions(gp32emu_qt6 PRIVATE GP32EMU_QT_SDL3_AUDIO=1 GP32EMU_QT_SDL3_INPUT=1 SDL_MAIN_HANDLED=1)
        target_link_libraries(gp32emu_qt6 PRIVATE gp32emu_sdl3 SDL3::SDL3)
    endif()
endif()

if (GP32EMU_BUILD_WIN64_FRONTEND)
    if (NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "GP32EMU_BUILD_WIN64_FRONTEND is Win64-only; no Win32 frontend is provided.")
    endif()
    find_package(SDL3 REQUIRED CONFIG)
    add_executable(gp32emu_win64 WIN32
        src/win64/gp32_win64_main.c
        src/win64/gp32_win64_ui.c
        src/win64/gp32_win64_video.c
        src/win64/gp32_win64_audio.c
        src/win64/gp32_win64_sdl_input.c
        src/input/gp32_sdl3_input_shared.c
        src/win64/gp32emu.rc
    )
    target_include_directories(gp32emu_win64 PRIVATE include src)
    target_compile_definitions(gp32emu_win64 PRIVATE GP32EMU_WIN64_FRONTEND=1 SDL_MAIN_HANDLED=1)
    target_link_libraries(gp32emu_win64 PRIVATE gp32emu SDL3::SDL3 user32 gdi32 comdlg32 winmm ole32 uuid d3d11 dxgi avrt shell32)
    if (MINGW)
        # Zig's MinGW driver ignores -mwindows; select the PE subsystem explicitly.
        target_link_options(gp32emu_win64 PRIVATE "-Wl,--subsystem,windows")
    endif()
    if (GP32EMU_BUILD_TESTS)
        add_executable(win64_frontend_test tests/win64_frontend_test.c
            src/win64/gp32_win64_ui.c src/win64/gp32emu.rc
            src/win64/gp32_win64_video.c src/win64/gp32_win64_audio.c
            src/win64/gp32_win64_sdl_input.c src/input/gp32_sdl3_input_shared.c)
        target_include_directories(win64_frontend_test PRIVATE include src)
        target_link_libraries(win64_frontend_test PRIVATE gp32emu SDL3::SDL3 user32 gdi32 comdlg32 winmm ole32 uuid d3d11 dxgi avrt shell32)
        add_test(NAME win64_frontend COMMAND win64_frontend_test)
        set_tests_properties(win64_frontend PROPERTIES TIMEOUT 30)
    endif()
    # Windows does not search the imported library's directory for DLLs.
    # Keep shared SDL beside the GUI and its device-free lifecycle test.
    get_target_property(gp32_sdl3_type SDL3::SDL3 TYPE)
    if (gp32_sdl3_type STREQUAL "SHARED_LIBRARY")
        foreach(frontend_target gp32emu_win64 win64_frontend_test)
            if (TARGET ${frontend_target})
                add_custom_command(TARGET ${frontend_target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:${frontend_target}>"
                    VERBATIM)
            endif()
        endforeach()
    endif()
endif()
if (MSVC)
    target_compile_options(gp32emu PRIVATE /W4)
else()
    target_compile_options(gp32emu PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (GP32EMU_BUILD_HEADLESS)
    add_executable(gp32_headless src/headless_main.c)
    target_link_libraries(gp32_headless PRIVATE gp32emu)
endif()

if (GP32EMU_BUILD_BENCHMARK)
    add_executable(gp32_bench scripts/bench_core.c)
    target_include_directories(gp32_bench PRIVATE src)
    target_link_libraries(gp32_bench PRIVATE gp32emu)
    add_executable(gp32_library_probe scripts/library_probe.c)
    target_include_directories(gp32_library_probe PRIVATE src)
    target_link_libraries(gp32_library_probe PRIVATE gp32emu)
    add_executable(gp32_smc_inspect scripts/smc_inspect.c)
    target_include_directories(gp32_smc_inspect PRIVATE src)
    target_link_libraries(gp32_smc_inspect PRIVATE gp32emu)
    # Compatibility sweep probe: same drive and identity JSON as gp32_bench
    # plus frame-stamped core log lines and a per-frame PC/video trace.
    add_executable(gp32_compat scripts/compat_probe.c)
    target_include_directories(gp32_compat PRIVATE src)
    target_link_libraries(gp32_compat PRIVATE gp32emu)
endif()

if (GP32EMU_BUILD_LIBRETRO)
    add_library(gp32emu_libretro SHARED src/libretro/libretro.c)
    target_include_directories(gp32emu_libretro PRIVATE src src/libretro)
    target_link_libraries(gp32emu_libretro PRIVATE gp32emu)
    set_target_properties(gp32emu_libretro PROPERTIES PREFIX "")
    if (NOT WIN32 AND NOT APPLE)
        # Export only the libretro API; internal emulator and miniz symbols stay local.
        target_link_options(gp32emu_libretro PRIVATE
            "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/libretro/libretro.map")
        set_property(TARGET gp32emu_libretro APPEND PROPERTY LINK_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/src/libretro/libretro.map")
    endif()
    if (ANDROID)
        set_target_properties(gp32emu_libretro PROPERTIES SUFFIX "_android.so")
        # RetroArch Android loads cores with the .so extension, via the NDK.
        target_link_options(gp32emu_libretro PRIVATE "-Wl,--no-undefined")
        # Android 15+ devices may use 16 KB pages. NDK r28 and newer align
        # 64-bit libraries by default; keep NDK r27 and older compliant too.
        if (ANDROID_ABI STREQUAL "arm64-v8a" OR ANDROID_ABI STREQUAL "x86_64")
            target_link_options(gp32emu_libretro PRIVATE
                "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")
        endif()
    endif()
endif()

if (GP32EMU_BUILD_SDL12)
    find_package(SDL REQUIRED)
    add_library(gp32emu_sdl12
        src/platform/sdl12/common.c
        src/platform/sdl12/video.c
        src/platform/sdl12/audio.c
        src/platform/sdl12/input.c
    )
    target_include_directories(gp32emu_sdl12 PUBLIC include PRIVATE src ${SDL_INCLUDE_DIR})
    target_link_libraries(gp32emu_sdl12 PUBLIC gp32emu ${SDL_LIBRARY})
    if (MSVC)
        target_compile_options(gp32emu_sdl12 PRIVATE /W4)
    else()
        target_compile_options(gp32emu_sdl12 PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_executable(gp32_sdl12 src/sdl12_main.c)
    target_include_directories(gp32_sdl12 PRIVATE src ${SDL_INCLUDE_DIR})
    target_link_libraries(gp32_sdl12 PRIVATE gp32emu gp32emu_sdl12 ${SDL_LIBRARY})
endif()
