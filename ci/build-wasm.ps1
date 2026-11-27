# ci/build-wasm.ps1 -- WebAssembly Live Playground (Mod A) Release build, CSP/Brotli/license
# verification, and deploy-receive.sh `demo` archive packager (Windows host).
#
#   $env:EMSDK = 'C:\emsdk'
#   .\ci\build-wasm.ps1 [-Version 2.1.2] [-Preset wasm-multi] [-Mode playground]
#
# Standalone validation modes:
#   .\ci\build-wasm.ps1 -ValidateStageOnly <stage_dir>
#   .\ci\build-wasm.ps1 -VerifyArchiveOnly <archive.tar.gz>
#
# Produces in dist\wasm\:
#   1. <product id>-wasm-demo.tar.gz         (ready for `ssh deploy@<host> demo < ...`)
#   2. <product id>-wasm-demo.tar.gz.sha256  (SHA-256 digest)

param(
    [string]$Version = '',
    [string]$Preset = '',
    [string]$Mode = '',
    [string]$OutputDir = '',
    [string]$ValidateStageOnly = '',
    [string]$VerifyArchiveOnly = ''
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot

# Single-source product brand (cmake/RagoShellDefaults.cmake, docs/Brand.md). Never read ci/.env.
$brandPath = Join-Path (Join-Path $root 'cmake') 'RagoShellDefaults.cmake'
$brandText = Get-Content -LiteralPath $brandPath -Raw
function Get-Brand([string]$Key) {
    if ($brandText -match "set\($Key\s+`"([^`"]+)`"\)") { return $Matches[1] }
    throw "$Key not found in cmake/RagoShellDefaults.cmake"
}
$productName = Get-Brand 'RAGO_PRODUCT_NAME'
$productId   = Get-Brand 'RAGO_PRODUCT_ID'

$cmakeListsPath = Join-Path $root 'CMakeLists.txt'
$cmakeText = Get-Content -LiteralPath $cmakeListsPath -Raw
$vMaj = if ($cmakeText -match 'PROJECT_VERSION_MAJOR\s+"(\d+)"') { $Matches[1] } else { '2' }
$vMin = if ($cmakeText -match 'PROJECT_VERSION_MINOR\s+"(\d+)"') { $Matches[1] } else { '1' }
$vPat = if ($cmakeText -match 'PROJECT_VERSION_PATCH\s+"(\d+)"') { $Matches[1] } else { '0' }
$vBld = if ($cmakeText -match 'PROJECT_VERSION_BUILD\s+"([^"]*)"') { $Matches[1] } else { '' }
$defaultVersion = "$vMaj.$vMin.$vPat$vBld"

$allowedRagoWasm = @(
    'RAGO_WASM_PRESET',
    'RAGO_WASM_MODE',
    'RAGO_WASM_OUTPUT_DIR'
)
foreach ($entry in (Get-ChildItem Env:)) {
    if ($entry.Name.StartsWith('RAGO_WASM_', [System.StringComparison]::OrdinalIgnoreCase)) {
        $matched = $false
        foreach ($allowed in $allowedRagoWasm) {
            if ([string]::Equals($entry.Name, $allowed, [System.StringComparison]::OrdinalIgnoreCase)) {
                $matched = $true
                break
            }
        }
        if (-not $matched) {
            throw "Unrecognized WASM environment variable '$($entry.Name)'."
        }
    }
}

if (-not $Version) { $Version = $defaultVersion }
if ($Version -match '^[vV](?=\d)') { $Version = $Version.Substring(1) }
if ($Version -notmatch '^[A-Za-z0-9][A-Za-z0-9._+-]{0,63}$' -or $Version.Contains('..')) {
    throw 'Invalid version string (allowed: alphanumeric, ., _, +, -; no slashes or "..").'
}

if (-not $Preset) {
    $Preset = if ($env:RAGO_WASM_PRESET) { $env:RAGO_WASM_PRESET } else { 'wasm-multi' }
}
if ($Preset -ne 'wasm-multi' -and $Preset -ne 'wasm-single') {
    throw "Unsupported WASM preset '$Preset' (expected 'wasm-multi' or 'wasm-single')."
}

if (-not $Mode) {
    $Mode = if ($env:RAGO_WASM_MODE) { $env:RAGO_WASM_MODE } else { 'playground' }
}
if ($Mode -ne 'playground' -and $Mode -ne 'gateway') {
    throw "Unsupported WASM mode '$Mode' (expected 'playground' or 'gateway')."
}

if (-not $OutputDir) {
    $OutputDir = if ($env:RAGO_WASM_OUTPUT_DIR) { $env:RAGO_WASM_OUTPUT_DIR } else { Join-Path (Join-Path $root 'dist') 'wasm' }
}

if ($ValidateStageOnly -and $VerifyArchiveOnly) {
    throw '-ValidateStageOnly and -VerifyArchiveOnly are mutually exclusive.'
}

$nodeCmd = Get-Command node -ErrorAction SilentlyContinue
if (-not $nodeCmd) {
    throw "Required tool 'node' was not found on PATH."
}
$nodeExe = $nodeCmd.Source
$packagerScript = Join-Path (Join-Path $root 'ci') 'package-wasm-demo.mjs'

$threadArgs = @()
if ($Preset -eq 'wasm-multi') {
    $threadArgs += '--require-threads'
}

if ($ValidateStageOnly) {
    & $nodeExe $packagerScript --validate-stage-only $ValidateStageOnly @threadArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    exit 0
}

if ($VerifyArchiveOnly) {
    & $nodeExe $packagerScript --verify-archive-only $VerifyArchiveOnly
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    exit 0
}

# Resolve Visual Studio CMake, Ninja, and bundled vcpkg when not already on PATH.
$vsInstallPath = ''
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path -LiteralPath $vswhere) {
    $vsInstallPath = (& $vswhere -latest -products * -property installationPath 2>$null | Select-Object -First 1)
}

$cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmakeCmd -and $vsInstallPath) {
    $vsCmakeDir = Join-Path $vsInstallPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
    if (Test-Path -LiteralPath (Join-Path $vsCmakeDir 'cmake.exe')) {
        $env:PATH = "$vsCmakeDir;$env:PATH"
        $cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
    }
}
if (-not $cmakeCmd) {
    throw "Required tool 'cmake' was not found on PATH."
}

$ninjaCmd = Get-Command ninja -ErrorAction SilentlyContinue
if (-not $ninjaCmd -and $vsInstallPath) {
    $vsNinjaDir = Join-Path $vsInstallPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
    if (Test-Path -LiteralPath (Join-Path $vsNinjaDir 'ninja.exe')) {
        $env:PATH = "$vsNinjaDir;$env:PATH"
        $ninjaCmd = Get-Command ninja -ErrorAction SilentlyContinue
    }
}
if (-not $ninjaCmd) {
    throw "Required tool 'ninja' was not found on PATH."
}

if (-not $env:EMSDK) {
    throw "EMSDK is not set (install Emscripten 5.0.5 via emsdk and set `$env:EMSDK = 'C:\path\to\emsdk')."
}
$emToolchain = Join-Path $env:EMSDK 'upstream\emscripten\cmake\Modules\Platform\Emscripten.cmake'
$emccBat = Join-Path $env:EMSDK 'upstream\emscripten\emcc.bat'
$emConfig = Join-Path $env:EMSDK '.emscripten'
if (-not (Test-Path -LiteralPath $emToolchain) -or -not (Test-Path -LiteralPath $emccBat)) {
    throw "Emscripten toolchain not found under EMSDK=$($env:EMSDK) (expected $emToolchain)."
}
if (-not (Test-Path -LiteralPath $emConfig)) {
    throw "$emConfig is missing (run: emsdk install 5.0.5 && emsdk activate 5.0.5)."
}
$emBinDir = Join-Path $env:EMSDK 'upstream\emscripten'
$env:PATH = "$emBinDir;$($env:EMSDK);$env:PATH"

if (-not $env:VCPKG_ROOT) {
    if ($env:VCPKG_INSTALLATION_ROOT -and (Test-Path -LiteralPath (Join-Path $env:VCPKG_INSTALLATION_ROOT 'scripts\buildsystems\vcpkg.cmake'))) {
        $env:VCPKG_ROOT = $env:VCPKG_INSTALLATION_ROOT
    } elseif ($vsInstallPath -and (Test-Path -LiteralPath (Join-Path $vsInstallPath 'VC\vcpkg\scripts\buildsystems\vcpkg.cmake'))) {
        $env:VCPKG_ROOT = Join-Path $vsInstallPath 'VC\vcpkg'
    }
}
if (-not $env:VCPKG_ROOT -or -not (Test-Path -LiteralPath (Join-Path $env:VCPKG_ROOT 'scripts\buildsystems\vcpkg.cmake'))) {
    throw "VCPKG_ROOT is not set and could not be auto-detected."
}

if (-not $env:Qt6_DIR) {
    if (Test-Path -LiteralPath 'C:\Qt\6.12.0\msvc2022_64') {
        $env:Qt6_DIR = 'C:\Qt\6.12.0\msvc2022_64'
    }
}
if (-not $env:Qt6_DIR -or -not (Test-Path -LiteralPath (Join-Path $env:Qt6_DIR 'bin')) -or (Test-Path -LiteralPath (Join-Path $env:Qt6_DIR 'Qt6Config.cmake'))) {
    throw "Qt6_DIR must point to the host desktop Qt 6 prefix root containing bin\ (e.g. C:\Qt\6.12.0\msvc2022_64, not lib\cmake\Qt6)."
}

if (-not $env:QT_WASM_PATH) {
    $qtParent = Split-Path -Parent $env:Qt6_DIR
    $wasmArch = if ($Preset -eq 'wasm-multi') { 'wasm_multithread' } else { 'wasm_singlethread' }
    $env:QT_WASM_PATH = Join-Path $qtParent $wasmArch
}
if (-not (Test-Path -LiteralPath (Join-Path $env:QT_WASM_PATH 'lib\cmake\Qt6'))) {
    throw "QT_WASM_PATH ($($env:QT_WASM_PATH)) does not contain lib\cmake\Qt6."
}

# Normalize forward slashes for CMakePresets.json $env{...} expansion on Windows.
$env:EMSDK       = $env:EMSDK.Replace('\', '/')
$env:VCPKG_ROOT  = $env:VCPKG_ROOT.Replace('\', '/')
$env:Qt6_DIR     = $env:Qt6_DIR.Replace('\', '/')
$env:QT_WASM_PATH = $env:QT_WASM_PATH.Replace('\', '/')

# Pre-flight Emscripten version check against Qt's baked-in QT_EMCC_VERSION.
$qconfigPath = Join-Path ($env:QT_WASM_PATH.Replace('/', '\')) 'include\QtCore\qconfig.h'
$expectedEmcc = ''
if (Test-Path -LiteralPath $qconfigPath) {
    $qconfigText = Get-Content -LiteralPath $qconfigPath -Raw
    if ($qconfigText -match '#define\s+QT_EMCC_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"') {
        $expectedEmcc = $Matches[1]
    }
}
$emccOut = (& $emccBat --version 2>&1 | Out-String)
$actualEmcc = if ($emccOut -match '(?im)emcc\b[^\r\n]*?\b(\d+\.\d+\.\d+)\b') {
    $Matches[1]
} elseif ($emccOut -match '\b(\d+\.\d+\.\d+)\b') {
    $Matches[1]
} else {
    ''
}
if ($expectedEmcc -and $actualEmcc -ne $expectedEmcc) {
    throw "Qt WASM ($($env:QT_WASM_PATH)) requires Emscripten $expectedEmcc, but emcc reports '$actualEmcc'. Run: emsdk install $expectedEmcc && emsdk activate $expectedEmcc"
}

$ciTempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("$productId-wasm-ci-" + [System.Guid]::NewGuid().ToString('N').Substring(0, 10))
New-Item -ItemType Directory -Force -Path $ciTempDir | Out-Null

try {
    Write-Host "==> Building $productName WebAssembly ($Preset, mode=$Mode, version=$Version, emcc=$actualEmcc)"

    Write-Host '==> Checking translations & source license notices'
    & $nodeExe (Join-Path (Join-Path $root 'ci') 'check-i18n.mjs')
    if ($LASTEXITCODE -ne 0) { throw 'check-i18n.mjs failed.' }

    & $nodeExe (Join-Path (Join-Path $root 'ci') 'generate-sbom.mjs') --verify-source-licenses
    if ($LASTEXITCODE -ne 0) { throw 'generate-sbom.mjs --verify-source-licenses failed.' }

    Push-Location $root
    try {
        Write-Host "==> Configuring CMake preset '$Preset'"
        & $cmakeCmd.Source --preset $Preset "-DRAGO_WASM_MODE=$Mode"
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for preset '$Preset'." }

        Write-Host "==> Compiling target '$productId' and WASM loader/Brotli assets"
        & $cmakeCmd.Source --build --preset $Preset --target $productId "${productId}_wasm_assets"
        if ($LASTEXITCODE -ne 0) { throw "CMake build failed for target '$productId'." }
    } finally {
        Pop-Location
    }

    $buildDir = Join-Path (Join-Path $root 'build') $Preset
    $appBinDir = Join-Path (Join-Path $buildDir 'src') "$productId-app"
    $stageDir = Join-Path $ciTempDir 'stage'
    New-Item -ItemType Directory -Force -Path $stageDir | Out-Null

    Write-Host '==> Installing licenses/ component into stage'
    & $cmakeCmd.Source --install $buildDir --prefix $stageDir --component licenses
    if ($LASTEXITCODE -ne 0) { throw 'cmake --install --component licenses failed.' }

    Write-Host '==> Staging WebAssembly runtime assets'
    $requiredAssets = @(
        'index.html',
        'wasm-boot.js',
        'qtloader.js',
        "$productId.js",
        "$productId.wasm",
        'app_icon.png'
    )
    foreach ($asset in $requiredAssets) {
        $srcFile = Join-Path $appBinDir $asset
        if (-not (Test-Path -LiteralPath $srcFile -PathType Leaf)) {
            throw "Expected build output missing: $srcFile"
        }
        $item = Get-Item -LiteralPath $srcFile -Force
        if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
            throw "Refusing symlink build output: $srcFile"
        }
        Copy-Item -LiteralPath $srcFile -Destination (Join-Path $stageDir $asset) -Force
    }

    $optionalAssets = @(
        "$productId.wasm.br",
        "$productId.worker.js",
        'qt_plugins.json',
        'qt_qml_imports.json'
    )
    foreach ($optAsset in $optionalAssets) {
        $optSrc = Join-Path $appBinDir $optAsset
        if (Test-Path -LiteralPath $optSrc -PathType Leaf) {
            $optItem = Get-Item -LiteralPath $optSrc -Force
            if (-not ($optItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
                Copy-Item -LiteralPath $optSrc -Destination (Join-Path $stageDir $optAsset) -Force
            }
        }
    }

    $qtSubDir = Join-Path $appBinDir 'qt'
    if (Test-Path -LiteralPath $qtSubDir -PathType Container) {
        $qtItem = Get-Item -LiteralPath $qtSubDir -Force
        if (-not ($qtItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
            Copy-Item -LiteralPath $qtSubDir -Destination (Join-Path $stageDir 'qt') -Recurse -Force
        }
    }

    $archiveOut = Join-Path $OutputDir "$productId-wasm-demo.tar.gz"
    Write-Host "==> Validating stage & packaging $archiveOut"
    & $nodeExe $packagerScript --stage $stageDir --output $archiveOut --require-vcpkg-copyrights @threadArgs
    if ($LASTEXITCODE -ne 0) { throw 'package-wasm-demo.mjs failed.' }

    Write-Host "==> Done: $archiveOut"
} finally {
    if (Test-Path -LiteralPath $ciTempDir) {
        Remove-Item -LiteralPath $ciTempDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
