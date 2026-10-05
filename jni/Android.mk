# gp32emu libretro core for Android (ndk-build).
#
# Mirrors the source list and flag policy of Makefile.libretro and
# CMakeLists.txt so the ndk-build core matches the CMake core. The output is
# libs/<abi>/libretro.so, the standard libretro core artifact name; the
# RetroArch/libretro-super Android packaging step renames it to
# gp32emu_libretro_android.so (the name RetroArch loads).
LOCAL_PATH := $(call my-dir)
GP32EMU_DIR := $(LOCAL_PATH)/..

# C-standard policy from cmake/c_standard.mk: prefer -std=c23, accept the older
# -std=c2x spelling, and fall back to C11 on an NDK whose Clang predates both.
ifeq ($(OS),Windows_NT)
GP32EMU_NULL := NUL
else
GP32EMU_NULL := /dev/null
endif
GP32EMU_C23_FLAG := $(strip $(shell $(TARGET_CC) -x c -std=c23 -fsyntax-only $(GP32EMU_DIR)/cmake/c_standard_probe.c > $(GP32EMU_NULL) 2>&1 && echo -std=c23))
ifeq ($(GP32EMU_C23_FLAG),)
GP32EMU_C23_FLAG := $(strip $(shell $(TARGET_CC) -x c -std=c2x -fsyntax-only $(GP32EMU_DIR)/cmake/c_standard_probe.c > $(GP32EMU_NULL) 2>&1 && echo -std=c2x))
endif
ifeq ($(GP32EMU_C23_FLAG),)
GP32EMU_C23_FLAG := -std=c11
endif
$(info gp32emu: C standard flag $(GP32EMU_C23_FLAG))

GP32EMU_INCLUDES := -I$(GP32EMU_DIR)/include -I$(GP32EMU_DIR)/src -I$(GP32EMU_DIR)/src/libretro
GP32EMU_DEFINES := -D__LIBRETRO__ -DGP32EMU_ENABLE_THREADS=1 \
    -D_XOPEN_SOURCE=700 -D_FILE_OFFSET_BITS=64 -DZIP_ENABLE_DEFLATE=0 -DZIP_HAVE_SYMLINK=0

# miniz/zip.c is third-party code; CMake builds it with warnings disabled.
include $(CLEAR_VARS)
LOCAL_MODULE := gp32emu_zip
LOCAL_SRC_FILES := ../src/third_party/zip/zip.c
LOCAL_CFLAGS := $(GP32EMU_C23_FLAG) -O3 $(GP32EMU_DEFINES) -w
LOCAL_EXPORT_C_INCLUDES := $(GP32EMU_DIR)/src/third_party/zip
include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := retro
LOCAL_SRC_FILES := \
    ../src/gp32.c \
    ../src/arm920t.c \
    ../src/s3c2400.c \
    ../src/gp32_codec.c \
    ../src/smartmedia.c \
    ../src/smc_direct.c \
    ../src/fxe.c \
    ../src/fpk.c \
    ../src/gp32_zip.c \
    ../src/input_script.c \
    ../src/platform/platform.c \
    ../src/audio/gp32_audio_resampler.c \
    ../src/media/gp32_media.c \
    ../src/media/gp32_video_effects.c \
    ../src/libretro/libretro.c
LOCAL_CFLAGS := $(GP32EMU_C23_FLAG) -O3 -fPIC -Wall -Wextra -Wpedantic $(GP32EMU_DEFINES) $(GP32EMU_INCLUDES)
LOCAL_STATIC_LIBRARIES := gp32emu_zip
LOCAL_LDLIBS := -lm
# Export only the libretro API; internal emulator symbols stay local.
LOCAL_LDFLAGS := -Wl,--version-script=$(GP32EMU_DIR)/src/libretro/libretro.map -Wl,--no-undefined
# Android 15+ devices may use 16 KB pages; keep NDK r27 and older compliant.
ifneq ($(filter arm64-v8a x86_64,$(TARGET_ARCH_ABI)),)
LOCAL_LDFLAGS += -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384
endif
include $(BUILD_SHARED_LIBRARY)
