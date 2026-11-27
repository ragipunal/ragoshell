# ci/gitea-release.ps1 -- create/update a Gitea release and upload dist/ assets + signed metadata.
#
#   .\ci\gitea-release.ps1 v2.1.0 [-Repo owner/ragoshell] [-DistDir dist] [-DryRun] [-Stable] [-Prerelease] [-NotesFile path]
#
# Env / ci\.env resolution order (ci\.env skipped when $env:RAGO_NO_DOTENV -eq '1'):
#   1) $env:GITEA_TOKEN / $env:GITEA_HOST / $env:GITEA_REPO
#   2) ci\.env file (gitignored, interactive local fallback only)
#   3) interactive prompt (GITEA_TOKEN only)
#
# Order (same as ci/gitea-release.sh): a new release is created as a DRAFT, the assets are uploaded, and only then is
# it published (PATCH draft=false, with the prerelease flag and body; also applied to an existing release on a
# re-run). Gitea refuses a second asset with the same name: in a draft an existing asset is replaced by
# delete-then-upload; in a published release the new file is uploaded under a temporary name first, the old one
# deleted, and the new one renamed.

param(
    [Parameter(Mandatory=$true)][string]$Tag,
    [string]$Repo = '',
    [string]$DistDir = 'dist',
    [string]$Host_ = $env:GITEA_HOST,
    [string]$NotesFile = '',
    [switch]$DryRun,
    [switch]$Stable,
    [switch]$Prerelease,
    [switch]$NoPushTag
)
$ErrorActionPreference = 'Stop'
$envFile = Join-Path $PSScriptRoot '.env'
function Get-DotEnvValue([string]$key) {
    if (($env:RAGO_NO_DOTENV -eq '1') -or -not (Test-Path -LiteralPath $envFile -PathType Leaf)) { return '' }
    $envItem = Get-Item -LiteralPath $envFile
    if ($envItem.Attributes.HasFlag([System.IO.FileAttributes]::ReparsePoint)) { return '' }
    $matched = ''
    foreach ($line in (Get-Content -LiteralPath $envFile)) {
        if ($line -match "^\s*$key\s*=\s*(.+?)\s*$") {
            $val = $Matches[1]
            if (($val.StartsWith('"') -and $val.EndsWith('"')) -or ($val.StartsWith("'") -and $val.EndsWith("'"))) {
                $val = $val.Substring(1, $val.Length - 2)
            }
            $matched = $val
        }
    }
    return $matched
}

if (-not $Host_) { $Host_ = Get-DotEnvValue 'GITEA_HOST' }
if (-not $Host_) {
    if ($DryRun) {
        $Host_ = 'https://gitea.example.com'
    } else {
        throw 'GITEA_HOST is required (set $env:GITEA_HOST or GITEA_HOST in ci/.env)'
    }
}
$Host_ = $Host_.TrimEnd('/')

if ($Tag -notmatch '^v\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$') {
    throw "Invalid release tag '$Tag' (expected v<major>.<minor>.<patch>[-prerelease])"
}
$ver = $Tag.Substring(1)

if ($Host_ -notmatch '^https://[A-Za-z0-9.-]+(:\d+)?$') {
    throw "GITEA_HOST must be an https:// origin (got '$Host_')"
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$brandText = Get-Content "$repoRoot\cmake\RagoShellDefaults.cmake" -Raw
function Get-Brand([string]$key) {
    if ($brandText -match "set\($key\s+`"([^`"]+)`"\)") { return $Matches[1] }
    throw "$key not found in cmake/RagoShellDefaults.cmake"
}
$productName = Get-Brand 'RAGO_PRODUCT_NAME'
$productId   = Get-Brand 'RAGO_PRODUCT_ID'
if (-not $Repo) {
    $envRepo = if ($env:GITEA_REPO) { $env:GITEA_REPO } else { Get-DotEnvValue 'GITEA_REPO' }
    $Repo = if ($envRepo) { $envRepo } else { "$productId/$productId" }
}
if ($Repo -notmatch '^[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$' -or $Repo.Contains('..')) {
    throw "Invalid Gitea repository slug '$Repo'"
}

$isPrerelease = if ($Stable) {
    $false
} elseif ($Prerelease) {
    $true
} else {
    ($ver -match '-')
}

$resolvedDist = if ([System.IO.Path]::IsPathRooted($DistDir)) { $DistDir } else { Join-Path $repoRoot $DistDir }
if (-not (Test-Path -LiteralPath $resolvedDist -PathType Container)) {
    throw "Dist directory not found: $resolvedDist"
}

# Collect version-matched binaries, SPDX 2.3 SBOM (.spdx.json), and signed metadata files (non-empty regular files only, never reparse points).
$assets = @()
$binAssets = @(Get-ChildItem -Path $resolvedDist -Filter "$productId-$ver-*" -File -ErrorAction SilentlyContinue |
    Where-Object { -not $_.Attributes.HasFlag([System.IO.FileAttributes]::ReparsePoint) -and $_.Length -gt 0 })
$assets += $binAssets

foreach ($metaName in @('SHA256SUMS.txt', 'SHA256SUMS.txt.minisig', 'releases.json', 'releases.json.minisig')) {
    $metaPath = Join-Path $resolvedDist $metaName
    if (Test-Path -LiteralPath $metaPath -PathType Leaf) {
        $item = Get-Item -LiteralPath $metaPath
        if (-not $item.Attributes.HasFlag([System.IO.FileAttributes]::ReparsePoint) -and $item.Length -gt 0) {
            $assets += $item
        }
    }
}
$wasmDistDir = Join-Path $resolvedDist 'wasm'
foreach ($wasmName in @("$productId-wasm-demo.tar.gz", "$productId-wasm-demo.tar.gz.minisig")) {
    $wasmPath = Join-Path $wasmDistDir $wasmName
    if (Test-Path -LiteralPath $wasmPath -PathType Leaf) {
        $wItem = Get-Item -LiteralPath $wasmPath
        if (-not $wItem.Attributes.HasFlag([System.IO.FileAttributes]::ReparsePoint) -and $wItem.Length -gt 0) {
            $assets += $wItem
        }
    }
}

if ($assets.Count -eq 0) {
    throw "No $productId-$ver-* artifacts found in $resolvedDist"
}

if ($DryRun) {
    Write-Host "==> [dry-run] Gitea Releases target: $Host_/$Repo/releases/tag/$Tag (prerelease=$isPrerelease)"
    Write-Host "    [dry-run] create as draft (or reuse the existing release), upload, then publish (draft=false)"
    foreach ($asset in $assets) {
        Write-Host "    [dry-run] upload $($asset.Name)"
    }
    exit 0
}

$envFile = Join-Path $PSScriptRoot '.env'
if (-not $env:GITEA_TOKEN -and ($env:RAGO_NO_DOTENV -ne '1') -and (Test-Path -LiteralPath $envFile -PathType Leaf)) {
    $envItem = Get-Item -LiteralPath $envFile
    if (-not $envItem.Attributes.HasFlag([System.IO.FileAttributes]::ReparsePoint)) {
        Get-Content -LiteralPath $envFile | ForEach-Object {
            if ($_ -match '^\s*GITEA_TOKEN\s*=\s*(.+?)\s*$') {
                $val = $Matches[1]
                if (($val.StartsWith('"') -and $val.EndsWith('"')) -or ($val.StartsWith("'") -and $val.EndsWith("'"))) {
                    $val = $val.Substring(1, $val.Length - 2)
                }
                $env:GITEA_TOKEN = $val
            }
        }
    }
}
if (-not $env:GITEA_TOKEN) {
    $env:GITEA_TOKEN = Read-Host 'GITEA_TOKEN' -AsSecureString |
        ForEach-Object { [Runtime.InteropServices.Marshal]::PtrToStringAuto(
            [Runtime.InteropServices.Marshal]::SecureStringToBSTR($_)) }
}
if (-not $env:GITEA_TOKEN) { throw 'GITEA_TOKEN is required' }
if ($env:GITEA_TOKEN -match '[\x00-\x20\x7f"\\]') {
    throw 'GITEA_TOKEN contains invalid whitespace, control, quote, or backslash characters'
}

$headers = @{ Authorization = "token $env:GITEA_TOKEN" }

if (-not $NoPushTag) {
    git -C $repoRoot rev-parse -q --verify "refs/tags/$Tag" 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) {
        git -C $repoRoot tag -a $Tag -m "$productName $Tag"
        if ($LASTEXITCODE -ne 0) { throw "git tag failed ($LASTEXITCODE)" }
        git -C $repoRoot push origin $Tag
        if ($LASTEXITCODE -ne 0) { throw "git push tag failed ($LASTEXITCODE)" }
    }
}

$releaseBody = if ($NotesFile -and (Test-Path -LiteralPath $NotesFile -PathType Leaf)) {
    Get-Content -LiteralPath $NotesFile -Raw -Encoding UTF8
} else {
    "$productName $Tag"
}

Write-Host "==> creating/updating release $Tag on $Repo"
function Get-ReleaseBody([bool]$draft) {
    $json = @{
        tag_name   = $Tag
        name       = "$productName $Tag"
        body       = $releaseBody
        draft      = $draft
        prerelease = $isPrerelease
    } | ConvertTo-Json -Depth 4
    return [System.Text.Encoding]::UTF8.GetBytes($json)
}

# A new release starts as a draft: nobody sees it until every asset is uploaded.
try {
    $release = Invoke-RestMethod -Headers $headers -Method Post `
        -Uri "$Host_/api/v1/repos/$Repo/releases" -Body (Get-ReleaseBody $true) -ContentType 'application/json; charset=utf-8'
} catch {
    Write-Host "==> release exists, fetching id"
    $release = [pscustomobject]@{ id = $null }
}

if (-not $release.id) {
    $release = Invoke-RestMethod -Headers $headers `
        -Uri "$Host_/api/v1/repos/$Repo/releases/tags/$Tag"
}
if (-not $release.id) { throw "no release id in the Gitea response for $Tag" }
$isDraft = ($release.draft -eq $true)
$assetsUrl = "$Host_/api/v1/repos/$Repo/releases/$($release.id)/assets"

$existingAssets = @(Invoke-RestMethod -Headers $headers -Method Get -Uri $assetsUrl)

# Upload through curl.exe with the token in a --config on stdin (never in argv); returns the parsed asset JSON.
function Send-Asset([string]$name, [string]$path) {
    $curlConfig = "header = `"Authorization: token $($env:GITEA_TOKEN)`"`n"
    $out = $curlConfig | & curl.exe -fsS --config - -X POST "$assetsUrl?name=$name" -F "attachment=@$path"
    if ($LASTEXITCODE -ne 0) { throw "upload failed: $name" }
    return ($out | Out-String | ConvertFrom-Json)
}

foreach ($asset in $assets) {
    $existing = $existingAssets | Where-Object { $_.name -eq $asset.Name } | Select-Object -First 1
    if (-not ($existing -and $existing.id)) {
        Write-Host "==> uploading $($asset.Name)"
        Send-Asset $asset.Name $asset.FullName | Out-Null
    } elseif ($isDraft) {
        # still a draft: nobody can download it yet, so the short gap of delete-then-upload is harmless
        Write-Host "==> replacing existing asset $($asset.Name) (id=$($existing.id)) in the draft"
        Invoke-RestMethod -Headers $headers -Method Delete -Uri "$assetsUrl/$($existing.id)" | Out-Null
        Send-Asset $asset.Name $asset.FullName | Out-Null
    } else {
        # published: upload first under a temporary name (same extension), then delete the old asset and rename
        $tmpName = "uploading-$($asset.Name)"
        $staleTmp = $existingAssets | Where-Object { $_.name -eq $tmpName } | Select-Object -First 1
        if ($staleTmp -and $staleTmp.id) {
            Invoke-RestMethod -Headers $headers -Method Delete -Uri "$assetsUrl/$($staleTmp.id)" | Out-Null
        }
        Write-Host "==> replacing existing asset $($asset.Name) (id=$($existing.id)): uploading the new file as $tmpName first"
        $new = Send-Asset $tmpName $asset.FullName
        if (-not $new.id) { throw "upload of $($asset.Name) returned no asset id; the old asset is unchanged" }
        Invoke-RestMethod -Headers $headers -Method Delete -Uri "$assetsUrl/$($existing.id)" | Out-Null
        $renameBody = [System.Text.Encoding]::UTF8.GetBytes((@{ name = $asset.Name } | ConvertTo-Json))
        try {
            Invoke-RestMethod -Headers $headers -Method Patch -Uri "$assetsUrl/$($new.id)" `
                -Body $renameBody -ContentType 'application/json; charset=utf-8' | Out-Null
        } catch {
            throw "$($asset.Name) is uploaded as $tmpName (id=$($new.id)) but the rename failed; rename it in Gitea or re-run"
        }
    }
}

# Publish last (and refresh name, body and the prerelease flag on an existing release too).
Write-Host "==> publishing release $Tag (draft=false, prerelease=$isPrerelease)"
Invoke-RestMethod -Headers $headers -Method Patch -Uri "$Host_/api/v1/repos/$Repo/releases/$($release.id)" `
    -Body (Get-ReleaseBody $false) -ContentType 'application/json; charset=utf-8' | Out-Null

Write-Host "==> Gitea release published ($($assets.Count) asset(s)): $Host_/$Repo/releases/tag/$Tag"
