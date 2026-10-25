# Builds the runtime's third-party libraries (upstream\third_party, fetched by setup_deps.py) and installs them into
# upstream\build-deps\prefix, where the main build finds them. Only needs re-running when a pin changes.
$ErrorActionPreference = 'Stop'
# Run from the repository's tools\windows: the repository's parent holds toolchain\ (llvm-mingw, Ninja, CMake)
# and receives generated\, Play\ and dist\.
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$toolchain = Join-Path $root 'toolchain'
$upstream = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$third = Join-Path $upstream 'third_party'
$work = Join-Path $upstream 'build-deps'
$prefix = Join-Path $work 'prefix'
$python = (Get-Command python).Source
$git = Split-Path -Parent (Get-Command git).Source
$env:PATH = @((Join-Path $toolchain 'llvm-mingw\bin'), (Join-Path $toolchain 'Ninja'), (Join-Path $toolchain 'CMake\bin'),
    (Split-Path -Parent $python), $git, "$env:SystemRoot\System32") -join ';'

& $python (Join-Path $upstream 'tools\setup_deps.py')
if ($LASTEXITCODE) { throw 'setup_deps.py failed' }

function Build-Dep($name, $source, [string[]]$options) {
    Write-Host "== $name"
    $build = Join-Path $work $name
    cmake -S $source -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" `
        -DCMAKE_PREFIX_PATH="$prefix" -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang `
        -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++ @options
    if ($LASTEXITCODE) { throw "${name}: configure failed" }
    cmake --build $build --target install
    if ($LASTEXITCODE) { throw "${name}: build failed" }
    Remove-Item -Recurse -Force $build # installed into the prefix; disk space is tight
}

Build-Dep zlib (Join-Path $third 'zlib') @('-DZLIB_BUILD_EXAMPLES=OFF')
Build-Dep zstd (Join-Path $third 'zstd\build\cmake') @('-DZSTD_BUILD_PROGRAMS=OFF', '-DZSTD_BUILD_TESTS=OFF',
    '-DZSTD_BUILD_SHARED=OFF')
Build-Dep SDL (Join-Path $third 'SDL') @('-DSDL_TESTS=OFF', '-DSDL_EXAMPLES=OFF')
Build-Dep Vulkan-Headers (Join-Path $third 'Vulkan-Headers') @()
Build-Dep shaderc (Join-Path $third 'shaderc') @('-DSHADERC_SKIP_TESTS=ON', '-DSHADERC_SKIP_EXAMPLES=ON',
    '-DSHADERC_SKIP_EXECUTABLES=ON', '-DSHADERC_SKIP_COPYRIGHT_CHECK=ON', '-DSHADERC_ENABLE_WERROR_COMPILE=OFF',
    "-DSHADERC_GLSLANG_DIR=$third\glslang", "-DSHADERC_SPIRV_HEADERS_DIR=$third\spirv-headers",
    "-DSHADERC_SPIRV_TOOLS_DIR=$third\spirv-tools", '-DSPIRV_SKIP_TESTS=ON', '-DSPIRV_SKIP_EXECUTABLES=ON',
    '-DSPIRV_WERROR=OFF', '-DENABLE_GLSLANG_BINARIES=OFF')

# The Vulkan loader ships with the GPU driver; link against it through an import library made from its exports.
Write-Host '== vulkan-1 import library'
$exports = llvm-readobj --coff-exports "$env:SystemRoot\System32\vulkan-1.dll" |
    Select-String '^\s*Name: (vk\w+)' | ForEach-Object { $_.Matches[0].Groups[1].Value }
if (-not $exports) { throw 'no exports read from vulkan-1.dll' }
$def = Join-Path $work 'vulkan-1.def'
Set-Content -Encoding ascii $def (@('LIBRARY vulkan-1.dll', 'EXPORTS') + $exports)
llvm-dlltool -m i386:x86-64 -d $def -l (Join-Path $prefix 'lib\libvulkan-1.a')
if ($LASTEXITCODE) { throw 'llvm-dlltool failed' }
