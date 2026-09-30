# Builds the protobuf submodule for the hand-maintained Visual Studio solution and installs it to
#   third_party\_install\protobuf\<Configuration>\   (bin\protoc.exe, lib\libprotobuf(d).lib, include\)
# Run once on a new machine, and again after changing the protobuf submodule version.
# The CMake presets do not need this: they build protobuf from source as a subproject.
param(
    [string[]]$Configurations = @('Debug', 'Release'),
    [string]$Generator = 'Visual Studio 17 2022',
    [string]$Toolset = 'v143'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root 'third_party\protobuf'
$build = Join-Path $root 'third_party\_build\protobuf'

if (-not (Test-Path (Join-Path $src 'CMakeLists.txt'))) {
    throw "protobuf submodule is missing. Run: git submodule update --init"
}

foreach ($cfg in $Configurations) {
    $prefix = Join-Path $root "third_party\_install\protobuf\$cfg"
    Write-Host "=== protobuf $cfg -> $prefix"

    cmake -S $src -B $build -G $Generator -A x64 -T $Toolset `
        "-DCMAKE_INSTALL_PREFIX=$prefix" `
        -Dprotobuf_BUILD_TESTS=OFF `
        -Dprotobuf_WITH_ZLIB=OFF `
        -Dprotobuf_BUILD_SHARED_LIBS=OFF `
        -Dprotobuf_MSVC_STATIC_RUNTIME=OFF
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($cfg)" }

    cmake --build $build --config $cfg --target install --parallel
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($cfg)" }

    & (Join-Path $prefix 'bin\protoc.exe') --version
}
