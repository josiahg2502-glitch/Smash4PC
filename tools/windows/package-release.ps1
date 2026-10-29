# Packages Smash 4 PC for release: dist\Smash4PC-v<version>-Windows.zip, holding the launcher, both game programs,
# the DLLs, the launcher's pictures, the README and the third-party licenses. Nothing personal from Play\ goes in:
# not online\ (the console's files and account), save\, mods\, cache\, the .ini settings or the logs.
# Build first (tools\windows\build-smash.ps1); this takes the programs from the build folders.
$ErrorActionPreference = 'Stop'
# Run from the repository's tools\windows: the repository's parent holds toolchain\ (llvm-mingw, Ninja, CMake)
# and receives generated\, Play\ and dist\.
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$upstream = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$version = (Select-String -Path (Join-Path $upstream 'launcher\smash\main.cpp') -Pattern 'kVersion = "([^"]+)"').Matches[0].Groups[1].Value
$name = "Smash4PC-v$version-Windows"
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist $name
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item (Join-Path $upstream 'build\Smash 4 PC.exe'), (Join-Path $upstream 'build\smash.exe') $stage
Copy-Item (Join-Path $upstream 'build-base\smash.exe') (Join-Path $stage 'smash-base.exe')
Copy-Item (Join-Path $upstream 'build-compat\smash-compat.exe') $stage
Copy-Item (Join-Path $upstream 'build-deps\prefix\bin\*.dll') $stage
Copy-Item (Join-Path $upstream 'assets\smash\logo.png') (Join-Path $stage 'title-logo.png')
Copy-Item (Join-Path $upstream 'assets\smash\launcher-background.jpg') $stage
Copy-Item (Join-Path $upstream 'assets\smash\README.txt') $stage
Copy-Item (Join-Path $upstream 'LICENSE') (Join-Path $stage 'LICENSE.txt')
New-Item -ItemType Directory -Force (Join-Path $stage 'mods') | Out-Null

# Third-party licenses, and the MPL-covered Cemu code's source (MPL 2.0 asks for it with the program).
$licenses = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Force $licenses | Out-Null
$third = Join-Path $upstream 'third_party'
@{ 'SDL3.txt' = 'SDL\LICENSE.txt'; 'Vulkan-Headers.md' = 'Vulkan-Headers\LICENSE.md'; 'shaderc.txt' = 'shaderc\LICENSE';
   'glslang.txt' = 'glslang\LICENSE.txt'; 'SPIRV-Tools.txt' = 'spirv-tools\LICENSE'; 'SPIRV-Headers.txt' = 'spirv-headers\LICENSE';
   'zlib.txt' = 'zlib\LICENSE'; 'zstd.txt' = 'zstd\LICENSE'; 'ZArchive.txt' = 'ZArchive\LICENSE'; 'addrlib.txt' = 'addrlib\LICENSE';
   'Cemu-NEX-MPL-2.0.txt' = 'cemu_nex\LICENSE.txt' }.GetEnumerator() | ForEach-Object {
    Copy-Item (Join-Path $third $_.Value) (Join-Path $licenses $_.Key)
}
Set-Content -Encoding utf8 (Join-Path $licenses 'stb.txt') 'stb_image and stb_truetype (Sean Barrett): public domain or MIT, at your choice (see the headers).'
Copy-Item -Recurse (Join-Path $third 'cemu_nex') (Join-Path $licenses 'cemu_nex-source')

$zip = Join-Path $dist "$name.zip"
Remove-Item -Force $zip -ErrorAction SilentlyContinue
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal # (one top folder, as unzipped)
Remove-Item -Recurse -Force $stage
"$zip ($([math]::Round((Get-Item $zip).Length / 1MB)) MB)"
