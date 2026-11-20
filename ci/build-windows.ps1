# ci/build-windows.ps1 -- Windows x64 build, test, stage and installer packaging.
#
#   .\ci\build-windows.ps1 [-Config Release] [-Version 2.1.0]
#
# Produces in dist/:
#   1. <product id>-<Version>-windows-x64.zip        (portable archive)
#   2. <product id>-<Version>-windows-x64-setup.exe  (NSIS installer, QML shortcuts + auto-upgrade)
#   3. <product id>-<Version>-windows-x64.msi        (WiX v5 MSI installer, QML shortcuts + MajorUpgrade)
# The product id ("ragoshell"), the display name and the URL are read from cmake/RagoShellDefaults.cmake.
# Optional Authenticode signing + RFC 3161 SHA-256 timestamping is controlled by RAGO_WIN_SIGN_*
# environment variables (see docs/WindowsCodeSigning.md).

param(
    [string]$Config = 'Release',
    [string]$Version = '',
    [switch]$ValidateSigningOnly,
    [string]$SignFileOnly = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vsRoot = 'C:\Program Files\Microsoft Visual Studio\18\Enterprise'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vsRoot) -and (Test-Path $vswhere)) {
    $vsRoot = (& $vswhere -latest -property installationPath | Out-String).Trim()
}
$cmake = if ($vsRoot -and (Test-Path $vsRoot)) { Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' } else { '' }
if (-not $cmake -or -not (Test-Path $cmake)) { $cmake = 'cmake' }

# Qt path resolution: prioritize $env:Qt6_DIR (accepting either the Qt prefix or lib\cmake\Qt6), fallback to default local installation
$qtDir = if ($env:Qt6_DIR -and (Test-Path $env:Qt6_DIR)) {
    if (Test-Path (Join-Path $env:Qt6_DIR 'Qt6Config.cmake')) {
        (Resolve-Path (Join-Path $env:Qt6_DIR '..\..\..')).Path
    } else {
        $env:Qt6_DIR
    }
} elseif (Test-Path 'C:\Qt\6.12.0\msvc2022_64') {
    'C:\Qt\6.12.0\msvc2022_64'
} else {
    $env:Qt6_DIR
}
$qtBin = if ($qtDir) { Join-Path $qtDir 'bin' } else { '' }
if ($qtBin -and (Test-Path $qtBin)) { $env:PATH = "$qtBin;$env:PATH" }
$plat = 'windows-x64'

# The brand has one source (cmake/RagoShellDefaults.cmake, docs/Brand.md); installers and file names read it from there.
$brandText = Get-Content "$root\cmake\RagoShellDefaults.cmake" -Raw
function Get-Brand([string]$key) {
    if ($brandText -match "set\($key\s+`"([^`"]+)`"\)") { return $Matches[1] }
    throw "$key not found in cmake/RagoShellDefaults.cmake"
}
$productName = Get-Brand 'RAGO_PRODUCT_NAME'
$productId   = Get-Brand 'RAGO_PRODUCT_ID'
$productUrl  = Get-Brand 'RAGO_PRODUCT_URL'
$appExe      = "$productId.exe"
$mcpExe      = "$productId-mcp.exe"
$iconFile    = "$root\install\windows\$productId.ico"

# Resolve default version from top-level CMakeLists.txt when -Version is omitted
$cmakeText = Get-Content "$root\CMakeLists.txt" -Raw
$vMaj = if ($cmakeText -match 'PROJECT_VERSION_MAJOR\s+"(\d+)"') { $Matches[1] } else { '2' }
$vMin = if ($cmakeText -match 'PROJECT_VERSION_MINOR\s+"(\d+)"') { $Matches[1] } else { '0' }
$vPat = if ($cmakeText -match 'PROJECT_VERSION_PATCH\s+"(\d+)"') { $Matches[1] } else { '0' }
$vBld = if ($cmakeText -match 'PROJECT_VERSION_BUILD\s+"([^"]*)"') { $Matches[1] } else { '' }
$cmakeVersion = "$vMaj.$vMin.$vPat$vBld"
if (-not $Version) { $Version = $cmakeVersion }
if ($Version -match '^[vV](?=\d)') { $Version = $Version.Substring(1) }

# Numeric 3-part (MSI) and 4-part (NSIS VIProductVersion) version strings
if ($Version -match '^(\d+)\.(\d+)\.(\d+)') {
    $msiVersion = "$($Matches[1]).$($Matches[2]).$($Matches[3])"
} else {
    $msiVersion = "$vMaj.$vMin.$vPat"
}
$nsisNumericVersion = "$msiVersion.0"

if (-not $SignFileOnly) {
    foreach ($tool in @('git', 'node')) {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
            throw "Required tool '$tool' not found on PATH"
        }
    }
    Write-Host "==> version: $Version (MSI: $msiVersion)"
}

# Isolate unit tests, headless QML checks and temporary signing artifacts from the host user's profile.
$ciTempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("$productId-win-ci-" + [System.Guid]::NewGuid().ToString('N').Substring(0, 8))
$ciConfigDir = Join-Path $ciTempDir 'cfg'
New-Item -ItemType Directory -Force $ciConfigDir | Out-Null
$prevConfigDir = $env:RAGOSHELL_CONFIG_DIR
$prevKeyStore  = $env:RAGOSHELL_KEYSTORE
$env:RAGOSHELL_CONFIG_DIR = $ciConfigDir
$env:RAGOSHELL_KEYSTORE   = 'file'
$tempPfxPath = $null
$tempMetadataPath = $null

function Test-HasEnvValue([string]$Value) {
    return (-not [string]::IsNullOrWhiteSpace($Value))
}

function New-RestrictedTempDir([string]$ParentDir) {
    $signDir = Join-Path $ParentDir 'sign'
    if (-not (Test-Path -LiteralPath $signDir)) {
        New-Item -ItemType Directory -Force $signDir | Out-Null
        if ($env:OS -eq 'Windows_NT') {
            try {
                $acl = Get-Acl -LiteralPath $signDir
                $acl.SetAccessRuleProtection($true, $false)
                $currentUser = [System.Security.Principal.WindowsIdentity]::GetCurrent().User
                $rule = New-Object System.Security.AccessControl.FileSystemAccessRule(
                    $currentUser,
                    [System.Security.AccessControl.FileSystemRights]::FullControl,
                    ([System.Security.AccessControl.InheritanceFlags]::ContainerInherit -bor [System.Security.AccessControl.InheritanceFlags]::ObjectInherit),
                    [System.Security.AccessControl.PropagationFlags]::None,
                    [System.Security.AccessControl.AccessControlType]::Allow
                )
                $acl.AddAccessRule($rule)
                Set-Acl -LiteralPath $signDir -AclObject $acl
            } catch {
                # Container accounts without standard SID mapping keep %TEMP% default user ACL.
            }
        } else {
            & chmod 700 $signDir 2>$null
        }
    }
    return $signDir
}

function Resolve-SignTool {
    if (Test-HasEnvValue $env:RAGO_WIN_SIGN_SIGNTOOL) {
        $override = $env:RAGO_WIN_SIGN_SIGNTOOL.Trim()
        if (Test-Path -LiteralPath $override -PathType Leaf) {
            return (Resolve-Path -LiteralPath $override).Path
        }
        $cmd = Get-Command $override -ErrorAction SilentlyContinue
        if ($cmd -and $cmd.Source) { return $cmd.Source }
        throw "RAGO_WIN_SIGN_SIGNTOOL points to a missing executable: $override"
    }

    # Prefer the newest x64 Windows 10/11 SDK signtool.exe (required by x64 Azure.CodeSigning.Dlib.dll).
    $sdkRoots = @()
    if ($env:WindowsSdkDir) { $sdkRoots += (Join-Path $env:WindowsSdkDir 'bin') }
    if (${env:ProgramFiles(x86)}) { $sdkRoots += (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin') }
    if ($env:ProgramFiles) { $sdkRoots += (Join-Path $env:ProgramFiles 'Windows Kits\10\bin') }

    foreach ($sdkRoot in $sdkRoots) {
        if (-not (Test-Path -LiteralPath $sdkRoot)) { continue }
        $candidates = Get-ChildItem -Path (Join-Path $sdkRoot '10.*\x64\signtool.exe') -File -ErrorAction SilentlyContinue |
            Sort-Object {
                $verDir = $_.Directory.Parent.Name
                if ($verDir -match '^\d+(\.\d+)+$') { [version]$verDir } else { [version]'0.0' }
            } -Descending
        if ($candidates) { return $candidates[0].FullName }
    }

    $pathCmd = Get-Command 'signtool.exe' -ErrorAction SilentlyContinue
    if ($pathCmd -and $pathCmd.Source) { return $pathCmd.Source }

    throw "Code signing is enabled via RAGO_WIN_SIGN_*, but signtool.exe was not found. Install the Windows 10/11 SDK Signing Tools or set RAGO_WIN_SIGN_SIGNTOOL."
}

function Resolve-SigningConfig([string]$TempDir) {
    $pfxFile  = if (Test-HasEnvValue $env:RAGO_WIN_SIGN_PFX) { $env:RAGO_WIN_SIGN_PFX.Trim() } else { '' }
    $pfxB64   = if (Test-HasEnvValue $env:RAGO_WIN_SIGN_PFX_BASE64) { $env:RAGO_WIN_SIGN_PFX_BASE64.Trim() } else { '' }
    $pfxPass  = if (-not [string]::IsNullOrEmpty($env:RAGO_WIN_SIGN_PASSWORD)) { $env:RAGO_WIN_SIGN_PASSWORD } else { '' }
    $sha1Raw  = if (Test-HasEnvValue $env:RAGO_WIN_SIGN_SHA1) { $env:RAGO_WIN_SIGN_SHA1.Trim() } else { '' }
    $subject  = if (Test-HasEnvValue $env:RAGO_WIN_SIGN_SUBJECT) { $env:RAGO_WIN_SIGN_SUBJECT.Trim() } else { '' }
    $dlib     = if (Test-HasEnvValue $env:RAGO_WIN_SIGN_DLIB) { $env:RAGO_WIN_SIGN_DLIB.Trim() } else { '' }
    $metaFile = if (Test-HasEnvValue $env:RAGO_WIN_SIGN_METADATA) { $env:RAGO_WIN_SIGN_METADATA.Trim() } else { '' }
    $metaJson = if (Test-HasEnvValue $env:RAGO_WIN_SIGN_METADATA_JSON) { $env:RAGO_WIN_SIGN_METADATA_JSON.Trim() } else { '' }
    $tsUrlRaw = if (Test-HasEnvValue $env:RAGO_WIN_SIGN_TIMESTAMP_URL) { $env:RAGO_WIN_SIGN_TIMESTAMP_URL.Trim() } else { '' }
    $storeName = if (Test-HasEnvValue $env:RAGO_WIN_SIGN_STORE) { $env:RAGO_WIN_SIGN_STORE.Trim() } else { '' }
    $machineStoreRaw = if (Test-HasEnvValue $env:RAGO_WIN_SIGN_MACHINE_STORE) { $env:RAGO_WIN_SIGN_MACHINE_STORE.Trim() } else { '' }
    $signToolOverride = if (Test-HasEnvValue $env:RAGO_WIN_SIGN_SIGNTOOL) { $env:RAGO_WIN_SIGN_SIGNTOOL.Trim() } else { '' }

    $activeModes = @()
    if ($pfxFile -or $pfxB64 -or $pfxPass) { $activeModes += 'PFX' }
    if ($sha1Raw) { $activeModes += 'SHA1' }
    if ($subject) { $activeModes += 'SUBJECT' }
    if ($dlib -or $metaFile -or $metaJson) { $activeModes += 'DLIB' }

    if ($activeModes.Count -eq 0) {
        if ($tsUrlRaw -or $storeName -or $machineStoreRaw -or $signToolOverride) {
            throw "Incomplete Windows code signing configuration: auxiliary RAGO_WIN_SIGN_* variable(s) are set, but no signing credential mode (PFX, SHA1, SUBJECT, or DLIB) was configured."
        }
        return @{ Enabled = $false }
    }

    if ($activeModes.Count -gt 1) {
        throw "Conflicting Windows code signing modes configured ($($activeModes -join ', ')); configure exactly one of PFX, SHA1, SUBJECT, or DLIB."
    }

    $mode = $activeModes[0]
    if (($mode -in @('PFX', 'DLIB')) -and ($storeName -or $machineStoreRaw)) {
        throw "RAGO_WIN_SIGN_STORE and RAGO_WIN_SIGN_MACHINE_STORE are only valid with SHA1 or SUBJECT signing modes."
    }

    $resolvedPfx = ''
    $cleanSha1 = ''
    $resolvedDlib = ''
    $resolvedMeta = ''

    switch ($mode) {
        'PFX' {
            if ($pfxFile -and $pfxB64) {
                throw "Specify either RAGO_WIN_SIGN_PFX or RAGO_WIN_SIGN_PFX_BASE64, not both."
            }
            if (-not $pfxFile -and -not $pfxB64) {
                throw "RAGO_WIN_SIGN_PASSWORD is set, but RAGO_WIN_SIGN_PFX (or RAGO_WIN_SIGN_PFX_BASE64) is missing."
            }
            if ([string]::IsNullOrEmpty($pfxPass)) {
                throw "RAGO_WIN_SIGN_PFX / RAGO_WIN_SIGN_PFX_BASE64 is set, but RAGO_WIN_SIGN_PASSWORD is missing."
            }
            if ($pfxFile) {
                if (-not (Test-Path -LiteralPath $pfxFile -PathType Leaf)) {
                    throw "PFX certificate file not found: $pfxFile"
                }
                $resolvedPfx = (Resolve-Path -LiteralPath $pfxFile).Path
            } else {
                $pfxBytes = $null
                try {
                    $pfxBytes = [Convert]::FromBase64String($pfxB64)
                } catch {
                    throw "RAGO_WIN_SIGN_PFX_BASE64 does not contain valid Base64 data."
                }
                if (-not $pfxBytes -or $pfxBytes.Length -eq 0) {
                    throw "RAGO_WIN_SIGN_PFX_BASE64 decoded to zero bytes."
                }
                $signDir = New-RestrictedTempDir -ParentDir $TempDir
                $script:tempPfxPath = Join-Path $signDir 'codesign.pfx'
                [System.IO.File]::WriteAllBytes($script:tempPfxPath, $pfxBytes)
                if ($env:OS -ne 'Windows_NT') { & chmod 600 $script:tempPfxPath 2>$null }
                [Array]::Clear($pfxBytes, 0, $pfxBytes.Length)
                $resolvedPfx = $script:tempPfxPath
            }
        }
        'SHA1' {
            $cleanSha1 = ($sha1Raw -replace '[\s:-]', '').ToUpperInvariant()
            if ($cleanSha1 -notmatch '^[0-9A-F]{40}$') {
                throw "RAGO_WIN_SIGN_SHA1 must be a 40-character hexadecimal SHA-1 thumbprint."
            }
        }
        'SUBJECT' {
            # $subject is already trimmed and non-empty.
        }
        'DLIB' {
            if (-not $dlib) {
                throw "Azure Trusted Signing metadata is set, but RAGO_WIN_SIGN_DLIB is missing."
            }
            if ($metaFile -and $metaJson) {
                throw "Specify either RAGO_WIN_SIGN_METADATA or RAGO_WIN_SIGN_METADATA_JSON, not both."
            }
            if (-not $metaFile -and -not $metaJson) {
                throw "RAGO_WIN_SIGN_DLIB is set, but RAGO_WIN_SIGN_METADATA (or RAGO_WIN_SIGN_METADATA_JSON) is missing."
            }
            if (-not (Test-Path -LiteralPath $dlib -PathType Leaf)) {
                throw "RAGO_WIN_SIGN_DLIB file not found: $dlib"
            }
            $resolvedDlib = (Resolve-Path -LiteralPath $dlib).Path
            if ($metaFile) {
                if (-not (Test-Path -LiteralPath $metaFile -PathType Leaf)) {
                    throw "RAGO_WIN_SIGN_METADATA file not found: $metaFile"
                }
                $resolvedMeta = (Resolve-Path -LiteralPath $metaFile).Path
            } else {
                try {
                    $null = $metaJson | ConvertFrom-Json -ErrorAction Stop
                } catch {
                    throw "RAGO_WIN_SIGN_METADATA_JSON is not valid JSON: $($_.Exception.Message)"
                }
                $signDir = New-RestrictedTempDir -ParentDir $TempDir
                $script:tempMetadataPath = Join-Path $signDir 'trusted-signing-metadata.json'
                $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
                [System.IO.File]::WriteAllText($script:tempMetadataPath, $metaJson, $utf8NoBom)
                $resolvedMeta = $script:tempMetadataPath
            }
        }
    }

    $tsUrl = if ($tsUrlRaw) {
        $tsUrlRaw
    } elseif ($mode -eq 'DLIB') {
        'http://timestamp.acs.microsoft.com'
    } else {
        'http://timestamp.digicert.com'
    }
    if ($tsUrl -notmatch '^https?://\S+$') {
        throw "RAGO_WIN_SIGN_TIMESTAMP_URL must be an http:// or https:// RFC 3161 URL: $tsUrl"
    }

    $useMachineStore = $machineStoreRaw -match '^(?i:1|true|yes)$'
    $signToolPath = Resolve-SignTool

    return @{
        Enabled      = $true
        Mode         = $mode
        SignTool     = $signToolPath
        TimestampUrl = $tsUrl
        PfxPath      = $resolvedPfx
        PfxPassword  = $pfxPass
        Sha1         = $cleanSha1
        Subject      = $subject
        StoreName    = $storeName
        MachineStore = $useMachineStore
        DlibPath     = $resolvedDlib
        MetadataPath = $resolvedMeta
    }
}

function Invoke-SignAndVerify([hashtable]$SignConfig, [string]$FilePath) {
    if (-not (Test-Path -LiteralPath $FilePath -PathType Leaf)) {
        throw "Target file for Authenticode signing does not exist: $FilePath"
    }
    Write-Host "    signing + verifying: $FilePath"
    $signArgs = @(
        'sign',
        '/fd', 'sha256',
        '/tr', $SignConfig.TimestampUrl,
        '/td', 'sha256',
        '/d', $productName,
        '/du', $productUrl
    )
    switch ($SignConfig.Mode) {
        'PFX' {
            # Never log $signArgs in PFX mode so RAGO_WIN_SIGN_PASSWORD never appears in build logs.
            $signArgs += @('/f', $SignConfig.PfxPath, '/p', $SignConfig.PfxPassword)
        }
        'SHA1' {
            if ($SignConfig.StoreName) { $signArgs += @('/s', $SignConfig.StoreName) }
            if ($SignConfig.MachineStore) { $signArgs += '/sm' }
            $signArgs += @('/sha1', $SignConfig.Sha1)
        }
        'SUBJECT' {
            if ($SignConfig.StoreName) { $signArgs += @('/s', $SignConfig.StoreName) }
            if ($SignConfig.MachineStore) { $signArgs += '/sm' }
            $signArgs += @('/n', $SignConfig.Subject)
        }
        'DLIB' {
            $signArgs += @('/dlib', $SignConfig.DlibPath, '/dmdf', $SignConfig.MetadataPath)
        }
    }
    $signArgs += $FilePath

    & $SignConfig.SignTool @signArgs
    if ($LASTEXITCODE -ne 0) {
        throw "signtool sign failed for '$FilePath' (exit code $LASTEXITCODE)"
    }

    & $SignConfig.SignTool 'verify' '/pa' '/v' $FilePath
    if ($LASTEXITCODE -ne 0) {
        throw "signtool verify (/pa /v) failed for '$FilePath' (exit code $LASTEXITCODE)"
    }
}

try {

$signCfg = Resolve-SigningConfig -TempDir $ciTempDir

if ($SignFileOnly) {
    if (-not $signCfg.Enabled) {
        throw "-SignFileOnly requires Windows code signing environment variables (RAGO_WIN_SIGN_*)."
    }
    Invoke-SignAndVerify -SignConfig $signCfg -FilePath $SignFileOnly
    return
}

if ($signCfg.Enabled) {
    Write-Host "==> code signing: enabled (mode=$($signCfg.Mode), timestamp=$($signCfg.TimestampUrl), signtool=$($signCfg.SignTool))"
} else {
    Write-Warning 'RAGO_WIN_SIGN_* not set; skipping Windows Authenticode code signing (unsigned build).'
}

if ($ValidateSigningOnly) {
    & node "$root\ci\generate-sbom.mjs" --verify-source-licenses
    if ($LASTEXITCODE -ne 0) { throw "Source license notices verification failed ($LASTEXITCODE)" }
    return
}

Write-Host "==> configure + build ($Config)"
$cmakeArgs = @('--preset', 'windows')
if ($qtDir) {
    $cmakeArgs += "-DCMAKE_PREFIX_PATH=$qtDir"
}
$vcpkgToolchain = if ($env:VCPKG_ROOT -and (Test-Path "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake")) {
    "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
} elseif ($env:VCPKG_INSTALLATION_ROOT -and (Test-Path "$env:VCPKG_INSTALLATION_ROOT\scripts\buildsystems\vcpkg.cmake")) {
    "$env:VCPKG_INSTALLATION_ROOT\scripts\buildsystems\vcpkg.cmake"
} else {
    $null
}
if ($vcpkgToolchain) {
    $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain"
}

if ($vswhere -and (Test-Path $vswhere)) {
    $vsVer = (& $vswhere -latest -property installationVersion | Out-String).Trim()
    if ($vsVer -and $vsVer.StartsWith('17.')) {
        $cmakeArgs += @('-G', 'Visual Studio 17 2022')
    }
}

& $cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }
& $cmake --build "$root\build\windows" --config $Config
if ($LASTEXITCODE -ne 0) { throw "CMake build failed ($LASTEXITCODE)" }

Write-Host "==> unit tests"
& "$root\build\windows\src\ragoshell-unit-tests\$Config\rago_unit_tests.exe"
if ($LASTEXITCODE -ne 0) { throw "Unit tests failed ($LASTEXITCODE)" }

Write-Host "==> translations (up to date, placeholders, plural forms)"
& $cmake --build "$root\build\windows" --config $Config --target update_translations
if ($LASTEXITCODE -ne 0) { throw "update_translations failed ($LASTEXITCODE)" }
& git -C $root diff --exit-code --stat -- src/ragoshell-app/i18n
if ($LASTEXITCODE -ne 0) { throw "translation files are out of date: build the update_translations target and commit src/ragoshell-app/i18n" }
& node "$root\ci\check-i18n.mjs"
if ($LASTEXITCODE -ne 0) { throw "ci/check-i18n.mjs failed ($LASTEXITCODE)" }

Write-Host "==> staging ($Version)"
$stage = "$root\dist\$productId-$Version-$plat"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null

# The app, the MCP server and the driver DLLs share one output folder.
# Copy the two product executables explicitly (not *.exe) and drop any leftover
# node.exe / rago-shell from incremental build folders predating the QJSEngine switch.
$binDir = "$root\build\windows\src\ragoshell-app\$Config"
Remove-Item -Recurse -Force "$binDir\rago-shell", "$binDir\node.exe" -ErrorAction SilentlyContinue
# Both executables must exist: a missing one used to be skipped silently and the installers shipped without it.
foreach ($exe in @($appExe, $mcpExe)) {
    if (-not (Test-Path "$binDir\$exe")) { throw "$binDir\$exe not found: the build did not produce it" }
}
Copy-Item "$binDir\$appExe", "$binDir\$mcpExe", "$binDir\*.dll" $stage

$vcpkgBin = "$root\build\windows\vcpkg_installed\x64-windows\bin"
if (Test-Path "$vcpkgBin\libssl-3-x64.dll") {
    Copy-Item "$vcpkgBin\libssl-3-x64.dll", "$vcpkgBin\libcrypto-3-x64.dll" $stage -Force -ErrorAction SilentlyContinue
}

# Qt runtime + plugin DLLs (--qmldir for the QtQuick/Controls/Dialogs imports);
# the MCP server uses a subset of the same DLLs.
$windeployqt = if ($qtBin -and (Test-Path "$qtBin\windeployqt.exe")) {
    "$qtBin\windeployqt.exe"
} else {
    (Get-Command windeployqt.exe -ErrorAction SilentlyContinue).Source
}
if (-not $windeployqt) { $windeployqt = 'windeployqt.exe' }

& $windeployqt --release --no-compiler-runtime --no-translations `
    --qmldir "$root\src\ragoshell-app\qml" "$stage\$appExe"
if ($LASTEXITCODE -ne 0) { throw "windeployqt ($appExe) failed ($LASTEXITCODE)" }

# Bundle MSVC x64 C++ Runtime DLLs so clean client machines run out-of-the-box
$crtDirs = Get-ChildItem "$vsRoot\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notmatch '\\onecore\\' } |
    Sort-Object FullName -Descending
if ($crtDirs) {
    $crtDir = $crtDirs[0].FullName
    Write-Host "==> bundling MSVC CRT from $crtDir"
    Copy-Item "$crtDir\*.dll" $stage -Force
} else {
    Write-Warning "MSVC CRT redistributable folder not found under $vsRoot; target PCs will need VC++ Redistributable."
}

& $cmake --install "$root\build\windows" --config $Config --component licenses --prefix $stage
if ($LASTEXITCODE -ne 0) { throw "CMake licenses component install failed ($LASTEXITCODE)" }
& node "$root\ci\generate-sbom.mjs" --verify-licenses "$stage\licenses" --require-vcpkg-copyrights
if ($LASTEXITCODE -ne 0) { throw "Staged licenses/ bundle verification failed ($LASTEXITCODE)" }

if ($signCfg.Enabled) {
    Write-Host "==> code signing staged executables ($appExe, $mcpExe)"
    Invoke-SignAndVerify -SignConfig $signCfg -FilePath "$stage\$appExe"
    Invoke-SignAndVerify -SignConfig $signCfg -FilePath "$stage\$mcpExe"
}

Write-Host "==> QML runtime load check (English, then Arabic for right-to-left and Japanese for CJK fonts)"
if (Test-Path $ciConfigDir) { Remove-Item -Recurse -Force $ciConfigDir }
New-Item -ItemType Directory -Force $ciConfigDir | Out-Null
foreach ($checkArgs in @('--check-qml', '--check-qml --lang ar', '--check-qml --lang ja')) {
    $qmlCheck = Start-Process -FilePath "$stage\$appExe" -ArgumentList $checkArgs -PassThru
    # First start right after staging can be slow (antivirus scans the fresh DLLs);
    # the check itself takes a few seconds, so this only catches real hangs.
    if (-not $qmlCheck.WaitForExit(60000)) {
        Stop-Process -Id $qmlCheck.Id -Force -ErrorAction SilentlyContinue
        throw "$productId $checkArgs timed out"
    }
    if ($qmlCheck.ExitCode -ne 0) {
        throw "$productId $checkArgs failed with exit code $($qmlCheck.ExitCode)"
    }
}

Write-Host "==> packaging portable ZIP"
$zipOut = "$stage.zip"
if (Test-Path $zipOut) { Remove-Item -Force $zipOut }
Compress-Archive -Path "$stage\*" -DestinationPath $zipOut

# Locate NSIS (makensis.exe)
$makensis = (Get-Command makensis -ErrorAction SilentlyContinue).Source
if (-not $makensis) {
    foreach ($candidate in @(
        "$env:LOCALAPPDATA\Programs\NSIS\makensis.exe",
        "${env:ProgramFiles(x86)}\NSIS\makensis.exe",
        "$env:ProgramFiles\NSIS\makensis.exe"
    )) {
        if (Test-Path $candidate) { $makensis = $candidate; break }
    }
}

$setupOut = "$stage-setup.exe"
if ($makensis) {
    Write-Host "==> packaging NSIS installer ($setupOut)"
    if (Test-Path $setupOut) { Remove-Item -Force $setupOut }
    $nsisArgs = @(
        '/V2',
        '/INPUTCHARSET', 'UTF8',
        "/DAPP_NAME=$productName",
        "/DAPP_PUBLISHER=$productName",
        "/DAPP_URL=$productUrl",
        "/DAPP_ID=$productId",
        "/DAPP_EXE=$appExe",
        "/DMCP_EXE=$mcpExe",
        "/DAPP_VERSION=$Version",
        "/DAPP_VERSION_NUMERIC=$nsisNumericVersion",
        "/DSTAGE_DIR=$stage",
        "/DOUT_FILE=$setupOut",
        "/DICON_NAME=$productId.ico",
        "/DICON_FILE=$iconFile",
        "/DSIDEBAR_BMP=$root\install\windows\nsis-sidebar.bmp",
        "/DTOPBAR_BMP=$root\install\windows\nsis-topbar.bmp",
        "/DLICENSE_FILE=$root\LICENSE"
    )
    if ($signCfg.Enabled) {
        $nsisVerRaw = (& $makensis /VERSION 2>$null | Out-String).Trim()
        if ($nsisVerRaw -match '^v?(\d+)\.(\d+)' -and ([version]"$($Matches[1]).$($Matches[2])" -ge [version]'3.8')) {
            $signDir = New-RestrictedTempDir -ParentDir $ciTempDir
            $uninstSignCmdFile = Join-Path $signDir 'sign-uninst.cmd'
            $cmdLines = @(
                '@echo off',
                '"%RAGO_INTERNAL_PWSH%" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%RAGO_INTERNAL_BUILD_SCRIPT%" -SignFileOnly "%~1"',
                'exit /b %ERRORLEVEL%'
            )
            [System.IO.File]::WriteAllLines($uninstSignCmdFile, $cmdLines, [System.Text.Encoding]::ASCII)
            $env:RAGO_INTERNAL_PWSH = (Get-Process -Id $PID).Path
            $env:RAGO_INTERNAL_BUILD_SCRIPT = $PSCommandPath
            $nsisArgs += "/DUNINST_SIGN_CMD=call `"$uninstSignCmdFile`" `"%1`""
        } else {
            Write-Warning "NSIS version '$nsisVerRaw' is older than 3.08; skipping compile-time Uninstall.exe signing (!uninstfinalize requires NSIS 3.08+)."
        }
    }
    $nsisArgs += "$root\install\windows\$productId.nsi"
    & $makensis @nsisArgs
    if ($LASTEXITCODE -ne 0) { throw "NSIS packaging failed ($LASTEXITCODE)" }
    if ($signCfg.Enabled) {
        Write-Host "==> code signing NSIS installer ($setupOut)"
        Invoke-SignAndVerify -SignConfig $signCfg -FilePath $setupOut
    }
} else {
    Write-Warning 'makensis.exe not found; skipping NSIS .exe installer.'
}

# Locate WiX v5 CLI (wix.exe)
$wix = (Get-Command wix -ErrorAction SilentlyContinue).Source
if (-not $wix -and (Test-Path "$env:USERPROFILE\.dotnet\tools\wix.exe")) {
    $wix = "$env:USERPROFILE\.dotnet\tools\wix.exe"
}

$msiOut = "$stage.msi"
if ($wix) {
    Write-Host "==> packaging WiX MSI installer ($msiOut)"
    if (Test-Path $msiOut) { Remove-Item -Force $msiOut }
    & $wix build -arch x64 `
        -pdbtype none `
        -ext WixToolset.UI.wixext `
        -d "ProductName=$productName" `
        -d "ProductUrl=$productUrl" `
        -d "AppExe=$appExe" `
        -d "McpExe=$mcpExe" `
        -d "StageDir=$stage" `
        -d "MsiVersion=$msiVersion" `
        -d "IconFile=$iconFile" `
        -d "LicenseRtf=$root\install\windows\License.rtf" `
        -d "DialogBmp=$root\install\windows\wix-dialog.bmp" `
        -d "BannerBmp=$root\install\windows\wix-banner.bmp" `
        -o $msiOut `
        "$root\install\windows\$productId.wxs"
    if ($LASTEXITCODE -ne 0) { throw "WiX MSI packaging failed ($LASTEXITCODE)" }
    if ($signCfg.Enabled) {
        Write-Host "==> code signing WiX MSI installer ($msiOut)"
        Invoke-SignAndVerify -SignConfig $signCfg -FilePath $msiOut
    }
} else {
    Write-Warning 'wix.exe not found; skipping MSI installer.'
}

Write-Host "==> done:"
foreach ($artifact in @($zipOut, $setupOut, $msiOut)) {
    if (Test-Path $artifact) {
        $mb = [math]::Round((Get-Item $artifact).Length / 1MB, 2)
        Write-Host "    $artifact ($mb MB)"
    }
}

} finally {
    if ($tempPfxPath -and (Test-Path -LiteralPath $tempPfxPath)) {
        try {
            $pfxLen = (Get-Item -LiteralPath $tempPfxPath -Force).Length
            if ($pfxLen -gt 0) {
                $zeros = New-Object byte[] $pfxLen
                [System.IO.File]::WriteAllBytes($tempPfxPath, $zeros)
            }
        } catch {
            # Best-effort zero wipe before removal.
        }
        Remove-Item -LiteralPath $tempPfxPath -Force -ErrorAction SilentlyContinue
    }
    if ($tempMetadataPath -and (Test-Path -LiteralPath $tempMetadataPath)) {
        Remove-Item -LiteralPath $tempMetadataPath -Force -ErrorAction SilentlyContinue
    }
    Remove-Item Env:\RAGO_INTERNAL_PWSH -ErrorAction SilentlyContinue
    Remove-Item Env:\RAGO_INTERNAL_BUILD_SCRIPT -ErrorAction SilentlyContinue
    if ($null -eq $prevConfigDir) { Remove-Item Env:\RAGOSHELL_CONFIG_DIR -ErrorAction SilentlyContinue } else { $env:RAGOSHELL_CONFIG_DIR = $prevConfigDir }
    if ($null -eq $prevKeyStore)  { Remove-Item Env:\RAGOSHELL_KEYSTORE   -ErrorAction SilentlyContinue } else { $env:RAGOSHELL_KEYSTORE   = $prevKeyStore }
    if (Test-Path $ciTempDir) { Remove-Item -Recurse -Force $ciTempDir -ErrorAction SilentlyContinue }
}
