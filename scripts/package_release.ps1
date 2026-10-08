#Requires -Version 7.0
param(
    [Parameter(Mandatory)][string]$WindowsBuild,
    [Parameter(Mandatory)][string]$LinuxArm64Build,
    [Parameter(Mandatory)][string]$AndroidArm64Build,
    [Parameter(Mandatory)][string]$AndroidArmv7Build,
    [Parameter(Mandatory)][string]$SdlDll,
    [Parameter(Mandatory)][string]$SdlLicense,
    [Parameter(Mandatory)][string]$StripTool,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$version = '1.0.0'
$revision = (git -C $repo rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify source revision.' }
$dirty = git -C $repo status --porcelain
if ($LASTEXITCODE -ne 0 -or $dirty) { throw 'Commit reviewed source before packaging.' }
$out = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $out) { throw 'Output already exists; choose a new directory.' }

$info = Join-Path $repo 'gp32emu_libretro.info'
$licenses = Join-Path $repo 'licenses'
# Every archive carries the same documentation and license notices.
$commonCopies = @(
    @{From="$repo/README.md";To='README.md'},
    @{From="$repo/docs/RELEASE-$version.md";To='RELEASE-NOTES.md'},
    @{From="$repo/docs/MANUAL.ko.md";To='docs/MANUAL.ko.md'}
)
# Five independent archives; Platform names both the archive and its manifest field.
$packages = @(
    @{Platform='windows-x64';Info=$false;Strip=@();Copies=@(
        @{From=(Join-Path $WindowsBuild 'gp32emu_win64.exe');To='gp32emu_win64.exe'},
        @{From=$SdlDll;To='SDL3.dll'},
        @{From=$SdlLicense;To='SDL3-LICENSE.txt'})},
    @{Platform='libretro-windows-x64';Info=$true;Strip=@();Copies=@(
        @{From=(Join-Path $WindowsBuild 'gp32emu_libretro.dll');To='gp32emu_libretro.dll'})},
    @{Platform='linux-aarch64';Info=$true;Strip=@('gp32emu_libretro.so');Copies=@(
        @{From=(Join-Path $LinuxArm64Build 'gp32emu_libretro.so');To='gp32emu_libretro.so'})},
    @{Platform='android-arm64';Info=$true;Strip=@('gp32emu_libretro_android.so');Copies=@(
        @{From=(Join-Path $AndroidArm64Build 'gp32emu_libretro_android.so');To='gp32emu_libretro_android.so'})},
    @{Platform='android-armv7';Info=$true;Strip=@('gp32emu_libretro_android.so');Copies=@(
        @{From=(Join-Path $AndroidArmv7Build 'gp32emu_libretro_android.so');To='gp32emu_libretro_android.so'})}
)
# Validate inputs before creating a partial distribution.
$required = @($SdlDll, $SdlLicense, $StripTool, $info) + $commonCopies.From
foreach ($package in $packages) { $required += $package.Copies.From }
foreach ($file in $required) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing input: $file" }
}
if (-not (Test-Path -LiteralPath $licenses -PathType Container)) { throw "Missing input directory: $licenses" }

$stage = Join-Path ([IO.Path]::GetTempPath()) "gp32emu-package-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Force $out | Out-Null
$sums = [Collections.Generic.List[string]]::new()
foreach ($package in $packages) {
    $name = "gp32emu-$version-$($package.Platform)"
    $root = Join-Path $stage $package.Platform
    New-Item -ItemType Directory -Force (Join-Path $root 'docs') | Out-Null
    foreach ($copy in @($package.Copies) + $commonCopies) {
        Copy-Item -LiteralPath $copy.From -Destination (Join-Path $root $copy.To)
    }
    if ($package.Info) { Copy-Item -LiteralPath $info -Destination $root }
    Copy-Item -LiteralPath $licenses -Destination $root -Recurse
    # Strip only the distribution copies; retain build symbols for diagnosis.
    foreach ($binary in $package.Strip) {
        & $StripTool --strip-debug (Join-Path $root $binary)
        if ($LASTEXITCODE -ne 0) { throw "Cannot strip debug information from $($package.Platform)/$binary" }
    }
    $manifest = [ordered]@{
        version=$version
        source_revision=$revision
        platform=$package.Platform
        files=@(Get-ChildItem -LiteralPath $root -File -Recurse | Sort-Object FullName | ForEach-Object {
            [ordered]@{
                path=[IO.Path]::GetRelativePath($root, $_.FullName).Replace('\','/')
                sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            }
        })
    }
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $root 'manifest.json') -Encoding utf8
    Compress-Archive -Path (Join-Path $root '*') -DestinationPath (Join-Path $out "$name.zip")
    $sums.Add("$((Get-FileHash -LiteralPath (Join-Path $out "$name.zip") -Algorithm SHA256).Hash.ToLowerInvariant())  $name.zip")
}
Set-Content -LiteralPath (Join-Path $out 'SHA256SUMS.txt') -Value $sums -Encoding utf8
