# Builds cafe-recomp and cafe-census (upstream\build\recomp) with the project's portable toolchain, then runs the
# function finder self-test.
$ErrorActionPreference = 'Stop'
# Run from the repository's tools\windows: the repository's parent holds toolchain\ (llvm-mingw, Ninja, CMake)
# and receives generated\, Play\ and dist\.
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$toolchain = Join-Path $root 'toolchain'
$upstream = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$python = (Get-Command python).Source
$git = Split-Path -Parent (Get-Command git).Source # setup_deps.py runs git
$env:PATH = @((Join-Path $toolchain 'llvm-mingw\bin'), (Join-Path $toolchain 'Ninja'), (Join-Path $toolchain 'CMake\bin'),
    $git, "$env:SystemRoot\System32") -join ';'

& $python (Join-Path $upstream 'tools\setup_deps.py')
if ($LASTEXITCODE) { throw 'setup_deps.py failed' }
cmake -S $upstream -B (Join-Path $upstream 'build') -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++ `
    -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres `
    -DCMAKE_EXE_LINKER_FLAGS=-static
if ($LASTEXITCODE) { throw 'cmake configure failed' }
ninja -C (Join-Path $upstream 'build') cafe-recomp cafe-census function_finder_test
if ($LASTEXITCODE) { throw 'build failed' }
& (Join-Path $upstream 'build\recomp\function_finder_test.exe')
if ($LASTEXITCODE) { throw 'function_finder_test failed' }
