# gp32emu libretro core: ndk-build project settings.
#
# arm64-v8a   -> AArch64 native backend (ARM_JIT_NATIVE_A64)
# x86_64      -> x86-64 native backend
# armeabi-v7a -> portable ARM block interpreter (no host backend exists)
APP_ABI := arm64-v8a armeabi-v7a x86_64
APP_PLATFORM := android-21
APP_OPTIM := release
