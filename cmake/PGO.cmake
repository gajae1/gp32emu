# Optional Clang IR profiles for the shared emulator and libretro boundary.
# Keep compile options private: several tests include production .c files in
# altered translation units and must not consume those functions' profiles.
set(GP32EMU_PGO_MODE "OFF" CACHE STRING "Clang IR PGO: OFF, GENERATE or USE")
set_property(CACHE GP32EMU_PGO_MODE PROPERTY STRINGS OFF GENERATE USE)
set(GP32EMU_PGO_FILE "" CACHE FILEPATH "Merged llvm-profdata input for PGO USE")
set(GP32EMU_PGO_RUNTIME "" CACHE FILEPATH "Optional target compiler-rt profile archive for PGO GENERATE (ELF)")

if(GP32EMU_PGO_MODE STREQUAL "OFF")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gp32-pgo-$<CONFIG>.txt" CONTENT "mode=OFF\n")
    return()
endif()
if(NOT GP32EMU_PGO_MODE MATCHES "^(GENERATE|USE)$")
    message(FATAL_ERROR "GP32EMU_PGO_MODE must be OFF, GENERATE or USE")
endif()
if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR CMAKE_C_COMPILER_VERSION VERSION_LESS 18 OR MSVC)
    message(FATAL_ERROR "GP32emu PGO requires the Clang 18+ GNU-style driver (including Zig 0.13)")
endif()
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "GP32emu PGO requires a Release build")
endif()

# LLVM's IR profile keys include absolute paths for static functions. Strip
# only the checkout prefix, retaining src/... to distinguish equal basenames.
# Both training and use builds compute this for their own checkout location.
string(REGEX MATCHALL "/" gp32_pgo_prefix_parts "${CMAKE_CURRENT_SOURCE_DIR}/")
list(LENGTH gp32_pgo_prefix_parts gp32_pgo_prefix_depth)
set(gp32_pgo_options "SHELL:-mllvm -static-func-strip-dirname-prefix=${gp32_pgo_prefix_depth}")
set(gp32_pgo_profile_hash "none")
if(GP32EMU_PGO_MODE STREQUAL "GENERATE")
    list(APPEND gp32_pgo_options -fprofile-generate -fprofile-update=atomic
        "SHELL:-mllvm -enable-name-compression=false")
    # Link requirements propagate even to a test using the instrumented core;
    # the test's own sources do not inherit instrumentation options.
    target_link_options(gp32emu PUBLIC "$<$<CONFIG:Release>:-fprofile-generate>")
    if(GP32EMU_PGO_RUNTIME)
        if(NOT UNIX OR APPLE OR NOT EXISTS "${GP32EMU_PGO_RUNTIME}")
            message(FATAL_ERROR "GP32EMU_PGO_RUNTIME requires an existing target archive and an ELF platform")
        endif()
        target_link_libraries(gp32emu PUBLIC
            "$<$<CONFIG:Release>:-Wl,--whole-archive>"
            "$<$<CONFIG:Release>:${GP32EMU_PGO_RUNTIME}>"
            "$<$<CONFIG:Release>:-Wl,--no-whole-archive>")
    endif()
    # The profile runtime writes at process exit. A training frontend may
    # dlclose the core first; keep its callbacks mapped until process exit.
    if(TARGET gp32emu_libretro AND UNIX AND NOT APPLE)
        target_link_options(gp32emu_libretro PRIVATE "$<$<CONFIG:Release>:-Wl,-z,nodelete>")
    endif()
    set(gp32_pgo_zip_option -fno-profile-generate)
else()
    if(NOT EXISTS "${GP32EMU_PGO_FILE}" OR IS_DIRECTORY "${GP32EMU_PGO_FILE}")
        message(FATAL_ERROR "PGO USE needs GP32EMU_PGO_FILE pointing to a merged .profdata file")
    endif()
    file(SHA256 "${GP32EMU_PGO_FILE}" gp32_pgo_profile_hash)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GP32EMU_PGO_FILE}")
    list(APPEND gp32_pgo_options "-fprofile-use=${GP32EMU_PGO_FILE}"
        -Werror=backend-plugin)
    target_link_options(gp32emu PUBLIC "$<$<CONFIG:Release>:-fprofile-use=${GP32EMU_PGO_FILE}>")
    set(gp32_pgo_zip_option -fno-profile-use)
endif()

foreach(gp32_pgo_target gp32emu gp32emu_libretro)
    if(TARGET ${gp32_pgo_target})
        foreach(gp32_pgo_option IN LISTS gp32_pgo_options)
            target_compile_options(${gp32_pgo_target} PRIVATE "$<$<CONFIG:Release>:${gp32_pgo_option}>")
        endforeach()
        if(GP32EMU_PGO_MODE STREQUAL "USE")
            # Replacing a profile must rebuild objects, not merely relink.
            get_target_property(gp32_pgo_sources ${gp32_pgo_target} SOURCES)
            set_property(SOURCE ${gp32_pgo_sources} APPEND PROPERTY OBJECT_DEPENDS "${GP32EMU_PGO_FILE}")
        endif()
    endif()
endforeach()
set_property(SOURCE src/third_party/zip/zip.c APPEND PROPERTY COMPILE_OPTIONS
    "$<$<CONFIG:Release>:${gp32_pgo_zip_option}>")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gp32-pgo-$<CONFIG>.txt" CONTENT
"mode=$<IF:$<CONFIG:Release>,${GP32EMU_PGO_MODE},OFF>
compiler=${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}
profile_sha256=${gp32_pgo_profile_hash}
ipo=${GP32EMU_ENABLE_IPO}
options=${gp32_pgo_options}
")
