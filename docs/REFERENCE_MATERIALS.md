# GP32 implementation references

The [OpenHandhelds GP32 archive](https://dl.openhandhelds.org/cgi-bin/gp32.cgi)
contains SDKs, source releases, firmware and development documentation. The
2026-10-04 local collection selected all non-Games categories, including technical
demos: 389 of 398 discovered items were downloaded, totaling 475,321,042 bytes.
Nine returned 404, DNS or timeout errors. This is a collection-time result,
not proof that every unreachable external host is permanently gone.

Original archives remain outside this repository. Per-item source URLs,
SHA-256 hashes and errors are recorded in the private
`results/resume84-openhandhelds/state.json`. Old host installers were not run.
The [Internet Archive collection](https://archive.org/download/GP32Collection)
is a fallback catalog; it was not bulk-downloaded or bundled with the emulator.

## Useful source and documentation

| Material | Relevant use |
| --- | --- |
| [Mirko SDK 0.91](https://dl.openhandhelds.org/gp32/uploads/Home/GP32%20-%20Development/Libraries/mirkoSDK091.tar.gz) | Direct hardware access, PCM start/stop, clock and DMA setup |
| [Slubman firmware source](https://dl.openhandhelds.org/cgi-bin/gp32.cgi?0,0,0,0,46,531) | Open firmware launch paths, file services and hardware initialization; archive includes a COPYING file |
| `api_ref_2.1.5.zip`, `startup-en.zip`, `work_en.zip` | SDK API and application startup conventions |
| `OKFDocs_v2.zip`, `OKLibs_v2a.zip` | Alternative runtime and file API conventions |
| `mame4all_gp32_v1.1_src.tar.bz2`, `gpneopopsrc.zip`, `gpnese-src.zip` | Software using GP32 hardware; these are not implementations of a GP32 host emulator |
| [Samsung S3C2400 manual](https://datasheets.chipdb.org/Samsung/S3C2400.pdf) | Register definitions and peripheral behavior; mirrored manufacturer document, consulted online |

Consult the included licenses before reusing source. The firmware/SDK study
has already exposed a missing IIS FIFO flush at playback stop, corrected using
a small independent implementation and a public-peripheral-API regression.
See [the performance and correctness record](GP32_PERFORMANCE_STRATEGY.md).

## Keep boot paths distinct

BIOS+SMC executes the supplied firmware. Direct FXE/FPK loading supplies firmware
services through HLE. Passing one path does not validate the other. The supplied
FPK set has eight distinct GPKG packages with embedded FXE/assets: a bounded
600-frame direct-load smoke run produced one application file-error UI, five
uniform frames and two runs exceeding the 60-second host limit. These results
do not establish either successful gameplay or a loader deadlock. A package may
still require user data or input despite including its own companion assets.

Firmware file names and version numbers are insufficient identity. Preserve
exact hashes with reproductions: several supplied v1.6.6 images behaved
differently with the same core and SMC. See [PC coverage](PC_LIBRARY_COVERAGE.md).
No automatic per-title firmware selection is implemented.
