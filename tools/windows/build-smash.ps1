# Builds the Smash port: dependencies (once), cafe-recomp, the translated cross_f.rpx files, the game programs and
# the launcher. Output in Play\: "Smash 4 PC.exe" (the launcher), smash.exe (built from the update's v304 code),
# smash-base.exe (from the game's own v1.0 code, to play without the update), the DLLs, the logo and background.
param([string]$Rpx = 'N:\ROMS\Wii U\Super Smash Bros for Wii U [Update] [0005000e10144f00]\code\cross_f.rpx',
      [string]$BaseRpx = 'N:\ROMS\Wii U\Super Smash Bros for Wii U [Game] [0005000010144f00]\code\cross_f.rpx')
$ErrorActionPreference = 'Stop'
# Run from the repository's tools\windows: the repository's parent holds toolchain\ (llvm-mingw, Ninja, CMake)
# and receives generated\, Play\ and dist\.
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$toolchain = Join-Path $root 'toolchain'
$upstream = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$build = Join-Path $upstream 'build'
$prefix = Join-Path $upstream 'build-deps\prefix'
$play = Join-Path $root 'Play'

if (-not (Test-Path (Join-Path $prefix 'lib\libvulkan-1.a'))) {
    & (Join-Path $PSScriptRoot 'build-deps.ps1')
}
& (Join-Path $PSScriptRoot 'build-recomp.ps1') # also configures $build
$env:PATH = @((Join-Path $toolchain 'llvm-mingw\bin'), (Join-Path $toolchain 'Ninja'), (Join-Path $toolchain 'CMake\bin'),
    "$env:SystemRoot\System32") -join ';'

# One program per code version: the update's in build\, the game's own in build-base\.
function Build-Game([string]$rpx, [string]$generated, [string]$dir, [string[]]$targets) {
    & (Join-Path $build 'recomp\cafe-recomp.exe') $rpx $generated --any-rpx
    if ($LASTEXITCODE) { throw "cafe-recomp failed on $rpx" }
    cmake -S $upstream -B $dir -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang `
        -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++ -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres `
        -DCMAKE_EXE_LINKER_FLAGS=-static "-DSMASH_GENERATED_DIR=$generated"
    if ($LASTEXITCODE) { throw 'cmake configure failed' }
    ninja -C $dir @targets
    if ($LASTEXITCODE) { throw 'build failed' }
}
Build-Game $Rpx (Join-Path $root 'generated\smash') $build @('smash', 'smash-launcher')
New-Item -ItemType Directory -Force $play | Out-Null
Copy-Item (Join-Path $build 'smash.exe'), (Join-Path $build 'Smash 4 PC.exe') $play
if (Test-Path $BaseRpx) {
    $baseBuild = Join-Path $upstream 'build-base'
    Build-Game $BaseRpx (Join-Path $root 'generated\smash-base') $baseBuild @('smash')
    Copy-Item (Join-Path $baseBuild 'smash.exe') (Join-Path $play 'smash-base.exe')
}
Copy-Item (Join-Path $prefix 'bin\*.dll') $play
Copy-Item (Join-Path $upstream 'assets\smash\logo.png') (Join-Path $play 'title-logo.png')
Copy-Item (Join-Path $upstream 'assets\smash\launcher-background.jpg') $play
New-Item -ItemType Directory -Force (Join-Path $play 'mods') | Out-Null
