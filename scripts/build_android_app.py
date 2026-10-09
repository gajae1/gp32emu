#!/usr/bin/env python3
"""Build a signed standalone APK with the installed SDK/NDK; no Gradle.

Requires Python 3.10+, CMake, Ninja, a JDK, SDK build-tools 36.0.0,
platform android-36 and NDK 28.2.13676358. Nothing is downloaded or installed.
Only an existing keystore is used (default alias: androiddebugkey).
GP32EMU_KEYSTORE_PASSWORD and GP32EMU_KEY_PASSWORD override the standard
Android debug passwords; GP32EMU_KEY_ALIAS optionally overrides the alias.
Passwords reach apksigner through environment variables, never command lines.

Outputs and retained intermediate files live outside the source checkout.
--debug produces a debuggable, testOnly private APK with a distinct filename.
--test also builds a same-key instrumentation APK using platform Java APIs
(including android.test.*); external dependencies such as AndroidX are not
resolved. This script never installs an APK or communicates with a device.

References: https://developer.android.com/tools/{aapt2,d8,apksigner,zipalign}
            https://developer.android.com/guide/practices/page-sizes
"""

from __future__ import annotations

import argparse
from datetime import date
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile


ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "org.gp32emu.app"
VERSION = "1.0.0"
MIN_API, TARGET_API = 23, 35
ANDROID_NS = "http://schemas.android.com/apk/res/android"
ABIS = {
    "arm64-v8a": "aarch64-linux-android",
    "armeabi-v7a": "armv7a-linux-androideabi",
}
ALIGN_FLAGS = "-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384"
# Explicit assets only, never copy a BIOS/game directory or opaque archive.
ASSET_SUFFIXES = {
    ".txt", ".md", ".json", ".xml", ".html", ".css", ".js",
    ".png", ".jpg", ".jpeg", ".webp", ".gif", ".svg", ".ttf", ".otf",
    ".ogg", ".wav", ".mp3",
}


class BuildError(Exception):
    pass


def require_file(path: Path) -> Path:
    if not path.is_file():
        raise BuildError(f"Required file missing: {path}")
    return path


def executable(name: str, fallback: Path | None = None) -> Path:
    found = shutil.which(name)
    if found:
        return Path(found)
    if fallback and fallback.is_file():
        return fallback
    raise BuildError(f"Required tool not found: {name}")


def run(label: str, args: list, *, env=None, capture=False) -> str:
    # Never echo a command or serialize the signing environment, even on error.
    print(f"[build] {label}", flush=True)
    result = subprocess.run(
        [str(arg) for arg in args], cwd=ROOT, env=env,
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None,
        text=True, encoding="utf-8", errors="replace", check=False,
    )
    if result.returncode:
        if capture and result.stdout:
            print(result.stdout, file=sys.stderr)
        raise BuildError(f"{label} failed (exit {result.returncode}).")
    return result.stdout or ""


def input_files(directory: Path) -> list[Path]:
    """Reject links escaping a designated input tree; never scan the checkout."""
    if not directory.exists():
        return []
    if directory.is_symlink() or not directory.is_dir():
        raise BuildError(f"Expected an ordinary input directory: {directory}")
    files = []
    for path in sorted(directory.rglob("*")):
        if path.is_symlink() or not path.resolve().is_relative_to(directory.resolve()):
            raise BuildError(f"Linked input is not allowed: {path}")
        if path.is_file():
            files.append(path)
    return files


def check_asset(path: Path, relative: Path) -> None:
    names = [part.lower() for part in relative.parts]
    if (path.suffix.lower() not in ASSET_SUFFIXES
            or any("bios" in part or part.startswith(("rom", "gp32166"))
                   for part in names)):
        raise BuildError(f"Refusing possible BIOS/ROM or unsupported asset: {path}")


def copy_asset(source: Path, destination: Path) -> None:
    if destination.exists():
        raise BuildError(f"Duplicate packaged asset: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)


def stage_assets(source: Path, destination: Path) -> None:
    destination.mkdir(parents=True)
    for path in input_files(source):
        relative = path.relative_to(source)
        check_asset(path, relative)
        if relative.parts[0].lower() == "licenses":
            raise BuildError("android/assets/licenses is reserved for automatic notices.")
        copy_asset(path, destination / relative)

    notices = destination / "licenses"
    # The checkout currently has no root LICENSE; include it automatically if
    # one is added, and retain README's actual upstream licensing statement.
    for path in sorted(ROOT.iterdir()):
        if path.is_file() and re.match(r"^(licen[sc]e|copying|notice)([._-]|$)", path.name, re.I):
            copy_asset(path, notices / path.name)
    copy_asset(require_file(ROOT / "README.md"), notices / "PROJECT-README.md")
    license_files = input_files(ROOT / "licenses")
    if not license_files:
        raise BuildError("No third-party notices found in licenses/.")
    for path in license_files:
        check_asset(path, path.relative_to(ROOT / "licenses"))
        copy_asset(path, notices / "third-party" / path.relative_to(ROOT / "licenses"))

    # miniz carries additional MIT/public-domain notices inside its source.
    # Extract complete comments without inventing or rewriting licence text.
    for path in input_files(ROOT / "src" / "third_party"):
        relative = path.relative_to(ROOT)
        if re.match(r"^(licen[sc]e|copying|notice)([._-]|$)", path.name, re.I):
            copy_asset(path, notices / relative)
        elif path.suffix.lower() in {".c", ".h"}:
            comments = re.findall(r"/\*.*?\*/", path.read_text(encoding="utf-8"), re.S)
            blocks = list(dict.fromkeys(block for block in comments if
                "permission is hereby granted" in block.lower()
                or "redistribution and use" in block.lower()
                or "this is free and unencumbered software" in block.lower()))
            if blocks:
                target = notices / relative.with_suffix(path.suffix + ".notices.txt")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text("\n\n".join(blocks) + "\n", encoding="utf-8")


def stage_manifest(source: Path, destination: Path, *, debug: bool, test: bool) -> None:
    ET.register_namespace("android", ANDROID_NS)
    tree = ET.parse(require_file(source))
    manifest = tree.getroot()
    package = manifest.get("package")
    if manifest.tag != "manifest" or not package:
        raise BuildError(f"Invalid Android manifest: {source}")
    if not test and package != PACKAGE:
        raise BuildError(f"App manifest package must be {PACKAGE}.")
    if test:
        instrumentation = manifest.find("instrumentation")
        if (package == PACKAGE or instrumentation is None
                or instrumentation.get(f"{{{ANDROID_NS}}}targetPackage") != PACKAGE):
            raise BuildError(f"Test manifest needs a distinct package and targetPackage={PACKAGE}.")
    manifest.set(f"{{{ANDROID_NS}}}versionCode", "1")
    manifest.set(f"{{{ANDROID_NS}}}versionName", VERSION)
    sdk = manifest.find("uses-sdk")
    if sdk is None:
        sdk = ET.SubElement(manifest, "uses-sdk")
    sdk.set(f"{{{ANDROID_NS}}}minSdkVersion", str(MIN_API))
    sdk.set(f"{{{ANDROID_NS}}}targetSdkVersion", str(TARGET_API))
    application = manifest.find("application")
    if application is None:
        application = ET.SubElement(manifest, "application")
    application.set(f"{{{ANDROID_NS}}}debuggable", str(debug or test).lower())
    application.set(f"{{{ANDROID_NS}}}testOnly", str(debug or test).lower())
    # The frontend may dlopen a path under ApplicationInfo.nativeLibraryDir.
    application.set(f"{{{ANDROID_NS}}}extractNativeLibs", "true")
    tree.write(destination, encoding="utf-8", xml_declaration=True)


def verify_elf(objdump: Path, library: Path) -> None:
    output = run(f"Check ELF alignment: {library.parent.name}/{library.name}",
                 [objdump, "-p", library], capture=True)
    alignments = re.findall(r"\bLOAD\b[^\n]*\balign 2\*\*(\d+)", output)
    if not alignments or any(int(value) < 14 for value in alignments):
        raise BuildError(f"ELF LOAD segments are not 16 KB aligned: {library}")


def build_native(tools: dict, ndk: Path, output: Path, work: Path, debug: bool) -> list[Path]:
    libraries = []
    mode = "Debug" if debug else "Release"
    for abi, triple in ABIS.items():
        build_dir = output / "native" / mode.lower() / abi
        # Mirror build_android.ps1's required C23/libretro settings, while
        # aligning both ABIs and avoiding any stale prebuilt core copy.
        run(f"Configure core: {abi}", [
            tools["cmake"], "-S", ROOT, "-B", build_dir, "-G", "Ninja",
            f"-DCMAKE_TOOLCHAIN_FILE={ndk / 'build/cmake/android.toolchain.cmake'}",
            f"-DCMAKE_MAKE_PROGRAM={tools['ninja']}", f"-DANDROID_ABI={abi}",
            f"-DANDROID_PLATFORM=android-{MIN_API}", f"-DCMAKE_BUILD_TYPE={mode}",
            "-DGP32EMU_BUILD_LIBRETRO=ON", "-DGP32EMU_BUILD_HEADLESS=OFF",
            "-DGP32EMU_BUILD_TESTS=OFF", "-DGP32EMU_REQUIRE_C23=ON",
            f"-DCMAKE_SHARED_LINKER_FLAGS={ALIGN_FLAGS}",
        ])
        run(f"Build core: {abi}", [tools["cmake"], "--build", build_dir,
            "--target", "gp32emu_libretro", "--parallel", "4"])
        libdir = work / "lib" / abi
        libdir.mkdir(parents=True)
        core = libdir / "libgp32core.so"
        shutil.copyfile(require_file(build_dir / "gp32emu_libretro_android.so"), core)
        frontend = libdir / "libgp32frontend.so"
        run(f"Build C23 JNI bridge: {abi}", [
            tools["clang"], f"--target={triple}{MIN_API}",
            f"--sysroot={tools['sysroot']}", "-std=c23", "-fPIC", "-shared",
            "-Wall", "-Wextra", "-O0" if debug else "-O2",
            "-g" if debug else "-DNDEBUG", "-I", ROOT / "src/libretro",
            "-I", ROOT / "include", ROOT / "android/native/frontend.c",
            "-o", frontend, "-Wl,--no-undefined", "-Wl,-soname,libgp32frontend.so",
            *ALIGN_FLAGS.split(), "-ljnigraphics", "-llog", "-ldl",
        ])
        for library in (core, frontend):
            verify_elf(tools["objdump"], library)
            libraries.append(library)
    return libraries


def build_apk(tools: dict, source: Path, work: Path, keystore: Path,
              *, debug: bool, libraries=(), app_classes: Path | None = None) -> tuple[Path, Path]:
    test = app_classes is not None
    work.mkdir(parents=True)
    manifest = work / "AndroidManifest.xml"
    stage_manifest(source / "AndroidManifest.xml", manifest, debug=debug, test=test)
    assets = work / "assets"
    stage_assets(source / "assets", assets)
    generated, classes, dex = (work / name for name in ("generated", "classes", "dex"))
    for directory in (generated, classes, dex):
        directory.mkdir()
    unsigned = work / "unsigned.apk"
    link = [tools["aapt2"], "link", "-o", unsigned, "--manifest", manifest,
            "-I", tools["android_jar"], "--java", generated, "-A", assets]
    resources = input_files(source / "res")
    if resources:
        for path in resources:
            check_asset(path, path.relative_to(source / "res"))
        compiled = work / "resources.zip"
        run("Compile resources", [tools["aapt2"], "compile", "--dir", source / "res", "-o", compiled])
        link.append(compiled)
    run("Link APK resources and manifest", link)
    sources = sorted((source / "java").rglob("*.java")) + sorted(generated.rglob("*.java"))
    if not sources:
        raise BuildError(f"No Java sources in {source / 'java'}")
    classpath = [tools["android_jar"]]
    test_jars = tools["test_jars"] if test else []
    classpath.extend(test_jars)
    if app_classes:
        classpath.append(app_classes)
    # An argfile and a class JAR avoid Windows command-line length limits.
    arguments = ["-encoding", "UTF-8", "-source", "8", "-target", "8",
                 "-bootclasspath", os.pathsep.join(str(tools[key]) for key in ("android_jar", "lambda_stubs")), "-classpath",
                 os.pathsep.join(str(path) for path in classpath), "-d", classes,
                 "-g" if debug or test else "-g:none", *sources]
    argfile = work / "javac.args"
    argfile.write_text("\n".join('"' + str(arg).replace("\\", "/").replace('"', '\\"') + '"'
                                 for arg in arguments), encoding="utf-8")
    run("Compile Java", [tools["javac"], f"@{argfile}"])
    class_jar = work / "classes.jar"
    with zipfile.ZipFile(class_jar, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(classes.rglob("*.class")):
            archive.write(path, path.relative_to(classes).as_posix())
    d8 = [tools["java"], "-Xmx1g", "-cp", tools["d8"], "com.android.tools.r8.D8",
          "--debug" if debug or test else "--release", "--min-api", str(MIN_API),
          "--lib", tools["android_jar"], "--output", dex]
    for jar in test_jars:
        d8.extend(["--lib", jar])
    if app_classes:
        d8.extend(["--classpath", app_classes])
    run("Compile DEX", [*d8, class_jar])
    require_file(dex / "classes.dex")
    with zipfile.ZipFile(unsigned, "a") as archive:
        for path in sorted(dex.glob("*.dex")):
            archive.write(path, path.name, compress_type=zipfile.ZIP_DEFLATED)
        for path in libraries:
            archive.write(path, f"lib/{path.parent.name}/{path.name}", compress_type=zipfile.ZIP_STORED)
    aligned, signed = work / "aligned.apk", work / "signed.apk"
    run("Align APK (16 KB native pages)", [tools["zipalign"], "-f", "-P", "16", "4", unsigned, aligned])
    signing_env = os.environ.copy()
    signing_env.setdefault("GP32EMU_KEYSTORE_PASSWORD", "android")
    signing_env.setdefault("GP32EMU_KEY_PASSWORD", signing_env["GP32EMU_KEYSTORE_PASSWORD"])
    signer = [tools["java"], "-jar", tools["apksigner"]]
    run("Sign with existing keystore", [*signer, "sign", "--ks", keystore,
        "--ks-key-alias", os.environ.get("GP32EMU_KEY_ALIAS", "androiddebugkey"),
        "--ks-pass", "env:GP32EMU_KEYSTORE_PASSWORD", "--key-pass", "env:GP32EMU_KEY_PASSWORD",
        "--min-sdk-version", str(MIN_API), "--v4-signing-enabled", "false",
        "--out", signed, aligned], env=signing_env)
    run("Verify APK signature", [*signer, "verify", "--verbose", "--min-sdk-version", str(MIN_API), signed])
    run("Verify signed APK alignment", [tools["zipalign"], "-c", "-P", "16", "4", signed])
    return signed, class_jar


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sdk", type=Path, default=Path(os.environ.get("ANDROID_SDK_ROOT")
        or os.environ.get("ANDROID_HOME") or "C:/Users/qaz03/AppData/Local/Android/Sdk"),
        help="Installed Android SDK root")
    parser.add_argument("--ndk", type=Path, help="NDK root (default: SDK/ndk/28.2.13676358)")
    parser.add_argument("--keystore", type=Path, default=Path.home() / ".android/debug.keystore",
        help="Existing signing key; missing keys are never generated")
    parser.add_argument("--output-dir", type=Path,
        default=ROOT.parent / "artifacts" / f"android-app-{date.today():%Y%m%d}",
        help="Artifact directory outside the checkout")
    parser.add_argument("--debug", action="store_true", help="Private testOnly/debuggable build, filename ends in -debug.apk")
    parser.add_argument("--test", action="store_true", help="Also build android/test instrumentation APK; never runs it")
    args = parser.parse_args()

    keystore = args.keystore.expanduser().resolve()
    if not keystore.is_file():
        raise BuildError(f"Existing keystore required: {keystore}. Refusing to create a key.")
    sdk = args.sdk.expanduser().resolve()
    ndk = (args.ndk or sdk / "ndk/28.2.13676358").expanduser().resolve()
    output = args.output_dir.expanduser().resolve()
    if output.is_relative_to(ROOT):
        raise BuildError("--output-dir must be outside the source checkout.")
    require_file(ndk / "build/cmake/android.toolchain.cmake")
    require_file(ROOT / "android/AndroidManifest.xml")
    require_file(ROOT / "android/native/frontend.c")
    if not list((ROOT / "android/java").rglob("*.java")):
        raise BuildError("No app Java sources found in android/java.")
    if args.test:
        require_file(ROOT / "android/test/AndroidManifest.xml")
        if not list((ROOT / "android/test/java").rglob("*.java")):
            raise BuildError("--test needs Java sources in android/test/java.")

    suffix = ".exe" if os.name == "nt" else ""
    host = "windows-x86_64" if os.name == "nt" else ("darwin-x86_64" if sys.platform == "darwin" else "linux-x86_64")
    llvm = ndk / "toolchains/llvm/prebuilt" / host
    build_tools = sdk / "build-tools/36.0.0"
    platform = sdk / "platforms/android-36"
    javac = executable("javac")
    tools = {
        "cmake": executable("cmake"),
        "ninja": executable("ninja", ROOT.parent / ".tools/Scripts/ninja.exe"),
        "javac": javac,
        "java": require_file(javac.parent / f"java{suffix}"),
        "clang": require_file(llvm / "bin" / f"clang{suffix}"),
        "objdump": require_file(llvm / "bin" / f"llvm-objdump{suffix}"),
        "sysroot": llvm / "sysroot",
        "aapt2": require_file(build_tools / f"aapt2{suffix}"),
        "zipalign": require_file(build_tools / f"zipalign{suffix}"),
        "d8": require_file(build_tools / "lib/d8.jar"),
        "apksigner": require_file(build_tools / "lib/apksigner.jar"),
        "android_jar": require_file(platform / "android.jar"),
        "lambda_stubs": require_file(build_tools / "core-lambda-stubs.jar"),
        "test_jars": sorted((platform / "optional").glob("android.test.*.jar")),
    }
    output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="work-", dir=output))
    print(f"Intermediate files: {work}", flush=True)
    native = build_native(tools, ndk, output, work, args.debug)
    app, classes = build_apk(tools, ROOT / "android", work / "app", keystore,
                             debug=args.debug, libraries=native)
    stem = f"gp32emu-{VERSION}-android" + ("-debug" if args.debug else "")
    results = [(app, output / f"{stem}.apk")]
    if args.test:
        test_apk, _ = build_apk(tools, ROOT / "android/test", work / "test", keystore,
                                debug=True, app_classes=classes)
        results.append((test_apk, output / f"{stem}-test.apk"))
    # Only expose final names after every requested APK has passed verification.
    for signed, destination in results:
        os.replace(signed, destination)
        print(f"APK: {destination}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (BuildError, OSError, ET.ParseError, zipfile.BadZipFile) as error:
        print(f"Build failed: {error}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("Build interrupted; intermediate files retained.", file=sys.stderr)
        sys.exit(130)
