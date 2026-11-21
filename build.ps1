# CorridorKey 2 for Resolve - Windows x64 build
# Licence: CC BY-NC-SA 4.0 with the CorridorKey additional terms (see LICENSE).
param([switch]$DependenciesOnly)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$build = Join-Path $root 'build\windows'
$deps = Join-Path $build 'deps'
$prefix = Join-Path $deps 'install'

function Invoke-Native([string]$program, [string[]]$arguments) {
    & $program @arguments
    if ($LASTEXITCODE -ne 0) { throw "$program failed with exit code $LASTEXITCODE" }
}

function Get-Source([string]$name, [string]$url, [string]$sha256) {
    $archive = Join-Path $deps "$name.tar.gz"
    if (!(Test-Path -LiteralPath $archive)) {
        Invoke-Native 'curl.exe' @('--fail', '--location', '--silent', '--show-error', '--output', $archive, $url)
    }
    if ((Get-FileHash -LiteralPath $archive).Hash -ne $sha256) { throw "$name archive SHA-256 mismatch" }
    $source = Join-Path $deps $name
    New-Item -ItemType Directory -Path $source -Force | Out-Null
    # OpenEXR's fuzz-test symlink is unused and Windows can require elevation to create it.
    Invoke-Native 'tar.exe' @('-xzf', $archive, '-C', $source, '--strip-components', '1', '--exclude=*/oss-fuzz')
}

New-Item -ItemType Directory -Path $deps -Force | Out-Null
$stamp = Join-Path $prefix 'windows-static-3.5.2-3.2.3-trimpaths'
if (!(Test-Path -LiteralPath $stamp)) {
    Get-Source 'imath' 'https://github.com/AcademySoftwareFoundation/Imath/archive/refs/tags/v3.2.3.tar.gz' 'e10c12b3f21f45bf08e09d4215d9c7691368d747beebd840de0b6fefed2df9f8'
    Get-Source 'openexr' 'https://github.com/AcademySoftwareFoundation/openexr/archive/refs/tags/v3.5.2.tar.gz' '85a291b9b8563fabef1aa3f6e912d744ce1d4b9fd5a8f978eda194cd51b15ae9'
    # OpenJPH's diagnostic macros embed __FILE__. Trim the builder's source root.
    $trim = '/d1trimfile:"' + $root + '"'
    $common = @('-A', 'x64', "-DCMAKE_INSTALL_PREFIX=$prefix", "-DCMAKE_CXX_FLAGS=$trim",
                '-DBUILD_SHARED_LIBS=OFF', '-DBUILD_TESTING=OFF')
    Invoke-Native 'cmake' (@('-S', "$deps/imath", '-B', "$deps/build-imath") + $common +
        @('-DIMATH_INSTALL_PKG_CONFIG=OFF', '-DPYTHON=OFF'))
    Invoke-Native 'cmake' @('--build', "$deps/build-imath", '--config', 'Release', '--target', 'install', '--parallel', '4')
    Invoke-Native 'cmake' (@('-S', "$deps/openexr", '-B', "$deps/build-openexr") + $common +
        @("-DImath_DIR=$prefix/lib/cmake/Imath", '-DOPENEXR_FORCE_INTERNAL_DEFLATE=ON',
          '-DOPENEXR_BUILD_TOOLS=OFF', '-DOPENEXR_INSTALL_TOOLS=OFF', '-DOPENEXR_BUILD_EXAMPLES=OFF',
          '-DOPENEXR_INSTALL_PKG_CONFIG=OFF', '-DOPENEXR_TEST_LIBRARIES=OFF', '-DOPENEXR_TEST_TOOLS=OFF'))
    Invoke-Native 'cmake' @('--build', "$deps/build-openexr", '--config', 'Release', '--target', 'install', '--parallel', '4')
    Copy-Item -LiteralPath "$deps/openexr/LICENSE.md" -Destination "$prefix/OpenEXR-LICENSE.md"
    Copy-Item -LiteralPath "$deps/imath/LICENSE.md" -Destination "$prefix/Imath-LICENSE.md"
    Copy-Item -LiteralPath "$deps/openexr/external/OpenJPH/LICENSE" -Destination "$prefix/OpenJPH-LICENSE.txt"
    Copy-Item -LiteralPath "$deps/openexr/external/deflate/COPYING" -Destination "$prefix/libdeflate-COPYING.txt"
    New-Item -ItemType File -Path $stamp | Out-Null
}
if ($DependenciesOnly) { return }

Invoke-Native 'cmake' @('-S', $root, '-B', $build, '-A', 'x64', "-DCK2_DEPS=$prefix")
Invoke-Native 'cmake' @('--build', $build, '--config', 'Release', '--parallel', '4')
Invoke-Native (Join-Path $build 'Release\ck2_tests.exe') @()

$contents = Join-Path $build 'CorridorKey2.ofx.bundle\Contents'
$win64 = Join-Path $contents 'Win64'
$resources = Join-Path $contents 'Resources'
New-Item -ItemType Directory -Force -Path $win64, "$resources\worker", "$resources\setup\bin", "$resources\licenses" | Out-Null
Copy-Item -LiteralPath "$build\Release\CorridorKey2.ofx" -Destination $win64 -Force
Copy-Item -LiteralPath "$root\worker\ck2_worker.py", "$root\worker\ck2_mps.py", "$root\worker\ck2_progress.py" -Destination "$resources\worker" -Force
Copy-Item -LiteralPath "$root\setup\ck2_setup.ps1", "$root\setup\requirements-windows.txt",
    "$root\setup\corridorkey_files.sha256", "$root\setup\weights.sha256" -Destination "$resources\setup" -Force
Copy-Item -LiteralPath "$root\setup\wheels" -Destination "$resources\setup" -Recurse -Force
Copy-Item -LiteralPath "$root\installer\com.creativevideotips.corridorkey2.png" -Destination $resources -Force
Copy-Item -LiteralPath "$root\LICENSE", "$root\THIRD_PARTY_NOTICES.md", "$prefix\OpenEXR-LICENSE.md",
    "$prefix\Imath-LICENSE.md", "$prefix\OpenJPH-LICENSE.txt", "$prefix\libdeflate-COPYING.txt" -Destination "$resources\licenses" -Force
Copy-Item -LiteralPath "$root\third_party\openfx\LICENSE.md" -Destination "$resources\licenses\OpenFX-LICENSE.md" -Force

$uvVersion = '0.12.18'
$uvArchive = Join-Path $deps "uv-$uvVersion-windows.zip"
if (!(Test-Path -LiteralPath $uvArchive)) {
    Invoke-Native 'curl.exe' @('--fail', '--location', '--silent', '--show-error', '--output', $uvArchive,
        "https://github.com/astral-sh/uv/releases/download/$uvVersion/uv-x86_64-pc-windows-msvc.zip")
}
if ((Get-FileHash -LiteralPath $uvArchive).Hash -ne 'cae6a3bc25239f83dffb467a4b180508d9da23986c04639ebfa44e43e6a84bff') {
    throw 'uv archive SHA-256 mismatch'
}
Invoke-Native 'tar.exe' @('-xf', $uvArchive, '-C', "$resources\setup\bin", 'uv.exe')
foreach ($name in 'LICENSE-MIT','LICENSE-APACHE') {
    $licence = Join-Path "$resources\licenses" "uv-$name"
    Invoke-Native 'curl.exe' @('--fail', '--location', '--silent', '--show-error', '--output', $licence,
        "https://raw.githubusercontent.com/astral-sh/uv/$uvVersion/$name")
}
Write-Host "Built $contents"
