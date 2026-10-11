# GP32emu 1.0.1 Switch core source package

This package contains the GP32emu core sources and the compiled static library
`gp32emu_libretro_libnx.a`. To play, download `gp32emu-1.0.1-switch-libnx.zip`
instead. A static library cannot be loaded by RetroArch on a console.

## Rebuild

Install devkitPro's devkitA64 and libnx, then use its configured shell in this
source directory. The release was built with the official
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
The full project source archive includes the manually dispatched GitHub Actions
workflow used for this build.

This port uses the ARM64 JIT through libnx, with an interpreter fallback when
JIT permissions are unavailable. It has not been tested on a Switch console.
No BIOS images, games or saves are included.

## 한국어 안내

Switch용 코어 소스와 빌드된 정적 라이브러리를 담은 개발용 압축파일입니다.
실행하려면 별도 `switch-libnx.zip`의 NRO를 사용하세요. `.a`는 RetroArch 소스와
연결해 NRO를 만들 때 사용하는 파일입니다.

## Credits

GP32emu originates with [gameblabla](https://github.com/gameblabla/gp32emu).
This fork is maintained at [gajae1/gp32emu](https://github.com/gajae1/gp32emu).
The upstream repository does not state a licence for its own code; this package
does not add one. MAME-derived code, ZIP/miniz and RetroArch retain their
respective notices in `licenses/` and the accompanying source files.
