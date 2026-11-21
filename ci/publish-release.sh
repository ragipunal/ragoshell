#!/usr/bin/env bash
# ci/publish-release.sh -- Generate, sign, verify, and publish RagoShell release artifacts
# to Cloudflare R2 (`dl.<domain>`), GitHub Releases, Gitea Releases, and optional live hooks.
#
# Usage:
#   ./ci/publish-release.sh --dry-run [options] [version]
#   ./ci/publish-release.sh [options] [version]
#
# Security & atomicity guarantees (docs/ReleasePipeline.md):
# - Never reads, sources, or prints ci/.env (sets RAGO_NO_DOTENV=1).
# - Reads brand and domain strictly from cmake/RagoShellDefaults.cmake.
# - Refuses live publishing unless Minisign (`minisign -Vm` + trusted comment binding)
#   and SHA-256 / byte-size checks pass on both releases.json and SHA256SUMS.txt.
# - Uploads immutable versioned objects (`releases/<version>/*`, max-age=31536000, immutable)
#   and forge releases FIRST, and updates root `releases.json.minisig` + `releases.json`
#   (`Cache-Control: public, max-age=300`) LAST so clients never hit 404 or mismatched files.
# - A pre-release (version with a SemVer pre-release part, or --prerelease) publishes only the versioned objects
#   and the forge releases: the root manifest, its signature and the pulse/site hooks are skipped (and the skip is
#   printed), because every stable client reads the root manifest.

set -euo pipefail
set +x
export RAGO_NO_DOTENV=1

root="$(cd "$(dirname "$0")/.." && pwd)"

brand() { sed -n "s/^[[:space:]]*set($1 \"\\(.*\\)\")\$/\\1/p" "$root/cmake/RagoShellDefaults.cmake"; }
cmake_ver() { sed -n "s/^set($1 \"\\(.*\\)\")\$/\\1/p" "$root/CMakeLists.txt"; }

product_id="$(brand RAGO_PRODUCT_ID)"
product_name="$(brand RAGO_PRODUCT_NAME)"
github_owner="$(brand RAGO_GITHUB_OWNER)"
[ -n "$github_owner" ] || github_owner="$product_id"
domain="$(brand RAGO_DOMAIN)"
if [ -z "$product_id" ] || [ -z "$product_name" ] || [ -z "$domain" ]; then
    echo "error: failed to read brand defaults from cmake/RagoShellDefaults.cmake" >&2
    exit 1
fi

default_version="$(cmake_ver PROJECT_VERSION_MAJOR).$(cmake_ver PROJECT_VERSION_MINOR).$(cmake_ver PROJECT_VERSION_PATCH)$(cmake_ver PROJECT_VERSION_BUILD)"

dry_run=0
skip_generate=0
skip_sign=0
verify_node_only=0
allow_test_pubkey=0
allow_custom_endpoints=0
dist_dir="$root/dist"
version_arg=""
targets_csv="r2,github,gitea"
prerelease_mode="auto"
mandatory_flag=0
min_supported_ver=""
published_at=""
android_store_url="${RAGO_ANDROID_STORE_URL:-}"
ios_store_url="${RAGO_IOS_STORE_URL:-}"
require_platforms=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --dry-run)
            dry_run=1
            shift
            ;;
        --skip-generate)
            skip_generate=1
            shift
            ;;
        --skip-sign)
            skip_sign=1
            shift
            ;;
        --verify-node-only)
            verify_node_only=1
            shift
            ;;
        --allow-test-pubkey)
            allow_test_pubkey=1
            shift
            ;;
        --allow-custom-endpoints)
            allow_custom_endpoints=1
            shift
            ;;
        --dist)
            dist_dir="${2:?error: --dist requires a directory path}"
            shift 2
            ;;
        --version)
            version_arg="${2:?error: --version requires a version}"
            shift 2
            ;;
        --targets)
            targets_csv="${2:?error: --targets requires comma-separated targets (r2,github,gitea,pulse,site)}"
            shift 2
            ;;
        --stable)
            prerelease_mode="false"
            shift
            ;;
        --prerelease)
            prerelease_mode="true"
            shift
            ;;
        --mandatory)
            mandatory_flag=1
            shift
            ;;
        --min-supported-version)
            min_supported_ver="${2:?error: --min-supported-version requires a version}"
            shift 2
            ;;
        --published-at)
            published_at="${2:?error: --published-at requires YYYY-MM-DD}"
            shift 2
            ;;
        --android-store-url)
            android_store_url="${2:?error: --android-store-url requires an https:// URL}"
            shift 2
            ;;
        --ios-store-url)
            ios_store_url="${2:?error: --ios-store-url requires an https:// URL}"
            shift 2
            ;;
        --require-platforms)
            require_platforms="${2:?error: --require-platforms requires comma-separated platform names}"
            shift 2
            ;;
        --help|-h)
            cat <<'EOF'
Usage: ./ci/publish-release.sh [options] [version]

Options:
  --dry-run                        Validate everything and print exact upload & Cache-Control plan without network calls
  --version <ver>                  Release version or v-tag (defaults to CMakeLists.txt PROJECT_VERSION)
  --dist <dir>                     Artifact directory (default: ./dist)
  --targets <csv>                  Comma-separated targets: r2,github,gitea,pulse,site (default: r2,github,gitea)
  --skip-generate                  Use existing releases.json & SHA256SUMS.txt in --dist (still verifies them)
  --skip-sign                      Skip Minisign verification ONLY when combined with --dry-run
  --verify-node-only               With --dry-run, allow Node Ed25519 verification when minisign CLI is absent
  --stable / --prerelease          Override pre-release detection (default: prerelease iff version contains '-');
                                   a pre-release never updates the root releases.json or runs the pulse/site hooks
  --mandatory                      Mark release as mandatory in generated releases.json
  --min-supported-version <ver>    Minimum supported version in generated releases.json
  --published-at <YYYY-MM-DD>      Override UTC publication date
  --android-store-url <https://..> Optional Google Play listing URL
  --ios-store-url <https://..>     Optional Apple App Store listing URL
  --require-platforms <csv>        Required primary platforms (default: windows,macos,linux)

Environment (Cloudflare R2):
  R2_BUCKET                        Target R2 bucket name (e.g. ragoshell-releases)
  R2_ACCOUNT_ID                    Cloudflare account ID (builds https://<id>.r2.cloudflarestorage.com)
  R2_ENDPOINT_URL                  Explicit S3-compatible https:// endpoint (overrides R2_ACCOUNT_ID)
  R2_ACCESS_KEY_ID                 Scoped R2 S3 API Access Key ID
  R2_SECRET_ACCESS_KEY             Scoped R2 S3 API Secret Access Key

Environment (GitHub & Gitea Releases):
  GH_TOKEN / GITHUB_TOKEN          GitHub token with contents:write
  GITHUB_REPOSITORY                GitHub owner/repo (default: <github_owner>/<product_id>)
  GITEA_TOKEN                      Repo-scoped Gitea release token
  GITEA_HOST                       Gitea base URL (required for live `gitea` target)
  GITEA_REPO                       Gitea owner/repo (default: <product_id>/<product_id>)

Environment (Optional Post-Publish Hooks):
  PULSE_API_ADMIN_URL              e.g. https://api.<domain>/v1/admin/releases
  PULSE_ADMIN_TOKEN                Admin bearer token for release-pulse-api
  RAGO_SITE_RELEASES_SSH           e.g. deploy@host:/srv/site (atomic scp + mv for static nginx root)
EOF
            exit 0
            ;;
        --*)
            echo "error: unknown flag '$1' (run ./ci/publish-release.sh --help)" >&2
            exit 1
            ;;
        *)
            if [ -z "$version_arg" ]; then
                version_arg="$1"
                shift
            else
                echo "error: unexpected positional argument '$1'" >&2
                exit 1
            fi
            ;;
    esac
done

if [ "$skip_sign" -eq 1 ] && [ "$dry_run" -ne 1 ]; then
    echo "error: --skip-sign is strictly forbidden outside --dry-run mode." >&2
    exit 1
fi
if [ "$verify_node_only" -eq 1 ] && [ "$dry_run" -ne 1 ]; then
    echo "error: --verify-node-only is strictly forbidden outside --dry-run mode." >&2
    exit 1
fi
if [ "$allow_test_pubkey" -eq 1 ] && [ "$dry_run" -ne 1 ]; then
    echo "error: --allow-test-pubkey is strictly forbidden outside --dry-run mode." >&2
    exit 1
fi

command -v node >/dev/null 2>&1 || { echo "error: required tool 'node' not found on PATH" >&2; exit 1; }

raw_version="${version_arg:-$default_version}"
version="$(RAGO_RAW_VER="$raw_version" node --input-type=module -e '
import { normalizeAndValidateVersion } from "./ci/generate-releases-json.mjs";
process.stdout.write(normalizeAndValidateVersion(process.env.RAGO_RAW_VER));
')"
tag="v$version"

if [ "$prerelease_mode" = "auto" ]; then
    case "$version" in
        *-*) is_prerelease="true" ;;
        *)   is_prerelease="false" ;;
    esac
else
    is_prerelease="$prerelease_mode"
fi

# Stable clients read the root releases.json (R2, the release-pulse-api hook and the static site hook all serve that
# same manifest). A pre-release must never reach it: it goes only to releases/<version>/ and the forge releases.
# A pre-release version stays a pre-release here even with --stable.
publish_root=1
case "$version" in
    *-*) publish_root=0 ;;
esac
[ "$is_prerelease" = "true" ] && publish_root=0

# Parse and validate target list
want_r2=0
want_github=0
want_gitea=0
want_pulse=0
want_site=0

IFS=',' read -r -a raw_targets <<< "$targets_csv"
for t in "${raw_targets[@]}"; do
    trimmed="$(printf '%s' "$t" | tr -d '[:space:]')"
    [ -n "$trimmed" ] || continue
    case "$trimmed" in
        r2)     want_r2=1 ;;
        github) want_github=1 ;;
        gitea)  want_gitea=1 ;;
        pulse)  want_pulse=1 ;;
        site)   want_site=1 ;;
        *)
            echo "error: unknown publish target '$trimmed' (allowed: r2,github,gitea,pulse,site)" >&2
            exit 1
            ;;
    esac
done

[ -d "$dist_dir" ] || { echo "error: dist directory does not exist: $dist_dir" >&2; exit 1; }
dist_dir="$(cd "$dist_dir" && pwd)"

umask 077
pub_tmp="$(mktemp -d "${TMPDIR:-/tmp}/${product_id}-publish.XXXXXX")"
chmod 700 "$pub_tmp"
trap 'rm -rf "$pub_tmp"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
notes_file="$pub_tmp/RELEASE_NOTES.md"

# 1. Generate deterministic SPDX 2.3 SBOM + releases.json + SHA256SUMS.txt (unless skipped) and strictly verify
if [ "$skip_generate" -eq 0 ]; then
    node "$root/ci/generate-sbom.mjs" --version "$version" --dist "$dist_dir"
    gen_args=(--version "$version" --dist "$dist_dir" --notes-markdown-out "$notes_file")
    [ "$mandatory_flag" -eq 1 ] && gen_args+=(--mandatory)
    [ -n "$min_supported_ver" ] && gen_args+=(--min-supported-version "$min_supported_ver")
    [ -n "$published_at" ] && gen_args+=(--published-at "$published_at")
    [ -n "$android_store_url" ] && gen_args+=(--android-store-url "$android_store_url")
    [ -n "$ios_store_url" ] && gen_args+=(--ios-store-url "$ios_store_url")
    if [ -n "$require_platforms" ]; then
        gen_args+=(--require-platforms "$require_platforms")
    fi
    node "$root/ci/generate-releases-json.mjs" "${gen_args[@]}"
else
    # Build Markdown notes for GitHub/Gitea release bodies from verified manifest + CHANGELOG.
    RAGO_DIST="$dist_dir" RAGO_NOTES_OUT="$notes_file" RAGO_ROOT="$root" node --input-type=module - <<'EOF'
import fs from 'node:fs';
import path from 'node:path';
import { readBrandDefaults, parseChangelog, formatMarkdownReleaseNotes } from './ci/generate-releases-json.mjs';

const repoRoot = process.env.RAGO_ROOT;
const distDir = process.env.RAGO_DIST;
const brand = readBrandDefaults(repoRoot);
const manifest = JSON.parse(fs.readFileSync(path.join(distDir, 'releases.json'), 'utf8'));
const clPath = path.join(repoRoot, 'CHANGELOG');
let entry = null;
if (fs.existsSync(clPath)) {
  entry = parseChangelog(fs.readFileSync(clPath, 'utf8')).find((e) => e.version === manifest.latestVersion) || null;
}
fs.writeFileSync(
  process.env.RAGO_NOTES_OUT,
  formatMarkdownReleaseNotes(brand.productName, manifest.latestVersion, entry, manifest.artifacts, manifest.sbom || null),
  'utf8'
);
EOF
fi

node "$root/ci/generate-releases-json.mjs" --verify-only --dist "$dist_dir" --version "$version"
if [ -f "$dist_dir/$product_id-$version-sbom.spdx.json" ]; then
    node "$root/ci/generate-sbom.mjs" --verify-only --dist "$dist_dir" --version "$version"
fi

# 2. Sign (if secret key provided) and run mandatory signature verification BEFORE any network step
if [ "$skip_sign" -eq 0 ]; then
    sign_extra_args=()
    [ "$allow_test_pubkey" -eq 1 ] && sign_extra_args+=(--allow-test-pubkey)
    if [ -n "${RAGO_MINISIGN_SECRET_KEY:-}" ] || [ -n "${RAGO_MINISIGN_SECRET_KEY_FILE:-}" ]; then
        if [ "$dry_run" -eq 0 ] || command -v minisign >/dev/null 2>&1; then
            "$root/ci/sign-release.sh" --dist "$dist_dir" --version "$version" ${sign_extra_args[@]+"${sign_extra_args[@]}"}
        fi
    fi

    sign_verify_args=(--verify-only --dist "$dist_dir" --version "$version")
    [ "$verify_node_only" -eq 1 ] && sign_verify_args+=(--verify-node-only)
    [ "$allow_test_pubkey" -eq 1 ] && sign_verify_args+=(--allow-test-pubkey)
    "$root/ci/sign-release.sh" "${sign_verify_args[@]}"
else
    echo "==> [dry-run] skipping Minisign verification (--skip-sign)"
fi

# Collect exact verified artifact list (including optional SPDX 2.3 SBOM) from releases.json
artifact_names=()
while IFS= read -r line; do
    [ -n "$line" ] && artifact_names+=("$line")
done < <(RAGO_MANIFEST="$dist_dir/releases.json" node -e '
const fs = require("node:fs");
const m = JSON.parse(fs.readFileSync(process.env.RAGO_MANIFEST, "utf8"));
for (const a of m.artifacts) process.stdout.write(a.name + "\n");
if (m.sbom && m.sbom.name) process.stdout.write(m.sbom.name + "\n");
')

mime_type_for() {
    case "$1" in
        *.exe)       printf 'application/vnd.microsoft.portable-executable' ;;
        *.msi)       printf 'application/x-msi' ;;
        *.zip)       printf 'application/zip' ;;
        *.dmg)       printf 'application/x-apple-diskimage' ;;
        *.tar.gz)    printf 'application/gzip' ;;
        *.AppImage)  printf 'application/vnd.appimage' ;;
        *.apk)       printf 'application/vnd.android.package-archive' ;;
        *.aab)       printf 'application/x-authorware-bin' ;;
        *.spdx.json) printf 'application/spdx+json; charset=utf-8' ;;
        *.json)      printf 'application/json; charset=utf-8' ;;
        *.txt|*.minisig) printf 'text/plain; charset=utf-8' ;;
        *)           printf 'application/octet-stream' ;;
    esac
}

# Validate R2 settings upfront before uploading anything
r2_endpoint=""
r2_bucket="${R2_BUCKET:-${product_id}-releases}"
if [ "$want_r2" -eq 1 ]; then
    if [ -n "${R2_ENDPOINT_URL:-}" ]; then
        r2_endpoint="${R2_ENDPOINT_URL%/}"
    elif [ -n "${R2_ACCOUNT_ID:-}" ]; then
        if ! [[ "$R2_ACCOUNT_ID" =~ ^[A-Za-z0-9]{16,64}$ ]]; then
            echo "error: invalid R2_ACCOUNT_ID format." >&2
            exit 1
        fi
        r2_endpoint="https://${R2_ACCOUNT_ID}.r2.cloudflarestorage.com"
    elif [ "$dry_run" -eq 1 ]; then
        r2_endpoint="https://<R2_ACCOUNT_ID>.r2.cloudflarestorage.com"
    else
        echo "error: R2 upload requires R2_ENDPOINT_URL or R2_ACCOUNT_ID." >&2
        exit 1
    fi

    if ! [[ "$r2_bucket" =~ ^[a-z0-9][a-z0-9.-]{1,61}[a-z0-9]$ ]]; then
        echo "error: invalid R2_BUCKET name '$r2_bucket'." >&2
        exit 1
    fi

    if [ -n "${R2_ENDPOINT_URL:-}" ] || [ "$dry_run" -eq 0 ]; then
        if [ "$allow_custom_endpoints" -eq 1 ]; then
            if ! [[ "$r2_endpoint" =~ ^https://[A-Za-z0-9.-]+(:[0-9]+)?$ ]]; then
                echo "error: R2 endpoint must be an https:// origin (got '$r2_endpoint')." >&2
                exit 1
            fi
        else
            if ! [[ "$r2_endpoint" =~ ^https://[A-Za-z0-9-]+(\.eu|\.fedramp-moderate)?\.r2\.cloudflarestorage\.com$ ]]; then
                echo "error: R2_ENDPOINT_URL must be an official Cloudflare R2 S3 endpoint (*.r2.cloudflarestorage.com); got '$r2_endpoint'." >&2
                exit 1
            fi
        fi
    fi

    if [ "$dry_run" -eq 0 ]; then
        : "${R2_ACCESS_KEY_ID:?error: R2_ACCESS_KEY_ID is required for R2 upload}"
        : "${R2_SECRET_ACCESS_KEY:?error: R2_SECRET_ACCESS_KEY is required for R2 upload}"
        command -v aws >/dev/null 2>&1 || { echo "error: 'aws' CLI is required on PATH for R2 S3 uploads." >&2; exit 1; }
    fi
fi

r2_put_object() {
    local local_file="$1"
    local object_key="$2"
    local cache_control="$3"
    local content_type
    content_type="$(mime_type_for "$local_file")"

    if [ "$dry_run" -eq 1 ]; then
        echo "    [dry-run] R2 PUT s3://$r2_bucket/$object_key <- $(basename "$local_file") [Cache-Control: $cache_control] [Content-Type: $content_type]"
        return 0
    fi

    echo "==> R2 PUT s3://$r2_bucket/$object_key [Cache-Control: $cache_control]"
    AWS_ACCESS_KEY_ID="$R2_ACCESS_KEY_ID" \
    AWS_SECRET_ACCESS_KEY="$R2_SECRET_ACCESS_KEY" \
    AWS_DEFAULT_REGION="auto" \
    AWS_EC2_METADATA_DISABLED="true" \
        aws s3api put-object \
            --endpoint-url "$r2_endpoint" \
            --bucket "$r2_bucket" \
            --key "$object_key" \
            --body "$local_file" \
            --cache-control "$cache_control" \
            --content-type "$content_type" >/dev/null
}

# 3. Step A: Cloudflare R2 versioned immutable directory (`releases/<version>/*`)
if [ "$want_r2" -eq 1 ]; then
    immutable_cc="public, max-age=31536000, immutable"
    echo "==> Step 1/4: Cloudflare R2 versioned artifacts (s3://$r2_bucket/releases/$version/*)"
    for name in "${artifact_names[@]}"; do
        r2_put_object "$dist_dir/$name" "releases/$version/$name" "$immutable_cc"
    done
    for meta in SHA256SUMS.txt SHA256SUMS.txt.minisig releases.json releases.json.minisig; do
        if [ -f "$dist_dir/$meta" ]; then
            r2_put_object "$dist_dir/$meta" "releases/$version/$meta" "$immutable_cc"
        fi
    done
fi

# 4. Step B: GitHub Releases (`v<version>`)
if [ "$want_github" -eq 1 ]; then
    gh_repo="${GITHUB_REPOSITORY:-$github_owner/$product_id}"
    if ! [[ "$gh_repo" =~ ^[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$ ]] || [[ "$gh_repo" == *".."* ]]; then
        echo "error: invalid GITHUB_REPOSITORY '$gh_repo'" >&2
        exit 1
    fi

    gh_assets=()
    for name in "${artifact_names[@]}"; do
        gh_assets+=("$dist_dir/$name")
    done
    for meta in SHA256SUMS.txt SHA256SUMS.txt.minisig releases.json releases.json.minisig; do
        if [ -f "$dist_dir/$meta" ]; then
            gh_assets+=("$dist_dir/$meta")
        fi
    done

    if [ "$dry_run" -eq 1 ]; then
        echo "==> [dry-run] GitHub Releases target: https://github.com/$gh_repo/releases/tag/$tag (prerelease=$is_prerelease)"
        for a in "${gh_assets[@]}"; do
            echo "    [dry-run] upload $(basename "$a")"
        done
    else
        export GH_TOKEN="${GH_TOKEN:-${GITHUB_TOKEN:-}}"
        : "${GH_TOKEN:?error: GH_TOKEN or GITHUB_TOKEN is required for GitHub Releases}"
        command -v gh >/dev/null 2>&1 || { echo "error: 'gh' CLI is required on PATH for GitHub Releases" >&2; exit 1; }

        echo "==> publishing GitHub Release $tag on $gh_repo"
        if gh release view "$tag" --repo "$gh_repo" >/dev/null 2>&1; then
            gh release upload "$tag" "${gh_assets[@]}" --repo "$gh_repo" --clobber
            gh release edit "$tag" --repo "$gh_repo" --title "$product_name $tag" --notes-file "$notes_file" --prerelease="$is_prerelease"
        else
            create_flags=(--repo "$gh_repo" --title "$product_name $tag" --notes-file "$notes_file")
            if [ "$is_prerelease" = "true" ]; then
                create_flags+=(--prerelease)
            else
                create_flags+=(--latest)
            fi
            gh release create "$tag" "${gh_assets[@]}" "${create_flags[@]}"
        fi
    fi
fi

# 5. Step C: Gitea Releases (`v<version>`)
if [ "$want_gitea" -eq 1 ]; then
    gitea_flags=(--no-push-tag --notes-file "$notes_file")
    [ "$dry_run" -eq 1 ] && gitea_flags+=(--dry-run)
    if [ "$is_prerelease" = "true" ]; then
        gitea_flags+=(--prerelease)
    else
        gitea_flags+=(--stable)
    fi
    "$root/ci/gitea-release.sh" "${gitea_flags[@]}" "$tag" "${GITEA_REPO:-$product_id/$product_id}" "$dist_dir"
fi

# 6. Step D (Atomic Final Cutover): Cloudflare R2 root `releases.json.minisig` + `releases.json`
if [ "$want_r2" -eq 1 ] && [ "$publish_root" -eq 0 ]; then
    echo "==> Step 4/4: skipped for pre-release $version: root releases.json.minisig + releases.json are NOT updated (stable clients keep the current manifest; this release is only under releases/$version/)"
elif [ "$want_r2" -eq 1 ]; then
    root_cc="public, max-age=300"
    echo "==> Step 4/4: Cloudflare R2 atomic root manifest update (Cache-Control: $root_cc)"
    if [ -f "$dist_dir/releases.json.minisig" ]; then
        r2_put_object "$dist_dir/releases.json.minisig" "releases.json.minisig" "$root_cc"
    fi
    r2_put_object "$dist_dir/releases.json" "releases.json" "$root_cc"
fi

# 7. Optional Post-Publish Hook 1: Live reload `release-pulse-api` (`POST /v1/admin/releases`)
if [ "$want_pulse" -eq 1 ] && [ "$publish_root" -eq 0 ]; then
    echo "==> skipped release-pulse-api hook for pre-release $version (it would serve the pre-release manifest to stable clients)"
elif [ "$want_pulse" -eq 1 ]; then
    pulse_url="${PULSE_API_ADMIN_URL:-https://api.$domain/v1/admin/releases}"
    if [ "$allow_custom_endpoints" -eq 1 ]; then
        if ! [[ "$pulse_url" =~ ^https://[A-Za-z0-9.-]+(:[0-9]+)?/v1/admin/releases$ ]] && ! [[ "$pulse_url" =~ ^http://127\.0\.0\.1:[0-9]+/v1/admin/releases$ ]]; then
            echo "error: PULSE_API_ADMIN_URL must end with /v1/admin/releases over https:// (or http://127.0.0.1 for local testing)." >&2
            exit 1
        fi
    else
        expected_prod="https://api.$domain/v1/admin/releases"
        expected_stg="https://api-staging.$domain/v1/admin/releases"
        if [ "$pulse_url" != "$expected_prod" ] && [ "$pulse_url" != "$expected_stg" ] && ! [[ "$pulse_url" =~ ^http://127\.0\.0\.1:[0-9]+/v1/admin/releases$ ]]; then
            echo "error: PULSE_API_ADMIN_URL '$pulse_url' is not an allowlisted API host ($expected_prod or $expected_stg)." >&2
            exit 1
        fi
    fi
    if [ "$dry_run" -eq 1 ]; then
        echo "==> [dry-run] release-pulse-api hook: POST $pulse_url <- releases.json"
    else
        : "${PULSE_ADMIN_TOKEN:?error: PULSE_ADMIN_TOKEN is required when publishing to release-pulse-api}"
        if [[ "$PULSE_ADMIN_TOKEN" =~ [[:cntrl:][:space:]\"\\] ]]; then
            echo "error: PULSE_ADMIN_TOKEN contains invalid whitespace, control, quote, or backslash characters" >&2
            exit 1
        fi
        echo "==> updating release-pulse-api manifest at $pulse_url"
        curl -fsS --config - -X POST "$pulse_url" \
            -H 'Content-Type: application/json; charset=utf-8' \
            --data-binary "@$dist_dir/releases.json" >/dev/null <<EOF
header = "Authorization: Bearer ${PULSE_ADMIN_TOKEN}"
EOF
    fi
fi

# 8. Optional Post-Publish Hook 2: Static site / dl directory atomic update via forced command (`deploy-receive.sh releases`)
if [ "$want_site" -eq 1 ] && [ "$publish_root" -eq 0 ]; then
    echo "==> skipped static site hook for pre-release $version (it would replace the site's releases.json for stable clients)"
elif [ "$want_site" -eq 1 ]; then
    ssh_target="${RAGO_SITE_RELEASES_SSH:-deploy@$domain}"
    if ! [[ "$ssh_target" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*@[A-Za-z0-9][A-Za-z0-9.-]*$ ]] || [[ "$ssh_target" == *".."* ]]; then
        echo "error: invalid RAGO_SITE_RELEASES_SSH '$ssh_target' (expected user@host starting with alphanumeric)" >&2
        exit 1
    fi
    site_cmd="${RAGO_SITE_RELEASES_CMD:-releases}"
    case "$site_cmd" in
        releases|staging-releases) ;;
        *)
            echo "error: invalid RAGO_SITE_RELEASES_CMD '$site_cmd' (expected 'releases' or 'staging-releases')" >&2
            exit 1
            ;;
    esac
    if [ "$dry_run" -eq 1 ]; then
        echo "==> [dry-run] static site hook: stream releases.json + releases.json.minisig -> ssh -- $ssh_target $site_cmd"
    else
        : "${RAGO_SITE_RELEASES_SSH:?error: RAGO_SITE_RELEASES_SSH (user@host) is required for site target}"
        [ -f "$dist_dir/releases.json.minisig" ] && [ ! -L "$dist_dir/releases.json.minisig" ] || {
            echo "error: releases.json.minisig is required for live static site sync" >&2
            exit 1
        }
        COPYFILE_DISABLE=1 tar -czf - -C "$dist_dir" releases.json releases.json.minisig \
            | ssh -o BatchMode=yes -o StrictHostKeyChecking=yes -- "$RAGO_SITE_RELEASES_SSH" "$site_cmd"
    fi
fi

echo "==> release pipeline finished (${product_name} ${version}, dry_run=${dry_run})"
