#Requires -Version 7.0
param(
    [Parameter(Mandatory)][string]$WindowsBuild,
    [Parameter(Mandatory)][string]$LinuxArm64Build,
    [Parameter(Mandatory)][string]$AndroidArm64Build,
    [Parameter(Mandatory)][string]$AndroidArmv7Build,
    [Parameter(Mandatory)][string]$SdlDll,
    [Parameter(Mandatory)][string]$SdlLicense,
    [Parameter(Mandatory)][string]$StripTool,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$AndroidApk,
    [string]$IosIpa
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$version = '1.1.0'
$revision = (git -C $repo rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify source revision.' }
$dirty = git -C $repo status --porcelain
if ($LASTEXITCODE -ne 0 -or $dirty) { throw 'Commit reviewed source before packaging.' }
$out = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $out) { throw 'Output already exists; choose a new directory.' }

$info = Join-Path $repo 'gp32emu_libretro.info'
$licenses = Join-Path $repo 'licenses'
# Preserve document paths: moving release notes to the root breaks their links.
$documentPaths = @(
    'README.md', 'docs/MANUAL.ko.md', "docs/RELEASE-$version.md",
    'docs/RELEASE-1.0.1.md', 'docs/BUILDING.md', 'docs/ANDROID_BUILD.md',
    'docs/IOS_BUILD.md', 'ios/README.md', 'ios/CREDITS.md'
)
$commonCopies = @($documentPaths | Select-Object -Unique | ForEach-Object {
    @{From=(Join-Path $repo $_);To=$_}
})
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
if ($AndroidApk) { $required += $AndroidApk }
if ($IosIpa) { $required += $IosIpa }
foreach ($file in $required) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing input: $file" }
}
if (-not (Test-Path -LiteralPath $licenses -PathType Container)) { throw "Missing input directory: $licenses" }

# Public iOS downloads must not contain the publisher's device signature or
# provisioning profile. Check the Mach-O too: a missing directory alone does
# not prove that the executable has no embedded code signature.
if ($IosIpa) {
    $archive = [IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($IosIpa))
    try {
        foreach ($entry in $archive.Entries) {
            if ($entry.FullName -match '(^|/)_CodeSignature(/|$)|\.mobileprovision$') {
                throw 'The IPA contains signing/provisioning data; use the unsigned public build.'
            }
        }
        $binary = $archive.GetEntry('Payload/GP32emu.app/GP32emu')
        if (-not $binary) { throw 'The IPA does not contain GP32emu.' }
        $reader = [IO.BinaryReader]::new($binary.Open())
        try {
            $header = $reader.ReadBytes(32)
            if ($header.Length -ne 32 -or [BitConverter]::ToUInt32($header, 0) -ne 0xfeedfacfL -or
                [BitConverter]::ToUInt32($header, 4) -ne 0x0100000c) {
                throw 'Expected a single-architecture ARM64 iOS executable.'
            }
            $commands = [BitConverter]::ToUInt32($header, 16)
            $remaining = [long][BitConverter]::ToUInt32($header, 20)
            if ($commands -gt 4096 -or $remaining -gt $binary.Length - 32) { throw 'Invalid Mach-O header.' }
            for ($index = 0; $index -lt $commands; $index++) {
                if ($remaining -lt 8) { throw 'Truncated Mach-O commands.' }
                $command = $reader.ReadUInt32()
                $size = $reader.ReadUInt32()
                if ($size -lt 8 -or $size -gt $remaining) { throw 'Invalid Mach-O command size.' }
                if ($command -eq 0x1d) { throw 'The IPA executable is signed; remove its signature before packaging.' }
                if ($reader.ReadBytes([int]($size - 8)).Length -ne $size - 8) { throw 'Truncated Mach-O command.' }
                $remaining -= $size
            }
        } finally { $reader.Dispose() }
    } finally { $archive.Dispose() }
}

$stage = Join-Path ([IO.Path]::GetTempPath()) "gp32emu-package-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Force $out | Out-Null
$sums = [Collections.Generic.List[string]]::new()
foreach ($package in $packages) {
    $name = "gp32emu-$version-$($package.Platform)"
    $root = Join-Path $stage $package.Platform
    New-Item -ItemType Directory -Force (Join-Path $root 'docs') | Out-Null
    foreach ($copy in @($package.Copies) + $commonCopies) {
        $destination = Join-Path $root $copy.To
        New-Item -ItemType Directory -Force (Split-Path -Parent $destination) | Out-Null
        Copy-Item -LiteralPath $copy.From -Destination $destination
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

# Standalone mobile apps remain independent downloads, not nested in core ZIPs.
foreach ($mobile in @(
    @{Source=$AndroidApk;Name="gp32emu-$version-android.apk"},
    @{Source=$IosIpa;Name="gp32emu-$version-ios-unsigned.ipa"}
)) {
    if (-not $mobile.Source) { continue }
    $destination = Join-Path $out $mobile.Name
    Copy-Item -LiteralPath $mobile.Source -Destination $destination
    $sums.Add("$((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant())  $($mobile.Name)")
}
$sourceName = "gp32emu-$version-source.zip"
& git -C $repo archive --format=zip "--prefix=gp32emu-$version/" "--output=$(Join-Path $out $sourceName)" HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot package committed source.' }
$sums.Add("$((Get-FileHash -LiteralPath (Join-Path $out $sourceName) -Algorithm SHA256).Hash.ToLowerInvariant())  $sourceName")
Set-Content -LiteralPath (Join-Path $out 'SHA256SUMS.txt') -Value $sums -Encoding utf8
