# GP32emu 1.1.0 Switch core source package

This package contains the GP32emu core sources and the compiled static library
`gp32emu_libretro_libnx.a`. To play, download `gp32emu-1.1.0-switch-libnx.zip`
instead. A static library cannot be loaded by RetroArch on a console.

## Rebuild

Install devkitPro's devkitA64 and libnx, then use its configured shell in this
source directory. This package was built with the official
`devkitpro/devkita64` container; package versions are in `toolchain-packages.txt`.

```sh
export PATH="$DEVKITPRO/devkitA64/bin:$PATH"
make -f Makefile.libretro platform=libnx GP32EMU_REQUIRE_C23=1
```

This produces `gp32emu_libretro_libnx.a`. Copy it into a RetroArch 1.22.2 source
checkout as `libretro_libnx.a`, then run `make -f Makefile.libnx`. The result is
`retroarch_switch.nro`. See `manifest.json` for the exact RetroArch commit and
the [official build guide](https://docs.libretro.com/development/retroarch/compilation/switch-libnx/)
for dependencies. `retroarch-source.tar.gz` contains that frontend's source.

This port uses the ARM64 JIT through libnx, with an interpreter fallback when
JIT permissions are unavailable. Switch support is experimental; compatibility
varies by game and setup.
No BIOS images, games or saves are included.

## Credits

GP32emu originates with [gameblabla](https://github.com/gameblabla/gp32emu).
This fork is maintained at [gajae1/gp32emu](https://github.com/gajae1/gp32emu).
The upstream repository does not state a licence for its own code; this package
does not add one. MAME-derived code, ZIP/miniz and RetroArch retain their
respective notices in `licenses/` and the accompanying source files.
