#Requires -Version 7.0
param(
    [Parameter(Mandatory)][string]$WindowsBuild,
    [Parameter(Mandatory)][string]$LinuxArm64Build,
    [Parameter(Mandatory)][string]$AndroidArm64Build,
    [Parameter(Mandatory)][string]$AndroidArmv7Build,
    [Parameter(Mandatory)][string]$SdlDll,
    [Parameter(Mandatory)][string]$SdlLicense,
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
if ((Test-Path -LiteralPath $out) -or (Test-Path -LiteralPath "$out.zip")) {
    throw 'Output already exists; choose a new directory.'
}
$targets = @(
    @{Name='windows-x64';Build=$WindowsBuild;Files=@('gp32emu_win64.exe','gp32emu_libretro.dll')},
    @{Name='linux-aarch64';Build=$LinuxArm64Build;Files=@('gp32emu_libretro.so')},
    @{Name='android-arm64';Build=$AndroidArm64Build;Files=@('gp32emu_libretro_android.so')},
    @{Name='android-armv7';Build=$AndroidArmv7Build;Files=@('gp32emu_libretro_android.so')}
)
# Validate inputs before creating a partial distribution.
$required = @($SdlDll, $SdlLicense, "$repo/docs/RELEASE-$version.md")
foreach ($target in $targets) {
    foreach ($file in $target.Files) { $required += Join-Path $target.Build $file }
}
foreach ($file in $required) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing input: $file" }
}
foreach ($target in $targets) {
    $dest = Join-Path $out $target.Name
    New-Item -ItemType Directory -Force $dest | Out-Null
    foreach ($file in $target.Files) { Copy-Item -LiteralPath (Join-Path $target.Build $file) -Destination $dest }
    Copy-Item -LiteralPath "$repo/gp32emu_libretro.info" -Destination $dest
}
Copy-Item -LiteralPath $SdlDll -Destination "$out/windows-x64/SDL3.dll"
Copy-Item -LiteralPath $SdlLicense -Destination "$out/windows-x64/SDL3-LICENSE.txt"
Copy-Item -LiteralPath "$repo/README.md" -Destination $out
Copy-Item -LiteralPath "$repo/docs/RELEASE-$version.md" -Destination "$out/RELEASE-NOTES.md"
foreach ($doc in @('MANUAL.ko.md','ANDROID_BUILD.md','CPU_SPEED_OPTION.md','DEVELOPMENT_STATUS.md','libretro/README_LIBRETRO.md')) {
    $dest = Join-Path $out "docs/$doc"
    New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
    Copy-Item -LiteralPath "$repo/docs/$doc" -Destination $dest
}
Copy-Item -LiteralPath "$repo/licenses" -Destination $out -Recurse
$manifest = [ordered]@{
    version=$version
    source_revision=$revision
    files=@(Get-ChildItem -LiteralPath $out -File -Recurse | Sort-Object FullName | ForEach-Object {
        [ordered]@{
            path=[IO.Path]::GetRelativePath($out, $_.FullName).Replace('\','/')
            sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$out/manifest.json" -Encoding utf8
Compress-Archive -Path "$out/*" -DestinationPath "$out.zip"
Get-FileHash -LiteralPath "$out.zip" -Algorithm SHA256
