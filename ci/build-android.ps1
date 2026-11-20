# ci/build-android.ps1 -- Android arm64 Release build, signing, APK + AAB packaging, and 16 KB alignment gate (Windows host).
#
#   .\ci\build-android.ps1 [-Version 2.1.0] [-ValidateSigningOnly]
#
# Produces in dist/:
#   1. <product id>-<Version>-android-arm64.apk  (direct download / sideload APK; v2+v3 signed when keystore is set)
#   2. <product id>-<Version>-android-arm64.aab  (Google Play App Bundle; upload-key signed when keystore is set)
#
# Enforces 16 KB ELF LOAD segment alignment (p_align >= 0x4000) across the build tree and inside both .apk and .aab
# archives by default (RAGO_ANDROID_STRICT_ALIGN=1). See docs/AndroidCodeSigning.md.

param(
    [string]$Version = '',
    [switch]$ValidateSigningOnly,
    [string[]]$CheckAlignOnly = @(),
    [string[]]$SignPackagesOnly = @()
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$root = Split-Path -Parent $PSScriptRoot
$isWindowsHost = ($env:OS -eq 'Windows_NT')

# The brand has one source (cmake/RagoShellDefaults.cmake, docs/Brand.md); file names and identifiers read it from there.
$brandPath = Join-Path (Join-Path $root 'cmake') 'RagoShellDefaults.cmake'
$brandText = Get-Content -LiteralPath $brandPath -Raw
function Get-Brand([string]$Key) {
    if ($brandText -match "set\($Key\s+`"([^`"]+)`"\)") { return $Matches[1] }
    throw "$Key not found in cmake/RagoShellDefaults.cmake"
}
$productName = Get-Brand 'RAGO_PRODUCT_NAME'
$productId   = Get-Brand 'RAGO_PRODUCT_ID'
$bundleId    = Get-Brand 'RAGO_BUNDLE_ID'

$cmakeListsPath = Join-Path $root 'CMakeLists.txt'
$cmakeText = Get-Content -LiteralPath $cmakeListsPath -Raw
$vMaj = if ($cmakeText -match 'PROJECT_VERSION_MAJOR\s+"(\d+)"') { $Matches[1] } else { '2' }
$vMin = if ($cmakeText -match 'PROJECT_VERSION_MINOR\s+"(\d+)"') { $Matches[1] } else { '1' }
$vPat = if ($cmakeText -match 'PROJECT_VERSION_PATCH\s+"(\d+)"') { $Matches[1] } else { '0' }
$vBld = if ($cmakeText -match 'PROJECT_VERSION_BUILD\s+"([^"]*)"') { $Matches[1] } else { '' }
$minSdk = if ($cmakeText -match 'set\(RAGO_ANDROID_MIN_SDK\s+"(\d+)"') { $Matches[1] } else { '28' }
$defaultVersion = "$vMaj.$vMin.$vPat$vBld"

if (-not $Version) { $Version = $defaultVersion }
if ($Version -match '^[vV](?=\d)') { $Version = $Version.Substring(1) }
if ($Version -notmatch '^[A-Za-z0-9][A-Za-z0-9._+-]{0,63}$' -or $Version.Contains('..')) {
    throw 'Invalid version string (allowed: alphanumeric, ., _, +, -; no slashes or "..").'
}

$modeCount = 0
if ($ValidateSigningOnly) { $modeCount++ }
if ($CheckAlignOnly -and $CheckAlignOnly.Count -gt 0) { $modeCount++ }
if ($SignPackagesOnly -and $SignPackagesOnly.Count -gt 0) { $modeCount++ }
if ($modeCount -gt 1) {
    throw '-ValidateSigningOnly, -CheckAlignOnly, and -SignPackagesOnly are mutually exclusive.'
}
if ($SignPackagesOnly -and $SignPackagesOnly.Count -gt 0 -and $SignPackagesOnly.Count -ne 4) {
    throw '-SignPackagesOnly requires 4 paths: <in_apk>, <in_aab>, <out_apk>, <out_aab>.'
}

$strictAlign = if ([string]::IsNullOrEmpty($env:RAGO_ANDROID_STRICT_ALIGN)) { '1' } else { $env:RAGO_ANDROID_STRICT_ALIGN }
if ($strictAlign -ne '0' -and $strictAlign -ne '1') {
    throw "RAGO_ANDROID_STRICT_ALIGN must be '0' or '1'."
}
$requireSigning = if ([string]::IsNullOrEmpty($env:RAGO_ANDROID_REQUIRE_SIGNING)) { '0' } else { $env:RAGO_ANDROID_REQUIRE_SIGNING }
if ($requireSigning -ne '0' -and $requireSigning -ne '1') {
    throw "RAGO_ANDROID_REQUIRE_SIGNING must be '0' or '1'."
}

$allowedRagoAndroid = @(
    'RAGO_ANDROID_KEYSTORE',
    'RAGO_ANDROID_KEYSTORE_BASE64',
    'RAGO_ANDROID_KEYSTORE_PASS',
    'RAGO_ANDROID_KEY_ALIAS',
    'RAGO_ANDROID_KEY_PASS',
    'RAGO_ANDROID_REQUIRE_SIGNING',
    'RAGO_ANDROID_STRICT_ALIGN',
    'RAGO_ANDROID_BUILD_TOOLS_DIR',
    'RAGO_ANDROID_READELF',
    'RAGO_ANDROID_STORE_URL',
    'RAGO_ANDROID_MIN_SDK',
    'RAGO_ANDROID_TARGET_SDK',
    'RAGO_ANDROID_COMPILE_SDK'
)
foreach ($entry in (Get-ChildItem Env:)) {
    if ($entry.Name.StartsWith('RAGO_ANDROID_', [System.StringComparison]::Ordinal) -and ($allowedRagoAndroid -notcontains $entry.Name)) {
        throw "Unrecognized Android variable '$($entry.Name)' (check spelling in docs/AndroidCodeSigning.md)."
    }
}

$plat = 'android-arm64'
$ciTempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("$productId-android-ci-" + [System.Guid]::NewGuid().ToString('N').Substring(0, 10))
New-Item -ItemType Directory -Force -Path $ciTempDir | Out-Null
if (-not $isWindowsHost) { & chmod 700 $ciTempDir 2>$null }
$tempKeystorePath = $null

function Clear-AndRemoveFile([string]$FilePath) {
    if ([string]::IsNullOrWhiteSpace($FilePath)) { return }
    if (-not (Test-Path -LiteralPath $FilePath -PathType Leaf)) { return }
    try {
        $item = Get-Item -LiteralPath $FilePath -Force -ErrorAction Stop
        if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
            Remove-Item -LiteralPath $FilePath -Force -ErrorAction SilentlyContinue
            return
        }
        $len = [int][Math]::Min($item.Length, 67108864)
        if ($len -gt 0) {
            $zeros = New-Object byte[] $len
            [System.IO.File]::WriteAllBytes($FilePath, $zeros)
        }
    } catch {
        # Best-effort zero-wipe before unlink.
    }
    Remove-Item -LiteralPath $FilePath -Force -ErrorAction SilentlyContinue
}

function New-RestrictedTempDir([string]$ParentDir, [string]$SubName = 'sign') {
    $subDir = Join-Path $ParentDir $SubName
    if (-not (Test-Path -LiteralPath $subDir)) {
        New-Item -ItemType Directory -Force -Path $subDir | Out-Null
        if ($isWindowsHost) {
            try {
                $acl = Get-Acl -LiteralPath $subDir
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
                Set-Acl -LiteralPath $subDir -AclObject $acl
            } catch {
                # Container accounts without standard SID mapping keep %TEMP% default user ACL.
            }
        } else {
            & chmod 700 $subDir 2>$null
        }
    }
    return $subDir
}

# Capture sensitive signing variables into local scope and remove them from process environment
# so CMake, Ninja, vcpkg, and Gradle subprocesses never inherit keystore credentials.
$savedSecretEnv = @{}
$secretVarNames = @(
    'RAGO_ANDROID_KEYSTORE',
    'RAGO_ANDROID_KEYSTORE_BASE64',
    'ANDROID_KEYSTORE_BASE64',
    'RAGO_ANDROID_KEYSTORE_PASS',
    'ANDROID_KEYSTORE_PASSWORD',
    'RAGO_ANDROID_KEY_ALIAS',
    'ANDROID_KEY_ALIAS',
    'RAGO_ANDROID_KEY_PASS',
    'ANDROID_KEY_PASSWORD'
)
foreach ($name in $secretVarNames) {
    $val = [Environment]::GetEnvironmentVariable($name, 'Process')
    $savedSecretEnv[$name] = $val
    [Environment]::SetEnvironmentVariable($name, $null, 'Process')
}
$inBuildToolsDir = [Environment]::GetEnvironmentVariable('RAGO_ANDROID_BUILD_TOOLS_DIR', 'Process')
$inReadelfOverride = [Environment]::GetEnvironmentVariable('RAGO_ANDROID_READELF', 'Process')

function Normalize-SingleLine([string]$Name, [string]$Val) {
    if ([string]::IsNullOrEmpty($Val)) { return '' }
    if ($Val.IndexOfAny(@([char]"`r", [char]"`n", [char]0)) -ge 0) {
        throw "$Name must be a single-line value without newline characters."
    }
    $trimmed = $Val.Trim()
    if (-not $trimmed) {
        throw "$Name is set to whitespace only; unset it or provide a valid value."
    }
    return $trimmed
}

function Assert-ValidSecret([string]$Name, [string]$Val) {
    if ([string]::IsNullOrEmpty($Val)) { return }
    if ($Val.IndexOfAny(@([char]"`r", [char]"`n", [char]0)) -ge 0) {
        throw "$Name must not contain CR, LF, or NUL characters."
    }
    if (-not $Val.Trim()) {
        throw "$Name is set to whitespace only; unset it or provide a valid password."
    }
}

function Resolve-SingleLineAlias([string]$PrimaryName, [string]$PrimaryRaw, [string]$AliasName, [string]$AliasRaw) {
    $pVal = Normalize-SingleLine $PrimaryName $PrimaryRaw
    $aVal = Normalize-SingleLine $AliasName $AliasRaw
    if ($pVal -and $aVal -and ($pVal -cne $aVal)) {
        throw "Conflicting values in $PrimaryName and $AliasName; specify only one or ensure they match."
    }
    if ($pVal) { return $pVal }
    return $aVal
}

function Find-ExecutableInDir([string]$Dir, [string[]]$Candidates) {
    if ([string]::IsNullOrWhiteSpace($Dir) -or -not (Test-Path -LiteralPath $Dir -PathType Container)) {
        return ''
    }
    foreach ($cand in $Candidates) {
        $full = Join-Path $Dir $cand
        if (Test-Path -LiteralPath $full -PathType Leaf) {
            return (Resolve-Path -LiteralPath $full).Path
        }
    }
    return ''
}

function Resolve-JdkTool([string]$BaseName) {
    $candidates = if ($isWindowsHost) { @("$BaseName.exe", $BaseName) } else { @($BaseName) }
    if (-not [string]::IsNullOrWhiteSpace($env:JAVA_HOME)) {
        $fromJavaHome = Find-ExecutableInDir (Join-Path $env:JAVA_HOME 'bin') $candidates
        if ($fromJavaHome) { return $fromJavaHome }
    }
    foreach ($cand in $candidates) {
        $cmd = Get-Command $cand -ErrorAction SilentlyContinue
        if ($cmd -and $cmd.Source) { return $cmd.Source }
    }
    return ''
}

function Resolve-AndroidSdkGuess {
    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($env:ANDROID_SDK_ROOT)) { $candidates += $env:ANDROID_SDK_ROOT }
    if (-not [string]::IsNullOrWhiteSpace($env:ANDROID_HOME)) { $candidates += $env:ANDROID_HOME }
    if ($env:LOCALAPPDATA) { $candidates += (Join-Path $env:LOCALAPPDATA 'Android\Sdk') }
    if ($HOME) {
        $candidates += (Join-Path $HOME 'Library/Android/sdk')
        $candidates += '/usr/local/lib/android/sdk'
        $candidates += (Join-Path $HOME 'Android/Sdk')
    }
    foreach ($c in $candidates) {
        if (-not [string]::IsNullOrWhiteSpace($c) -and (Test-Path -LiteralPath $c -PathType Container)) {
            return (Resolve-Path -LiteralPath $c).Path
        }
    }
    return ''
}

function Resolve-SigningTools {
    $keytool = Resolve-JdkTool 'keytool'
    if (-not $keytool) { throw "Required JDK tool 'keytool' not found on PATH or JAVA_HOME/bin." }
    $jarsigner = Resolve-JdkTool 'jarsigner'
    if (-not $jarsigner) { throw "Required JDK tool 'jarsigner' not found on PATH or JAVA_HOME/bin." }
    $jarTool = Resolve-JdkTool 'jar'
    if (-not $jarTool) { throw "Required JDK tool 'jar' not found on PATH or JAVA_HOME/bin." }

    $zaNames = if ($isWindowsHost) { @('zipalign.exe', 'zipalign') } else { @('zipalign') }
    $asNames = if ($isWindowsHost) { @('apksigner.bat', 'apksigner.exe', 'apksigner') } else { @('apksigner') }

    $zipalign = ''
    $apksigner = ''
    if (-not [string]::IsNullOrEmpty($inBuildToolsDir)) {
        $btDir = Normalize-SingleLine 'RAGO_ANDROID_BUILD_TOOLS_DIR' $inBuildToolsDir
        if (-not (Test-Path -LiteralPath $btDir -PathType Container)) {
            throw "RAGO_ANDROID_BUILD_TOOLS_DIR does not exist: $btDir"
        }
        $zipalign = Find-ExecutableInDir $btDir $zaNames
        $apksigner = Find-ExecutableInDir $btDir $asNames
        if (-not $zipalign) { throw "zipalign not found under RAGO_ANDROID_BUILD_TOOLS_DIR ($btDir)" }
        if (-not $apksigner) { throw "apksigner not found under RAGO_ANDROID_BUILD_TOOLS_DIR ($btDir)" }
    } else {
        $sdkGuess = Resolve-AndroidSdkGuess
        if ($sdkGuess) {
            $btRoot = Join-Path $sdkGuess 'build-tools'
            if (Test-Path -LiteralPath $btRoot -PathType Container) {
                $dirs = Get-ChildItem -LiteralPath $btRoot -Directory -ErrorAction SilentlyContinue |
                    Sort-Object {
                        $clean = ($_.Name -replace '[^0-9.]', '')
                        if ($clean -match '^\d+(\.\d+)+$') { [version]$clean } else { [version]'0.0' }
                    } -Descending
                foreach ($d in $dirs) {
                    $za = Find-ExecutableInDir $d.FullName $zaNames
                    $as = Find-ExecutableInDir $d.FullName $asNames
                    if ($za -and $as) {
                        $zipalign = $za
                        $apksigner = $as
                        break
                    }
                }
            }
        }
        if (-not $zipalign -or -not $apksigner) {
            foreach ($n in $zaNames) {
                $c = Get-Command $n -ErrorAction SilentlyContinue
                if ($c -and $c.Source) { $zipalign = $c.Source; break }
            }
            foreach ($n in $asNames) {
                $c = Get-Command $n -ErrorAction SilentlyContinue
                if ($c -and $c.Source) { $apksigner = $c.Source; break }
            }
        }
    }

    if (-not $zipalign -or -not $apksigner) {
        throw "Android signing is enabled via RAGO_ANDROID_*, but 'zipalign' and 'apksigner' were not found under ANDROID_SDK_ROOT/build-tools or PATH (or set RAGO_ANDROID_BUILD_TOOLS_DIR)."
    }

    return @{
        Keytool   = $keytool
        Jarsigner = $jarsigner
        Jar       = $jarTool
        Zipalign  = $zipalign
        Apksigner = $apksigner
    }
}

function Resolve-ReadelfTool {
    $names = if ($isWindowsHost) { @('llvm-readelf.exe', 'readelf.exe', 'llvm-readelf', 'readelf') } else { @('llvm-readelf', 'readelf') }
    if (-not [string]::IsNullOrEmpty($inReadelfOverride)) {
        $ov = Normalize-SingleLine 'RAGO_ANDROID_READELF' $inReadelfOverride
        if (Test-Path -LiteralPath $ov -PathType Leaf) {
            return (Resolve-Path -LiteralPath $ov).Path
        }
        $cmd = Get-Command $ov -ErrorAction SilentlyContinue
        if ($cmd -and $cmd.Source) { return $cmd.Source }
        throw "RAGO_ANDROID_READELF points to a missing tool: $ov"
    }

    foreach ($n in $names) {
        $cmd = Get-Command $n -ErrorAction SilentlyContinue
        if ($cmd -and $cmd.Source) { return $cmd.Source }
    }

    $ndkRoots = @()
    if (-not [string]::IsNullOrWhiteSpace($env:ANDROID_NDK_ROOT) -and (Test-Path -LiteralPath $env:ANDROID_NDK_ROOT -PathType Container)) {
        $ndkRoots += $env:ANDROID_NDK_ROOT
    }
    if (-not [string]::IsNullOrWhiteSpace($env:ANDROID_NDK_HOME) -and (Test-Path -LiteralPath $env:ANDROID_NDK_HOME -PathType Container)) {
        $ndkRoots += $env:ANDROID_NDK_HOME
    }
    $sdkGuess = Resolve-AndroidSdkGuess
    if ($sdkGuess) {
        $ndkParent = Join-Path $sdkGuess 'ndk'
        if (Test-Path -LiteralPath $ndkParent -PathType Container) {
            $ndkDirs = Get-ChildItem -LiteralPath $ndkParent -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending
            foreach ($nd in $ndkDirs) { $ndkRoots += $nd.FullName }
        }
    }

    foreach ($nr in $ndkRoots) {
        $prebuilt = Join-Path (Join-Path $nr 'toolchains') (Join-Path 'llvm' 'prebuilt')
        if (-not (Test-Path -LiteralPath $prebuilt -PathType Container)) { continue }
        foreach ($platDir in (Get-ChildItem -LiteralPath $prebuilt -Directory -ErrorAction SilentlyContinue)) {
            $binDir = Join-Path $platDir.FullName 'bin'
            foreach ($exeName in @('llvm-readelf.exe', 'llvm-readelf')) {
                $cand = Join-Path $binDir $exeName
                if (Test-Path -LiteralPath $cand -PathType Leaf) {
                    return $cand
                }
            }
        }
    }
    return ''
}

function Test-SingleElfAlignment([string]$ReadelfBin, [string]$SoFile, [string]$Label) {
    $safeSo = if ($SoFile.StartsWith('-', [System.StringComparison]::Ordinal)) { "./$SoFile" } else { $SoFile }
    $outLines = & $ReadelfBin -lW $safeSo 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $outLines) {
        Write-Error "Failed to read ELF program headers from $Label" -ErrorAction Continue
        return $false
    }
    $loadCount = 0
    foreach ($line in $outLines) {
        $trimmed = $line.Trim()
        if ($trimmed -match '^LOAD(?:\s|$)') {
            $loadCount++
            $tokens = $trimmed -split '\s+'
            $lastToken = $tokens[$tokens.Length - 1]
            $alignVal = -1L
            if ($lastToken -match '^0[xX]([0-9A-Fa-f]+)$') {
                $alignVal = [Convert]::ToInt64($Matches[1], 16)
            } elseif ($lastToken -match '^[0-9]+$') {
                $alignVal = [long]$lastToken
            }
            if ($alignVal -lt 16384 -or (($alignVal -band ($alignVal - 1)) -ne 0)) {
                Write-Error "$Label has ELF LOAD alignment $lastToken (expected power-of-two >= 0x4000 / 16 KB)" -ErrorAction Continue
                return $false
            }
        }
    }
    if ($loadCount -eq 0) {
        Write-Error "$Label contains no valid ELF LOAD program headers" -ErrorAction Continue
        return $false
    }
    return $true
}

function Expand-ArchiveNativeLibs([string]$ArchivePath, [string]$DestDir) {
    New-Item -ItemType Directory -Force -Path $DestDir | Out-Null
    $resolvedDest = (Resolve-Path -LiteralPath $DestDir).Path
    if (-not $resolvedDest.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
        $resolvedDest += [System.IO.Path]::DirectorySeparatorChar
    }
    $isApk = $ArchivePath.EndsWith('.apk', [System.StringComparison]::OrdinalIgnoreCase)
    $count = 0
    $rawStream = [System.IO.File]::OpenRead($ArchivePath)
    $zip = $null
    try {
        # Compute ZIP central-directory local-header offsets for 16 KB APK uncompressed .so verification
        $soLocalOffsets = @{}
        if ($isApk -and $strictAlign -eq '1') {
            $fileLen = $rawStream.Length
            $searchLen = [int][Math]::Min(65557L, $fileLen)
            $tailBuf = New-Object byte[] $searchLen
            $rawStream.Position = $fileLen - $searchLen
            [void]$rawStream.Read($tailBuf, 0, $searchLen)
            $eocdIdx = -1
            for ($i = $searchLen - 22; $i -ge 0; $i--) {
                if ([BitConverter]::ToUInt32($tailBuf, $i) -eq 0x06054b50) {
                    $eocdIdx = $i
                    break
                }
            }
            if ($eocdIdx -lt 0) { throw "Invalid ZIP central directory in $ArchivePath" }
            $totalEntries = [BitConverter]::ToUInt16($tailBuf, $eocdIdx + 10)
            $cdOffset = [long][BitConverter]::ToUInt32($tailBuf, $eocdIdx + 16)
            $rawStream.Position = $cdOffset
            $hdrBuf = New-Object byte[] 46
            for ($idx = 0; $idx -lt $totalEntries; $idx++) {
                if ($rawStream.Read($hdrBuf, 0, 46) -ne 46 -or [BitConverter]::ToUInt32($hdrBuf, 0) -ne 0x02014b50) {
                    throw "Corrupt central directory entry in $ArchivePath"
                }
                $method = [BitConverter]::ToUInt16($hdrBuf, 10)
                $nLen = [BitConverter]::ToUInt16($hdrBuf, 28)
                $xLen = [BitConverter]::ToUInt16($hdrBuf, 30)
                $cLen = [BitConverter]::ToUInt16($hdrBuf, 32)
                $lfhOff = [long][BitConverter]::ToUInt32($hdrBuf, 42)
                $nameBytes = New-Object byte[] $nLen
                [void]$rawStream.Read($nameBytes, 0, $nLen)
                $rawStream.Position += ($xLen + $cLen)
                $cdName = [System.Text.Encoding]::UTF8.GetString($nameBytes)
                $soLocalOffsets[$cdName] = @{ Method = $method; LfhOffset = $lfhOff }
            }
            $rawStream.Position = 0
        }

        $zip = New-Object System.IO.Compression.ZipArchive($rawStream, [System.IO.Compression.ZipArchiveMode]::Read, $true)
        $lfhTail = New-Object byte[] 4
        foreach ($entry in $zip.Entries) {
            $entryName = $entry.FullName
            if ($entryName.StartsWith('/') -or $entryName.Contains('\')) {
                throw "Unsafe path in archive ${ArchivePath}: $entryName"
            }
            $parts = $entryName.Split('/', [System.StringSplitOptions]::RemoveEmptyEntries)
            if ($parts -contains '..') {
                throw "Path traversal entry in archive ${ArchivePath}: $entryName"
            }
            $unixFileType = ($entry.ExternalAttributes -shr 16) -band 0xF000
            if ($unixFileType -eq 0xA000) {
                throw "Symlink entry not allowed in archive ${ArchivePath}: $entryName"
            }
            if ($entryName.EndsWith('/') -or -not $entryName.EndsWith('.so', [System.StringComparison]::Ordinal)) {
                continue
            }
            if (-not ($entryName.StartsWith('lib/', [System.StringComparison]::Ordinal) -or $entryName.StartsWith('base/lib/', [System.StringComparison]::Ordinal))) {
                continue
            }
            if ($isApk -and $strictAlign -eq '1' -and $soLocalOffsets.ContainsKey($entryName)) {
                $meta = $soLocalOffsets[$entryName]
                if ($meta.Method -ne 0) {
                    throw "Native library $entryName in APK $ArchivePath is compressed (method=$($meta.Method)); expected uncompressed ZIP_STORED."
                }
                $savedPos = $rawStream.Position
                $rawStream.Position = $meta.LfhOffset + 26
                if ($rawStream.Read($lfhTail, 0, 4) -ne 4) {
                    throw "Truncated ZIP local header for $entryName in $ArchivePath"
                }
                $rawStream.Position = $savedPos
                $fnLen = [BitConverter]::ToUInt16($lfhTail, 0)
                $exLen = [BitConverter]::ToUInt16($lfhTail, 2)
                $dataOff = $meta.LfhOffset + 30 + $fnLen + $exLen
                if (($dataOff % 16384) -ne 0) {
                    $hexOff = ('0x{0:x}' -f $dataOff)
                    throw "Uncompressed .so entry $entryName in APK $ArchivePath sits at ZIP offset $hexOff (not 16 KB / 0x4000 aligned; run zipalign -P 16)."
                }
            }
            $relOsPath = [string]::Join([string][System.IO.Path]::DirectorySeparatorChar, $parts)
            $outPath = [System.IO.Path]::GetFullPath((Join-Path $resolvedDest $relOsPath))
            if (-not $outPath.StartsWith($resolvedDest, [System.StringComparison]::Ordinal)) {
                throw "Escaped destination while extracting $entryName"
            }
            $parent = Split-Path -Parent $outPath
            if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
                New-Item -ItemType Directory -Force -Path $parent | Out-Null
            }
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $outPath, $true)
            $count++
        }
    } finally {
        if ($zip) { $zip.Dispose() }
        $rawStream.Dispose()
    }
    return $count
}

function Invoke-Elf16kAlignmentGate([string[]]$Targets) {
    $readelfBin = Resolve-ReadelfTool
    if (-not $readelfBin) {
        if ($strictAlign -eq '1') {
            throw 'llvm-readelf / readelf not found; required for 16 KB ELF alignment gate (RAGO_ANDROID_STRICT_ALIGN=1).'
        }
        Write-Warning 'llvm-readelf / readelf not found; skipped 16 KB alignment check (RAGO_ANDROID_STRICT_ALIGN=0).'
        return
    }

    $issues = 0
    $totalChecked = 0
    $idx = 0

    foreach ($t in $Targets) {
        $idx++
        if ($t.StartsWith('-', [System.StringComparison]::Ordinal)) {
            Write-Error "Alignment target path must not start with '-': $t" -ErrorAction Continue
            $issues++
            continue
        }
        if (Test-Path -LiteralPath $t -PathType Container) {
            $soFiles = Get-ChildItem -LiteralPath $t -Recurse -Filter '*.so' -File -ErrorAction SilentlyContinue |
                Where-Object { -not ($_.Attributes -band [System.IO.FileAttributes]::ReparsePoint) }
            if (-not $soFiles -or $soFiles.Count -eq 0) {
                Write-Error "No .so shared libraries found in directory $t" -ErrorAction Continue
                $issues++
                continue
            }
            foreach ($sf in $soFiles) {
                $totalChecked++
                if (-not (Test-SingleElfAlignment -ReadelfBin $readelfBin -SoFile $sf.FullName -Label $sf.FullName)) {
                    $issues++
                }
            }
        } elseif (Test-Path -LiteralPath $t -PathType Leaf) {
            if ($t -match '\.(apk|aab)$') {
                $extDir = Join-Path $ciTempDir "align-extract-$idx"
                try {
                    $extractedCount = Expand-ArchiveNativeLibs -ArchivePath $t -DestDir $extDir
                    if ($extractedCount -le 0) {
                        Write-Error "Archive $t contains zero native .so libraries under lib/ (expected arm64-v8a native payload)." -ErrorAction Continue
                        $issues++
                    } else {
                        $canonExtDir = (Resolve-Path -LiteralPath $extDir).Path
                        $soFiles = Get-ChildItem -LiteralPath $canonExtDir -Recurse -Filter '*.so' -File -ErrorAction SilentlyContinue
                        foreach ($sf in $soFiles) {
                            $totalChecked++
                            $rel = $sf.FullName.Substring($canonExtDir.Length).TrimStart('\', '/').Replace('\', '/')
                            if (-not (Test-SingleElfAlignment -ReadelfBin $readelfBin -SoFile $sf.FullName -Label "${t}!${rel}")) {
                                $issues++
                            }
                        }
                    }
                } catch {
                    Write-Error $_.Exception.Message -ErrorAction Continue
                    $issues++
                } finally {
                    Remove-Item -LiteralPath $extDir -Recurse -Force -ErrorAction SilentlyContinue
                }
            } elseif ($t -match '\.so(\..+)?$') {
                $totalChecked++
                if (-not (Test-SingleElfAlignment -ReadelfBin $readelfBin -SoFile $t -Label $t)) {
                    $issues++
                }
            } else {
                Write-Error "Unsupported alignment target '$t' (expected directory, .so, .apk, or .aab)." -ErrorAction Continue
                $issues++
            }
        } else {
            Write-Error "Alignment target does not exist: $t" -ErrorAction Continue
            $issues++
        }
    }

    if ($issues -eq 0 -and $totalChecked -gt 0) {
        Write-Host "==> 16 KB ELF LOAD alignment check passed ($totalChecked .so binary inspection(s))."
        return
    }
    if ($strictAlign -eq '1') {
        throw "16 KB ELF LOAD alignment gate failed with $issues issue(s) (RAGO_ANDROID_STRICT_ALIGN=1)."
    }
    Write-Warning "16 KB ELF LOAD alignment check reported $issues issue(s), continuing because RAGO_ANDROID_STRICT_ALIGN=0."
}

function Invoke-WithScopedKeystorePasswords([string]$StorePass, [string]$KeyPass, [scriptblock]$Action) {
    [Environment]::SetEnvironmentVariable('RAGO_KS_PASS', $StorePass, 'Process')
    [Environment]::SetEnvironmentVariable('RAGO_KEY_PASS', $KeyPass, 'Process')
    try {
        & $Action
    } finally {
        [Environment]::SetEnvironmentVariable('RAGO_KS_PASS', $null, 'Process')
        [Environment]::SetEnvironmentVariable('RAGO_KEY_PASS', $null, 'Process')
    }
}

try {
    if ($CheckAlignOnly -and $CheckAlignOnly.Count -gt 0) {
        Invoke-Elf16kAlignmentGate -Targets $CheckAlignOnly
        exit 0
    }

    # --- Fail-fast validation of Android signing configuration ---
    $ksFile = Normalize-SingleLine 'RAGO_ANDROID_KEYSTORE' $savedSecretEnv['RAGO_ANDROID_KEYSTORE']

    $rawKsB64 = $savedSecretEnv['RAGO_ANDROID_KEYSTORE_BASE64']
    $rawAliasKsB64 = $savedSecretEnv['ANDROID_KEYSTORE_BASE64']
    if (-not [string]::IsNullOrEmpty($rawKsB64) -and -not $rawKsB64.Trim()) {
        throw 'RAGO_ANDROID_KEYSTORE_BASE64 is set to whitespace only; unset it or provide a valid value.'
    }
    if (-not [string]::IsNullOrEmpty($rawAliasKsB64) -and -not $rawAliasKsB64.Trim()) {
        throw 'ANDROID_KEYSTORE_BASE64 is set to whitespace only; unset it or provide a valid value.'
    }
    if ($rawKsB64 -and $rawAliasKsB64 -and ($rawKsB64 -cne $rawAliasKsB64)) {
        throw 'Conflicting values in RAGO_ANDROID_KEYSTORE_BASE64 and ANDROID_KEYSTORE_BASE64; specify only one or ensure they match.'
    }
    $ksB64 = if ($rawKsB64) { $rawKsB64 } else { $rawAliasKsB64 }

    $rawKsPass = $savedSecretEnv['RAGO_ANDROID_KEYSTORE_PASS']
    $rawAliasKsPass = $savedSecretEnv['ANDROID_KEYSTORE_PASSWORD']
    Assert-ValidSecret 'RAGO_ANDROID_KEYSTORE_PASS' $rawKsPass
    Assert-ValidSecret 'ANDROID_KEYSTORE_PASSWORD' $rawAliasKsPass
    if ($rawKsPass -and $rawAliasKsPass -and ($rawKsPass -cne $rawAliasKsPass)) {
        throw 'Conflicting values in RAGO_ANDROID_KEYSTORE_PASS and ANDROID_KEYSTORE_PASSWORD; specify only one or ensure they match.'
    }
    $ksPass = if ($rawKsPass) { $rawKsPass } else { $rawAliasKsPass }

    $keyAlias = Resolve-SingleLineAlias 'RAGO_ANDROID_KEY_ALIAS' $savedSecretEnv['RAGO_ANDROID_KEY_ALIAS'] 'ANDROID_KEY_ALIAS' $savedSecretEnv['ANDROID_KEY_ALIAS']

    $rawKeyPass = $savedSecretEnv['RAGO_ANDROID_KEY_PASS']
    $rawAliasKeyPass = $savedSecretEnv['ANDROID_KEY_PASSWORD']
    Assert-ValidSecret 'RAGO_ANDROID_KEY_PASS' $rawKeyPass
    Assert-ValidSecret 'ANDROID_KEY_PASSWORD' $rawAliasKeyPass
    if ($rawKeyPass -and $rawAliasKeyPass -and ($rawKeyPass -cne $rawAliasKeyPass)) {
        throw 'Conflicting values in RAGO_ANDROID_KEY_PASS and ANDROID_KEY_PASSWORD; specify only one or ensure they match.'
    }
    $keyPass = if ($rawKeyPass) { $rawKeyPass } else { $rawAliasKeyPass }

    $signingEnabled = $false
    $resolvedKeystore = ''
    $effectiveKeyPass = ''
    $tools = $null

    if ($ksFile -or $ksB64 -or $ksPass -or $keyAlias -or $keyPass) {
        $signingEnabled = $true
        if ($ksFile -and $ksB64) {
            throw 'Specify either RAGO_ANDROID_KEYSTORE or RAGO_ANDROID_KEYSTORE_BASE64, not both.'
        }
        if (-not $ksFile -and -not $ksB64) {
            throw 'Android signing variables are partially set, but neither RAGO_ANDROID_KEYSTORE nor RAGO_ANDROID_KEYSTORE_BASE64 was provided.'
        }
        if (-not $ksPass) {
            throw 'RAGO_ANDROID_KEYSTORE* is set, but RAGO_ANDROID_KEYSTORE_PASS is missing.'
        }
        if (-not $keyAlias) {
            throw 'RAGO_ANDROID_KEYSTORE* is set, but RAGO_ANDROID_KEY_ALIAS is missing.'
        }
        if ($keyAlias.StartsWith('-', [System.StringComparison]::Ordinal)) {
            throw "RAGO_ANDROID_KEY_ALIAS must not start with '-'."
        }
        $effectiveKeyPass = if ($keyPass) { $keyPass } else { $ksPass }

        $tools = Resolve-SigningTools
        $signDir = New-RestrictedTempDir -ParentDir $ciTempDir -SubName 'sign'

        if ($ksFile) {
            if (-not (Test-Path -LiteralPath $ksFile -PathType Leaf)) {
                throw "RAGO_ANDROID_KEYSTORE file does not exist: $ksFile"
            }
            $resolvedKeystore = (Resolve-Path -LiteralPath $ksFile).Path
        } else {
            $cleanedB64 = ($ksB64 -replace '\s+', '')
            if (-not $cleanedB64 -or ($cleanedB64.Length % 4 -ne 0) -or ($cleanedB64 -notmatch '^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$')) {
                throw 'RAGO_ANDROID_KEYSTORE_BASE64 does not contain valid Base64 data.'
            }
            $ksBytes = $null
            try {
                $ksBytes = [Convert]::FromBase64String($cleanedB64)
            } catch {
                throw 'RAGO_ANDROID_KEYSTORE_BASE64 does not contain valid Base64 data.'
            }
            if (-not $ksBytes -or $ksBytes.Length -eq 0) {
                throw 'RAGO_ANDROID_KEYSTORE_BASE64 decoded to zero bytes.'
            }
            $script:tempKeystorePath = Join-Path $signDir 'upload.keystore'
            [System.IO.File]::WriteAllBytes($script:tempKeystorePath, $ksBytes)
            if (-not $isWindowsHost) { & chmod 600 $script:tempKeystorePath 2>$null }
            [Array]::Clear($ksBytes, 0, $ksBytes.Length)
            $resolvedKeystore = $script:tempKeystorePath
        }

        Write-Host "==> validating Android upload keystore and alias '$keyAlias'"
        $ktOut = ''
        $ktRc = 0
        Invoke-WithScopedKeystorePasswords -StorePass $ksPass -KeyPass $effectiveKeyPass -Action {
            $script:ktTmpOut = (& $tools.Keytool -list -v -keystore $resolvedKeystore -alias $keyAlias '-storepass:env' 'RAGO_KS_PASS' 2>&1 | Out-String)
            $script:ktTmpRc = $LASTEXITCODE
        }
        $ktOut = $script:ktTmpOut
        $ktRc = $script:ktTmpRc
        Remove-Variable ktTmpOut, ktTmpRc -Scope Script -ErrorAction SilentlyContinue
        if ($ktRc -ne 0) {
            throw "Failed to read alias '$keyAlias' from Android keystore (check keystore format, password, and alias)."
        }
        if ($ktOut -notmatch '(?i)PrivateKeyEntry') {
            throw "Keystore alias '$keyAlias' is not a PrivateKeyEntry (cannot sign APK/AAB with a certificate-only entry)."
        }

        $probeDir = Join-Path $signDir 'probe'
        New-Item -ItemType Directory -Force -Path $probeDir | Out-Null
        $probeTxt = Join-Path $probeDir 'probe.txt'
        $probeJar = Join-Path $probeDir 'probe.jar'
        [System.IO.File]::WriteAllText($probeTxt, "probe`n")
        Push-Location $probeDir
        try {
            & $tools.Jar cf $probeJar 'probe.txt' | Out-Null
            if ($LASTEXITCODE -ne 0) { throw 'Failed to create temporary signing probe archive.' }
        } finally {
            Pop-Location
        }
        $probeRc = 0
        Invoke-WithScopedKeystorePasswords -StorePass $ksPass -KeyPass $effectiveKeyPass -Action {
            & $tools.Jarsigner -keystore $resolvedKeystore '-storepass:env' 'RAGO_KS_PASS' '-keypass:env' 'RAGO_KEY_PASS' -digestalg SHA-256 $probeJar $keyAlias *> $null
            $script:probeTmpRc = $LASTEXITCODE
        }
        $probeRc = $script:probeTmpRc
        Remove-Variable probeTmpRc -Scope Script -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $probeDir -Recurse -Force -ErrorAction SilentlyContinue
        if ($probeRc -ne 0) {
            throw "Failed to unlock private key for alias '$keyAlias' (verify RAGO_ANDROID_KEY_PASS / RAGO_ANDROID_KEYSTORE_PASS)."
        }
        Write-Host "==> Android signing configuration validated (zipalign: $($tools.Zipalign) | apksigner: $($tools.Apksigner))"
    } else {
        if ($requireSigning -eq '1') {
            throw 'RAGO_ANDROID_REQUIRE_SIGNING=1 requires RAGO_ANDROID_KEYSTORE* signing credentials (refusing to produce unsigned release APK/AAB).'
        }
        Write-Host 'warn: RAGO_ANDROID_KEYSTORE* not set; producing unsigned/debug-signed APK and AAB (local/PR build).'
    }

    function Invoke-SignAndVerifyAndroidPackages([string]$InApk, [string]$InAab, [string]$TargetApk, [string]$TargetAab) {
        if (-not (Test-Path -LiteralPath $InApk -PathType Leaf)) { throw "Input APK not found: $InApk" }
        if (-not (Test-Path -LiteralPath $InAab -PathType Leaf)) { throw "Input AAB not found: $InAab" }

        $workDir = New-RestrictedTempDir -ParentDir $ciTempDir -SubName 'pkg-sign'
        $alignedApk = Join-Path $workDir 'aligned.apk'
        $signedApk = Join-Path $workDir 'signed.apk'
        $signingAab = Join-Path $workDir 'signing.aab'

        try {
            $prevEap = $ErrorActionPreference
            $ErrorActionPreference = 'Continue'
            try {
                $zaHelp = (& $tools.Zipalign 2>&1 | Out-String)
            } finally {
                $ErrorActionPreference = $prevEap
            }
            if ($zaHelp -match '-P\s') {
                Write-Host '==> zipalign (-P 16 -f -v 4) APK'
                & $tools.Zipalign -P 16 -f -v 4 $InApk $alignedApk | Out-Null
                if ($LASTEXITCODE -ne 0) { throw 'zipalign (-P 16) failed on APK.' }
                & $tools.Zipalign -c -P 16 4 $alignedApk | Out-Null
                if ($LASTEXITCODE -ne 0) { throw 'zipalign (-c -P 16) verification failed on APK.' }
            } elseif ($strictAlign -eq '1') {
                throw "zipalign ($($tools.Zipalign)) does not support '-P 16' (install Android SDK Build-Tools >= 35 for 16 KB APK alignment)."
            } else {
                Write-Host '==> zipalign (-p -f -v 4) APK'
                & $tools.Zipalign -p -f -v 4 $InApk $alignedApk | Out-Null
                if ($LASTEXITCODE -ne 0) { throw 'zipalign (-p) failed on APK.' }
                & $tools.Zipalign -c -p 4 $alignedApk | Out-Null
                if ($LASTEXITCODE -ne 0) { throw 'zipalign (-c -p) verification failed on APK.' }
            }

            Write-Host "==> signing APK with apksigner (v2 + v3 schemes, alias '$keyAlias')"
            Invoke-WithScopedKeystorePasswords -StorePass $ksPass -KeyPass $effectiveKeyPass -Action {
                & $tools.Apksigner sign `
                    --ks $resolvedKeystore `
                    --ks-key-alias $keyAlias `
                    --ks-pass 'env:RAGO_KS_PASS' `
                    --key-pass 'env:RAGO_KEY_PASS' `
                    --min-sdk-version $minSdk `
                    --v2-signing-enabled true `
                    --v3-signing-enabled true `
                    --out $signedApk `
                    $alignedApk
                if ($LASTEXITCODE -ne 0) { throw 'apksigner sign failed.' }
            }

            Write-Host '==> verifying APK signature (apksigner verify --verbose --print-certs)'
            # Verify both against the APK manifest's minSdkVersion AND with --min-sdk-version 24
            # so Android's ApkVerifier does not short-circuit the v2 block when minSdkVersion >= 28.
            $apkManifestVerify = (& $tools.Apksigner verify --verbose --print-certs $signedApk 2>&1 | Out-String)
            if ($LASTEXITCODE -ne 0) {
                throw "apksigner verify failed:`n$apkManifestVerify"
            }
            $apkVerifyOut = (& $tools.Apksigner verify --min-sdk-version 24 --verbose --print-certs $signedApk 2>&1 | Out-String)
            if ($LASTEXITCODE -ne 0) {
                throw "apksigner verify (--min-sdk-version 24) failed:`n$apkVerifyOut"
            }
            Write-Host $apkVerifyOut.TrimEnd()
            if ($apkVerifyOut -notmatch [regex]::Escape('Verified using v2 scheme (APK Signature Scheme v2): true')) {
                throw 'APK verification did not confirm APK Signature Scheme v2.'
            }
            if ($apkVerifyOut -notmatch [regex]::Escape('Verified using v3 scheme (APK Signature Scheme v3): true')) {
                throw 'APK verification did not confirm APK Signature Scheme v3.'
            }

            Copy-Item -LiteralPath $InAab -Destination $signingAab -Force
            if (-not $isWindowsHost) { & chmod 600 $signingAab 2>$null }
            Write-Host "==> signing AAB with jarsigner (SHA-256, alias '$keyAlias')"
            Invoke-WithScopedKeystorePasswords -StorePass $ksPass -KeyPass $effectiveKeyPass -Action {
                & $tools.Jarsigner `
                    -keystore $resolvedKeystore `
                    '-storepass:env' 'RAGO_KS_PASS' `
                    '-keypass:env' 'RAGO_KEY_PASS' `
                    -digestalg SHA-256 `
                    $signingAab `
                    $keyAlias | Out-Null
                if ($LASTEXITCODE -ne 0) { throw 'jarsigner signing failed on AAB.' }
            }

            Write-Host '==> verifying AAB signature (jarsigner -verify -verbose -certs)'
            # Android upload certificates are self-signed (not chained to a JVM TLS CA), so omit -strict
            # (which exits 4 on self-signed certs) and enforce exit 0, 'jar verified.', and zero unsigned entries.
            $aabVerifyOut = (& $tools.Jarsigner -verify -verbose -certs $signingAab 2>&1 | Out-String)
            if ($LASTEXITCODE -ne 0) {
                throw "jarsigner -verify failed on AAB:`n$aabVerifyOut"
            }
            if ($aabVerifyOut -match '(?i)(jar is unsigned|contains unsigned entries)') {
                throw 'AAB verification reported unsigned content in bundle.'
            }
            if ($aabVerifyOut -notmatch [regex]::Escape('jar verified.')) {
                throw "AAB verification did not report 'jar verified.'."
            }

            foreach ($dest in @($TargetApk, $TargetAab)) {
                $parentDir = Split-Path -Parent $dest
                if ($parentDir -and -not (Test-Path -LiteralPath $parentDir -PathType Container)) {
                    New-Item -ItemType Directory -Force -Path $parentDir | Out-Null
                }
            }
            Move-Item -LiteralPath $signedApk -Destination $TargetApk -Force
            Move-Item -LiteralPath $signingAab -Destination $TargetAab -Force
        } finally {
            Remove-Item -LiteralPath $workDir -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    if ($ValidateSigningOnly) {
        Write-Host "==> -ValidateSigningOnly succeeded (signing_enabled=$([int]$signingEnabled))."
        exit 0
    }

    if ($SignPackagesOnly -and $SignPackagesOnly.Count -eq 4) {
        if (-not $signingEnabled) {
            throw '-SignPackagesOnly requires RAGO_ANDROID_KEYSTORE* signing credentials.'
        }
        Invoke-SignAndVerifyAndroidPackages -InApk $SignPackagesOnly[0] -InAab $SignPackagesOnly[1] -TargetApk $SignPackagesOnly[2] -TargetAab $SignPackagesOnly[3]
        exit 0
    }

    if (-not $isWindowsHost) {
        throw 'Full Android CMake build in ci/build-android.ps1 targets Windows hosts (android-arm64-release preset); on macOS/Linux run ./ci/build-android.sh.'
    }

    # Resolve Visual Studio CMake & Ninja on Windows when not already on PATH
    $vsRoot = 'C:\Program Files\Microsoft Visual Studio\18\Enterprise'
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vsRoot) -and (Test-Path -LiteralPath $vswhere)) {
        $vsRoot = (& $vswhere -latest -property installationPath | Out-String).Trim()
    }
    $cmakeExe = if ($vsRoot -and (Test-Path -LiteralPath $vsRoot)) {
        Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    } else { '' }
    if (-not $cmakeExe -or -not (Test-Path -LiteralPath $cmakeExe)) { $cmakeExe = 'cmake' }
    if (-not (Get-Command 'ninja' -ErrorAction SilentlyContinue) -and $vsRoot -and (Test-Path -LiteralPath $vsRoot)) {
        $vsNinjaDir = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
        if (Test-Path -LiteralPath $vsNinjaDir -PathType Container) {
            $env:PATH = "$vsNinjaDir;$env:PATH"
        }
    }
    foreach ($reqTool in @('node', 'ninja')) {
        if (-not (Get-Command $reqTool -ErrorAction SilentlyContinue)) {
            throw "Required tool '$reqTool' not found on PATH."
        }
    }

    $vcpkgToolchain = if ($env:VCPKG_ROOT -and (Test-Path -LiteralPath $env:VCPKG_ROOT -PathType Container)) {
        Join-Path $env:VCPKG_ROOT 'scripts\buildsystems\vcpkg.cmake'
    } elseif ($vsRoot -and (Test-Path -LiteralPath (Join-Path $vsRoot 'VC\vcpkg\scripts\buildsystems\vcpkg.cmake') -PathType Leaf)) {
        Join-Path $vsRoot 'VC\vcpkg\scripts\buildsystems\vcpkg.cmake'
    } else {
        throw 'Set VCPKG_ROOT to your vcpkg checkout (or install Visual Studio vcpkg).'
    }

    $qtHost = if ($env:Qt6_DIR -and (Test-Path -LiteralPath $env:Qt6_DIR -PathType Container)) {
        $env:Qt6_DIR
    } elseif (Test-Path -LiteralPath 'C:\Qt\6.12.0\msvc2022_64' -PathType Container) {
        'C:\Qt\6.12.0\msvc2022_64'
    } else {
        throw 'Set Qt6_DIR to your host Qt6 prefix (e.g. C:\Qt\6.12.0\msvc2022_64).'
    }

    $qtAndroid = if ($env:Qt6_DIR_ANDROID -and (Test-Path -LiteralPath $env:Qt6_DIR_ANDROID -PathType Container)) {
        $env:Qt6_DIR_ANDROID
    } else {
        $candAndroid = Join-Path (Split-Path -Parent $qtHost) 'android_arm64_v8a'
        if (Test-Path -LiteralPath $candAndroid -PathType Container) {
            $candAndroid
        } else {
            throw 'Set Qt6_DIR_ANDROID to your Android Qt6 prefix (e.g. C:\Qt\6.12.0\android_arm64_v8a).'
        }
    }
    $qtAndroidToolchain = Join-Path $qtAndroid 'lib\cmake\Qt6\qt.toolchain.cmake'
    if (-not (Test-Path -LiteralPath $qtAndroidToolchain -PathType Leaf)) {
        throw "qt.toolchain.cmake not found under Qt6_DIR_ANDROID ($qtAndroid)"
    }

    $sdkRoot = Resolve-AndroidSdkGuess
    if (-not $sdkRoot) { throw 'Set ANDROID_SDK_ROOT to your Android SDK.' }
    $env:ANDROID_SDK_ROOT = $sdkRoot

    $ndkRoot = if ($env:ANDROID_NDK_ROOT -and (Test-Path -LiteralPath $env:ANDROID_NDK_ROOT -PathType Container)) {
        (Resolve-Path -LiteralPath $env:ANDROID_NDK_ROOT).Path
    } elseif ($env:ANDROID_NDK_HOME -and (Test-Path -LiteralPath $env:ANDROID_NDK_HOME -PathType Container)) {
        (Resolve-Path -LiteralPath $env:ANDROID_NDK_HOME).Path
    } else {
        $ndkParent = Join-Path $sdkRoot 'ndk'
        $latestNdk = if (Test-Path -LiteralPath $ndkParent -PathType Container) {
            Get-ChildItem -LiteralPath $ndkParent -Directory -ErrorAction SilentlyContinue |
                Sort-Object {
                    $clean = ($_.Name -replace '[^0-9.]', '')
                    if ($clean -match '^\d+(\.\d+)+$') { [version]$clean } else { [version]'0.0' }
                } -Descending |
                Select-Object -First 1
        } else { $null }
        if ($latestNdk) { $latestNdk.FullName } else { throw 'Set ANDROID_NDK_ROOT to your Android NDK.' }
    }
    $env:ANDROID_NDK_ROOT = $ndkRoot
    $env:ANDROID_NDK_HOME = $ndkRoot

    $configurePreset = 'android-arm64-release'
    $buildPreset = 'android-arm64-release'

    Write-Host "==> host: Windows | configure: $configurePreset | build: $buildPreset | version: $Version | bundle: $bundleId"
    Write-Host "==> Qt host: $qtHost"
    Write-Host "==> Qt Android: $qtAndroid"
    Write-Host "==> Android SDK: $sdkRoot"
    Write-Host "==> Android NDK: $ndkRoot"

    Write-Host '==> translation consistency & source license checks'
    & node (Join-Path (Join-Path $root 'ci') 'check-i18n.mjs')
    if ($LASTEXITCODE -ne 0) { throw 'check-i18n.mjs failed.' }
    & node (Join-Path (Join-Path $root 'ci') 'generate-sbom.mjs') --verify-source-licenses
    if ($LASTEXITCODE -ne 0) { throw 'generate-sbom.mjs --verify-source-licenses failed.' }
    $qrcContent = Get-Content -LiteralPath (Join-Path $root 'src\ragoshell-app\qml.qrc') -Raw
    if ($qrcContent -notmatch [regex]::Escape('THIRD-PARTY-NOTICES.txt')) {
        throw 'THIRD-PARTY-NOTICES.txt must remain embedded in src/ragoshell-app/qml.qrc for Android in-app license notices.'
    }

    $buildDir = Join-Path (Join-Path $root 'build') $configurePreset
    $distDir = Join-Path $root 'dist'
    New-Item -ItemType Directory -Force -Path $distDir | Out-Null
    $outApk = Join-Path $distDir "$productId-$Version-$plat.apk"
    $outAab = Join-Path $distDir "$productId-$Version-$plat.aab"
    Remove-Item -LiteralPath $outApk, $outAab -Force -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $buildDir -PathType Container) {
        Get-ChildItem -LiteralPath $buildDir -Recurse -Directory -Filter 'android-build' -ErrorAction SilentlyContinue | ForEach-Object {
            Remove-Item -LiteralPath (Join-Path $_.FullName 'build\outputs\apk'), (Join-Path $_.FullName 'build\outputs\bundle') -Recurse -Force -ErrorAction SilentlyContinue
            Get-ChildItem -LiteralPath $_.FullName -MaxDepth 1 -File -Include '*.apk', '*.aab' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
        }
    }

    Write-Host "==> configure + Release build ($configurePreset)"
    & $cmakeExe --preset $configurePreset `
        "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain" `
        "-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=$qtAndroidToolchain" `
        "-DQT_HOST_PATH=$qtHost" `
        "-DANDROID_SDK_ROOT=$sdkRoot" `
        "-DANDROID_NDK_ROOT=$ndkRoot"
    if ($LASTEXITCODE -ne 0) { throw "CMake configure ($configurePreset) failed." }

    & $cmakeExe --build --preset $buildPreset
    if ($LASTEXITCODE -ne 0) { throw "CMake build ($buildPreset) failed." }

    Write-Host "==> packaging Android APK (${productId}_make_apk)"
    & $cmakeExe --build --preset $buildPreset --target "${productId}_make_apk"
    if ($LASTEXITCODE -ne 0) {
        & $cmakeExe --build --preset $buildPreset --target apk
        if ($LASTEXITCODE -ne 0) { throw 'Android APK target failed.' }
    }

    Write-Host "==> packaging Android App Bundle AAB (${productId}_make_aab)"
    & $cmakeExe --build --preset $buildPreset --target "${productId}_make_aab"
    if ($LASTEXITCODE -ne 0) {
        & $cmakeExe --build --preset $buildPreset --target aab
        if ($LASTEXITCODE -ne 0) { throw 'Android AAB target failed.' }
    }

    function Find-GradleArtifact([string]$SubDir, [string]$Extension) {
        $abDirs = Get-ChildItem -LiteralPath $buildDir -Recurse -Directory -Filter 'android-build' -ErrorAction SilentlyContinue
        foreach ($ab in $abDirs) {
            $outputsRoot = Join-Path $ab.FullName "build\outputs\$SubDir"
            if (Test-Path -LiteralPath $outputsRoot -PathType Container) {
                $rel = Get-ChildItem -LiteralPath $outputsRoot -Recurse -Filter "*release*$Extension" -File -ErrorAction SilentlyContinue | Sort-Object FullName | Select-Object -First 1
                if ($rel) { return $rel.FullName }
                $any = Get-ChildItem -LiteralPath $outputsRoot -Recurse -Filter "*$Extension" -File -ErrorAction SilentlyContinue | Sort-Object FullName | Select-Object -First 1
                if ($any) { return $any.FullName }
            }
        }
        $fallback = Get-ChildItem -LiteralPath $buildDir -Recurse -Filter "*$Extension" -File -ErrorAction SilentlyContinue | Sort-Object FullName | Select-Object -First 1
        if ($fallback) { return $fallback.FullName }
        return ''
    }

    $rawApk = Find-GradleArtifact -SubDir 'apk' -Extension '.apk'
    $rawAab = Find-GradleArtifact -SubDir 'bundle' -Extension '.aab'
    if (-not $rawApk -or -not (Test-Path -LiteralPath $rawApk -PathType Leaf)) {
        throw "APK file was not produced in $buildDir"
    }
    if (-not $rawAab -or -not (Test-Path -LiteralPath $rawAab -PathType Leaf)) {
        throw "AAB file was not produced in $buildDir"
    }

    if ($signingEnabled) {
        Invoke-SignAndVerifyAndroidPackages -InApk $rawApk -InAab $rawAab -TargetApk $outApk -TargetAab $outAab
    } else {
        Copy-Item -LiteralPath $rawApk -Destination $outApk -Force
        Copy-Item -LiteralPath $rawAab -Destination $outAab -Force
    }

    Write-Host "==> created APK: $outApk"
    Write-Host "==> created AAB: $outAab"

    Write-Host '==> running 16 KB ELF LOAD alignment gate (build tree + APK + AAB)'
    Invoke-Elf16kAlignmentGate -Targets @($buildDir, $outApk, $outAab)

    if (-not (Test-Path -LiteralPath $outApk -PathType Leaf) -or ((Get-Item -LiteralPath $outApk).Length -le 0)) {
        throw "Final APK is missing or empty: $outApk"
    }
    if (-not (Test-Path -LiteralPath $outAab -PathType Leaf) -or ((Get-Item -LiteralPath $outAab).Length -le 0)) {
        throw "Final AAB is missing or empty: $outAab"
    }

    Write-Host "==> done Android Release build step ($productName $Version)."
} finally {
    [Environment]::SetEnvironmentVariable('RAGO_KS_PASS', $null, 'Process')
    [Environment]::SetEnvironmentVariable('RAGO_KEY_PASS', $null, 'Process')
    Clear-AndRemoveFile $tempKeystorePath
    $signSubDir = Join-Path $ciTempDir 'sign'
    if (Test-Path -LiteralPath $signSubDir -PathType Container) {
        Get-ChildItem -LiteralPath $signSubDir -Recurse -File -Force -ErrorAction SilentlyContinue | ForEach-Object {
            Clear-AndRemoveFile $_.FullName
        }
    }
    Remove-Item -LiteralPath $ciTempDir -Recurse -Force -ErrorAction SilentlyContinue
    foreach ($kv in $savedSecretEnv.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable($kv.Key, $kv.Value, 'Process')
    }
}
