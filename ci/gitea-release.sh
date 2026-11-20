#!/usr/bin/env bash
# ci/gitea-release.sh -- create/update a Gitea release and upload dist/ assets + signed metadata.
#
#   GITEA_TOKEN=<api-token> ./ci/gitea-release.sh [--dry-run] [--stable|--prerelease] [--notes-file FILE] [--no-push-tag] <tag> [repo] [dist-dir]
#
# Env:  GITEA_TOKEN     (required unless --dry-run; create a repo-scoped release token at Gitea -> Settings -> Applications)
#       GITEA_HOST      (required unless --dry-run, e.g. https://git.example.com)
#       GITEA_REPO      (optional owner/repo slug; defaults to <product_id>/<product_id>)
#       RAGO_NO_DOTENV  (when "1", never source ci/.env; set automatically by ci/publish-release.sh)
#
# Order: a new release is created as a DRAFT, the assets are uploaded, and only then is it published (PATCH
# draft=false, with the prerelease flag and body; also applied to an existing release on a re-run). Gitea refuses a
# second asset with the same name, so on a re-run an existing asset is replaced by delete-then-upload only while the
# release is still a draft; on a published release the new file is uploaded under a temporary name first, the old one
# deleted, and the new one renamed, so a published release never loses an asset to a failed upload.

set -euo pipefail
set +x

root="$(cd "$(dirname "$0")/.." && pwd)"
brand() { sed -n "s/^set($1 \"\\(.*\\)\")\$/\\1/p" "$root/cmake/RagoShellDefaults.cmake"; }
product_id="$(brand RAGO_PRODUCT_ID)"
product_name="$(brand RAGO_PRODUCT_NAME)"

dry_run=0
prerelease_mode="auto"
notes_file=""
no_push_tag=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --dry-run)
            dry_run=1
            shift
            ;;
        --stable)
            prerelease_mode="false"
            shift
            ;;
        --prerelease)
            prerelease_mode="true"
            shift
            ;;
        --notes-file)
            notes_file="${2:?error: --notes-file requires a path}"
            shift 2
            ;;
        --no-push-tag)
            no_push_tag=1
            shift
            ;;
        --help|-h)
            cat <<'EOF'
Usage: ./ci/gitea-release.sh [--dry-run] [--stable|--prerelease] [--notes-file <path>] [--no-push-tag] <tag> [repo] [dist-dir]
EOF
            exit 0
            ;;
        --*)
            echo "error: unknown flag '$1'" >&2
            exit 1
            ;;
        *)
            break
            ;;
    esac
done

dotenv_file="$(dirname "$0")/.env"
read_dotenv_var() {
    local var_name="$1"
    if [ "${RAGO_NO_DOTENV:-0}" != "1" ] && [ -f "$dotenv_file" ] && [ ! -L "$dotenv_file" ]; then
        sed -n -E "s/^[[:space:]]*${var_name}[[:space:]]*=[[:space:]]*(.*)[[:space:]]*\$/\\1/p" "$dotenv_file" \
            | tail -n 1 \
            | sed -E "s/^\"(.*)\"\$/\\1/; s/^'(.*)'\$/\\1/"
    fi
}

if [ -z "${GITEA_HOST:-}" ]; then
    GITEA_HOST="$(read_dotenv_var GITEA_HOST)"
fi
if [ -z "${GITEA_REPO:-}" ]; then
    GITEA_REPO="$(read_dotenv_var GITEA_REPO)"
fi

tag="${1:?usage: gitea-release.sh [options] <tag> [repo] [dist-dir]}"
repo="${2:-${GITEA_REPO:-$product_id/$product_id}}"
dist="${3:-$root/dist}"
if [ "$dry_run" -eq 1 ]; then
    host="${GITEA_HOST:-https://gitea.example.com}"
else
    host="${GITEA_HOST:?error: GITEA_HOST is required}"
fi
host="${host%/}"

if ! [[ "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$ ]]; then
    echo "error: invalid release tag '$tag' (expected v<major>.<minor>.<patch>[-prerelease])" >&2
    exit 1
fi
ver="${tag#v}"

if ! [[ "$host" =~ ^https://[A-Za-z0-9.-]+(:[0-9]+)?$ ]]; then
    echo "error: GITEA_HOST must be an https:// origin (got '$host')" >&2
    exit 1
fi
if ! [[ "$repo" =~ ^[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$ ]] || [[ "$repo" == *".."* ]]; then
    echo "error: invalid Gitea repository slug '$repo'" >&2
    exit 1
fi

if [ "$prerelease_mode" = "auto" ]; then
    case "$ver" in
        *-*) is_prerelease="true" ;;
        *)   is_prerelease="false" ;;
    esac
else
    is_prerelease="$prerelease_mode"
fi

[ -d "$dist" ] || { echo "error: dist directory not found: $dist" >&2; exit 1; }
dist="$(cd "$dist" && pwd)"

# Collect version-matched binaries, SPDX 2.3 SBOM (.spdx.json), and signed metadata files (non-empty regular files only, never symlinks).
assets=()
for f in "$dist"/"$product_id-$ver"-*; do
    if [ -f "$f" ] && [ ! -L "$f" ] && [ -s "$f" ]; then
        assets+=("$f")
    fi
done
for meta in SHA256SUMS.txt SHA256SUMS.txt.minisig releases.json releases.json.minisig; do
    if [ -f "$dist/$meta" ] && [ ! -L "$dist/$meta" ] && [ -s "$dist/$meta" ]; then
        assets+=("$dist/$meta")
    fi
done

if [ "${#assets[@]}" -eq 0 ]; then
    echo "error: no $product_id-$ver-* artifacts found in $dist" >&2
    exit 1
fi

if [ "$dry_run" -eq 1 ]; then
    echo "==> [dry-run] Gitea Releases target: $host/$repo/releases/tag/$tag (prerelease=$is_prerelease)"
    echo "    [dry-run] create as draft (or reuse the existing release), upload, then publish (draft=false)"
    for asset in "${assets[@]}"; do
        echo "    [dry-run] upload $(basename "$asset")"
    done
    exit 0
fi

command -v node >/dev/null 2>&1 || { echo "error: node is required on PATH" >&2; exit 1; }

# Token resolution order (skipped when RAGO_NO_DOTENV=1 in automated pipelines):
#   1) GITEA_TOKEN env var
#   2) ci/.env file (parsed as data without shell execution; interactive local fallback only when RAGO_NO_DOTENV != 1)
#   3) interactive prompt
dotenv_file="$(dirname "$0")/.env"
if [ -z "${GITEA_TOKEN:-}" ] && [ "${RAGO_NO_DOTENV:-0}" != "1" ] && [ -f "$dotenv_file" ] && [ ! -L "$dotenv_file" ]; then
    GITEA_TOKEN="$(sed -n -E 's/^[[:space:]]*GITEA_TOKEN[[:space:]]*=[[:space:]]*(.*)[[:space:]]*$/\1/p' "$dotenv_file" \
        | tail -n 1 \
        | sed -E "s/^\"(.*)\"\$/\\1/; s/^'(.*)'\$/\\1/")"
fi
if [ -z "${GITEA_TOKEN:-}" ] && [ -t 0 ]; then
    read -rsp "GITEA_TOKEN: " GITEA_TOKEN
    echo
fi
: "${GITEA_TOKEN:?error: GITEA_TOKEN is required}"
if [[ "$GITEA_TOKEN" =~ [[:cntrl:][:space:]\"\\] ]]; then
    echo "error: GITEA_TOKEN contains invalid whitespace, control, quote, or backslash characters" >&2
    exit 1
fi

# Pass Authorization header via curl --config on stdin so the token never appears in `ps` argv.
gitea_curl() {
    local method="$1"
    local url="$2"
    shift 2
    curl -fsS --config - -X "$method" "$url" "$@" <<EOF
header = "Authorization: token ${GITEA_TOKEN}"
EOF
}

if [ "$no_push_tag" -eq 0 ]; then
    if ! git -C "$root" rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
        git -C "$root" tag -a "$tag" -m "$product_name $tag"
        git -C "$root" push origin "$tag"
    fi
fi

echo "==> creating/updating release $tag on $host/$repo"
# release_json <draft:true|false>: the create/edit payload (name, body, prerelease flag, draft state)
release_json() {
    RAGO_TAG="$tag" RAGO_PROD="$product_name" RAGO_PRE="$is_prerelease" RAGO_NOTES="${notes_file:-}" RAGO_DRAFT="$1" node - <<'EOF'
const fs = require('node:fs');
const tag = process.env.RAGO_TAG;
const prod = process.env.RAGO_PROD;
const prerelease = process.env.RAGO_PRE === 'true';
const notesFile = process.env.RAGO_NOTES;
const body = notesFile && fs.existsSync(notesFile)
  ? fs.readFileSync(notesFile, 'utf8')
  : `${prod} ${tag}`;
process.stdout.write(JSON.stringify({
  tag_name: tag,
  name: `${prod} ${tag}`,
  body,
  draft: process.env.RAGO_DRAFT === 'true',
  prerelease,
}));
EOF
}
# json_field <json> <field>: prints a top-level scalar field ("" when absent)
json_field() {
    RAGO_JSON="$1" RAGO_FIELD="$2" node -e '
const data = JSON.parse(process.env.RAGO_JSON);
const v = data ? data[process.env.RAGO_FIELD] : undefined;
if (v !== undefined && v !== null) process.stdout.write(String(v));
'
}
# asset_id <assets-json> <name>: id of the asset with exactly that name ("" when absent)
asset_id() {
    RAGO_ASSETS="$1" RAGO_NAME="$2" node -e '
const list = JSON.parse(process.env.RAGO_ASSETS || "[]");
const found = Array.isArray(list) ? list.find((a) => a && a.name === process.env.RAGO_NAME) : null;
if (found && found.id) process.stdout.write(String(found.id));
'
}

# A new release starts as a draft: nobody sees it (or its tag page offers it) until every asset is uploaded.
if ! resp="$(gitea_curl POST "$host/api/v1/repos/$repo/releases" -H 'Content-Type: application/json' -d "$(release_json true)" 2>/dev/null)"; then
    echo "==> release $tag already exists, fetching existing release metadata"
    resp="$(gitea_curl GET "$host/api/v1/repos/$repo/releases/tags/$tag")"
fi

release_id="$(json_field "$resp" id)"
[ -n "$release_id" ] || { echo "error: no release id in the Gitea response for $tag" >&2; exit 1; }
is_draft="$(json_field "$resp" draft)"
[ "$is_draft" = "true" ] || is_draft="false"
assets_url="$host/api/v1/repos/$repo/releases/$release_id/assets"

# Fetch existing assets so re-runs replace stale assets cleanly instead of failing with HTTP 422.
existing_assets_json="$(gitea_curl GET "$assets_url")"

uploaded=0
for f in "${assets[@]}"; do
    asset_name="$(basename "$f")"
    old_id="$(asset_id "$existing_assets_json" "$asset_name")"
    if [ -z "$old_id" ]; then
        echo "==> uploading $asset_name"
        gitea_curl POST "$assets_url?name=$asset_name" -F "attachment=@$f" >/dev/null
    elif [ "$is_draft" = "true" ]; then
        # still a draft: nobody can download it yet, so the short gap of delete-then-upload is harmless
        echo "==> replacing existing asset $asset_name (id=$old_id) in the draft"
        gitea_curl DELETE "$assets_url/$old_id" >/dev/null
        gitea_curl POST "$assets_url?name=$asset_name" -F "attachment=@$f" >/dev/null
    else
        # published: upload first under a temporary name (same extension), then delete the old asset and rename
        tmp_name="uploading-$asset_name"
        stale_tmp_id="$(asset_id "$existing_assets_json" "$tmp_name")"
        if [ -n "$stale_tmp_id" ]; then
            gitea_curl DELETE "$assets_url/$stale_tmp_id" >/dev/null
        fi
        echo "==> replacing existing asset $asset_name (id=$old_id): uploading the new file as $tmp_name first"
        new_resp="$(gitea_curl POST "$assets_url?name=$tmp_name" -F "attachment=@$f")"
        new_id="$(json_field "$new_resp" id)"
        [ -n "$new_id" ] || { echo "error: upload of $asset_name returned no asset id; the old asset is unchanged" >&2; exit 1; }
        gitea_curl DELETE "$assets_url/$old_id" >/dev/null
        rename_json="$(RAGO_NAME="$asset_name" node -e 'process.stdout.write(JSON.stringify({ name: process.env.RAGO_NAME }))')"
        if ! gitea_curl PATCH "$assets_url/$new_id" -H 'Content-Type: application/json' -d "$rename_json" >/dev/null; then
            echo "error: $asset_name is uploaded as $tmp_name (id=$new_id) but the rename failed; rename it in Gitea or re-run" >&2
            exit 1
        fi
    fi
    uploaded=$((uploaded + 1))
done

# Publish last (and refresh name, body and the prerelease flag on an existing release too).
echo "==> publishing release $tag (draft=false, prerelease=$is_prerelease)"
gitea_curl PATCH "$host/api/v1/repos/$repo/releases/$release_id" -H 'Content-Type: application/json' \
    -d "$(release_json false)" >/dev/null

echo "==> Gitea release published ($uploaded asset(s)): $host/$repo/releases/tag/$tag"
