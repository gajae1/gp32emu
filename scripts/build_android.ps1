param(
    [string]$Ndk = $env:ANDROID_NDK_HOME,
    [ValidateSet('arm64-v8a','armeabi-v7a')][string[]]$Abi = @('arm64-v8a','armeabi-v7a'),
    [string]$OutputDirectory,
    [int]$Jobs = 4,
    [int]$ApiLevel = 21
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $Ndk) { throw 'Pass -Ndk <Android NDK directory> or set ANDROID_NDK_HOME.' }
$toolchain = Join-Path $Ndk 'build/cmake/android.toolchain.cmake'
if (-not (Test-Path -LiteralPath $toolchain -PathType Leaf)) { throw "NDK toolchain missing: $toolchain" }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repoRoot 'build-android' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$cmake = (Get-Command cmake -ErrorAction Stop).Source
$manifest = @()
foreach ($targetAbi in $Abi) {
    $buildDirectory = Join-Path $OutputDirectory $targetAbi
    & $cmake -S $repoRoot -B $buildDirectory -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
        "-DANDROID_ABI=$targetAbi" "-DANDROID_PLATFORM=android-$ApiLevel" -DCMAKE_BUILD_TYPE=Release `
        -DGP32EMU_BUILD_LIBRETRO=ON -DGP32EMU_BUILD_HEADLESS=OFF -DGP32EMU_REQUIRE_C23=ON
    if ($LASTEXITCODE -ne 0) { throw "Configure failed: $targetAbi" }
    & $cmake --build $buildDirectory --target gp32emu_libretro --parallel $Jobs
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $targetAbi" }
    $core = Join-Path $buildDirectory 'gp32emu_libretro_android.so'
    if (-not (Test-Path -LiteralPath $core -PathType Leaf)) { throw "Core missing: $core" }
    $manifest += [pscustomobject]@{abi=$targetAbi;api=$ApiLevel;file=$core;sha256=(Get-FileHash -LiteralPath $core -Algorithm SHA256).Hash.ToLowerInvariant()}
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'manifest.json') -Encoding utf8
$manifest
