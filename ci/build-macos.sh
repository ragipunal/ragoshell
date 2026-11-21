#!/usr/bin/env bash
# ci/build-macos.sh -- macOS arm64 build, test, code signing, notarization and packaging.
#
#   VCPKG_ROOT=/path/to/vcpkg Qt6_DIR=/path/to/Qt/6.12.0/macos ./ci/build-macos.sh [--validate-signing-only] [version]
#
# Produces in dist/:
#   1. <product id>-<version>-macos-arm64.dmg     (UDZO drag-and-drop image: <product name>.app, /Applications link, licenses/)
#   2. <product id>-<version>-macos-arm64.tar.gz  (archive containing <product name>.app and licenses/)
#
# Without signing environment variables, the bundle and .dmg are ad-hoc signed (local and PR builds stay green).
# When any RAGO_MAC_* (or APPLE_* alias) variable is set, missing/partial/conflicting/unknown settings fail fast before CMake runs.
# See docs/MacOsCodeSigning.md for local Keychain and headless CI ephemeral Keychain + Notary API configuration.

set -euo pipefail
set +x
root="$(cd "$(dirname "$0")/.." && pwd)"

# The brand has one source (cmake/RagoShellDefaults.cmake, docs/Brand.md); file names and identifiers read it from there.
brand() { sed -n "s/^[[:space:]]*set($1 \"\\(.*\\)\")\$/\\1/p" "$root/cmake/RagoShellDefaults.cmake"; }
cmake_ver() { sed -n "s/^set($1 \"\\(.*\\)\")\$/\\1/p" "$root/CMakeLists.txt"; }
product_id="$(brand RAGO_PRODUCT_ID)"
product_name="$(brand RAGO_PRODUCT_NAME)"
bundle_id="$(brand RAGO_BUNDLE_ID)"
default_version="$(cmake_ver PROJECT_VERSION_MAJOR).$(cmake_ver PROJECT_VERSION_MINOR).$(cmake_ver PROJECT_VERSION_PATCH)$(cmake_ver PROJECT_VERSION_BUILD)"

validate_signing_only=0
allow_untrusted_test_cert=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --validate-signing-only)
            validate_signing_only=1
            shift
            ;;
        --allow-untrusted-test-cert)
            allow_untrusted_test_cert=1
            shift
            ;;
        *)
            break
            ;;
    esac
done
if [ "$allow_untrusted_test_cert" -eq 1 ] && [ "$validate_signing_only" -ne 1 ]; then
    echo "error: --allow-untrusted-test-cert is only permitted together with --validate-signing-only." >&2
    exit 1
fi
if [ "$#" -gt 1 ]; then
    echo "error: unexpected extra argument(s); usage: ./ci/build-macos.sh [--validate-signing-only] [version]" >&2
    exit 1
fi
version="${1:-${default_version:-dev}}"
case "$version" in
    [vV][0-9]*) version="${version#[vV]}" ;;
esac
plat=macos-arm64
preset=macos-release
app="$product_name.app"
mcp_bin="$product_id-mcp"

# Isolated temporary workspace and deterministic cleanup of ephemeral Keychains and decoded secret files.
ci_tmp="$(mktemp -d "${TMPDIR:-/tmp}/${product_id}-macos-ci.XXXXXX")"
chmod 700 "$ci_tmp"
ephemeral_kc=""
ephemeral_kc_list_modified=0
prev_keychains=()
temp_p12_file=""
temp_p8_file=""

wipe_file() {
    local f="${1:-}"
    [ -n "$f" ] && [ -f "$f" ] && [ ! -L "$f" ] || return 0
    local sz
    sz="$(wc -c < "$f" 2>/dev/null | tr -d '[:space:]' || printf '0')"
    if [ -n "$sz" ] && [ "$sz" -gt 0 ] 2>/dev/null; then
        dd if=/dev/zero of="$f" bs="$sz" count=1 conv=notrunc >/dev/null 2>&1 || true
    fi
    rm -f "$f"
}

wipe_dir_files() {
    local d="${1:-}"
    [ -n "$d" ] && [ -d "$d" ] || return 0
    local f
    while IFS= read -r -d '' f; do
        wipe_file "$f"
    done < <(find "$d" -type f -print0 2>/dev/null || true)
}

cleanup() {
    set +e
    set +x
    if [ "$ephemeral_kc_list_modified" -eq 1 ]; then
        if [ "${#prev_keychains[@]}" -gt 0 ]; then
            security list-keychains -d user -s "${prev_keychains[@]}" >/dev/null 2>&1 || true
        elif [ -f "$HOME/Library/Keychains/login.keychain-db" ]; then
            security list-keychains -d user -s "$HOME/Library/Keychains/login.keychain-db" >/dev/null 2>&1 || true
        fi
        ephemeral_kc_list_modified=0
    fi
    if [ -n "$ephemeral_kc" ]; then
        security lock-keychain "$ephemeral_kc" >/dev/null 2>&1 || true
    fi
    wipe_file "$temp_p12_file"
    temp_p12_file=""
    wipe_file "$temp_p8_file"
    temp_p8_file=""
    wipe_dir_files "$ci_tmp/sign"
    if [ -n "$ephemeral_kc" ]; then
        security delete-keychain "$ephemeral_kc" >/dev/null 2>&1 || true
        ephemeral_kc=""
    fi
    if [ -n "${ci_tmp:-}" ] && [ -d "$ci_tmp" ]; then
        rm -rf "$ci_tmp"
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

for tool in node codesign security plutil xattr ditto; do
    command -v "$tool" >/dev/null 2>&1 || { echo "error: required tool '$tool' not found on PATH" >&2; exit 1; }
done

# Validate version string strictly (single line, no slashes or '..' traversal) and reject misspelled RAGO_MAC* variables.
RAGO_BUILD_VERSION_CHECK="$version" node - <<'EOF'
const ver = process.env.RAGO_BUILD_VERSION_CHECK || '';
if (!/^[A-Za-z0-9][A-Za-z0-9._+-]{0,63}$/.test(ver) || ver.includes('..')) {
    console.error('error: invalid version string (allowed: alphanumeric, ., _, +, -; no slashes or "..").');
    process.exit(1);
}
const allowedRagoMac = new Set([
    'RAGO_MAC_SIGN_ID',
    'RAGO_MAC_CERT_P12',
    'RAGO_MAC_CERT_P12_BASE64',
    'RAGO_MAC_CERT_PASSWORD',
    'RAGO_MAC_NOTARY_PROFILE',
    'RAGO_MAC_NOTARY_KEY_ID',
    'RAGO_MAC_NOTARY_ISSUER',
    'RAGO_MAC_NOTARY_KEY_FILE',
    'RAGO_MAC_NOTARY_KEY_BASE64',
    'RAGO_MAC_NOTARY_TIMEOUT',
]);
for (const key of Object.keys(process.env)) {
    if ((key.startsWith('RAGO_MAC_') || key.startsWith('RAGO_MACOS_')) && !allowedRagoMac.has(key)) {
        console.error(`error: unrecognized macOS signing/notarization variable '${key}' (check spelling in docs/MacOsCodeSigning.md).`);
        process.exit(1);
    }
}
EOF

# Copy signing/notary environment into local shell variables, then immediately unset sensitive exported variables
# so child build/test processes (cmake, ninja, vcpkg, unit tests, --check-qml) never inherit secrets in environ.
in_sign_id="${RAGO_MAC_SIGN_ID:-}"
in_apple_dev_id="${APPLE_DEVELOPER_ID:-}"
in_cert_p12="${RAGO_MAC_CERT_P12:-}"
in_cert_p12_b64="${RAGO_MAC_CERT_P12_BASE64:-}"
in_apple_p12_b64="${APPLE_CERT_P12_BASE64:-}"
in_cert_pass="${RAGO_MAC_CERT_PASSWORD:-}"
in_apple_cert_pass="${APPLE_CERT_PASSWORD:-}"
in_notary_profile="${RAGO_MAC_NOTARY_PROFILE:-}"
in_notary_key_id="${RAGO_MAC_NOTARY_KEY_ID:-}"
in_apple_key_id="${APPLE_API_KEY_ID:-}"
in_notary_issuer="${RAGO_MAC_NOTARY_ISSUER:-}"
in_apple_issuer="${APPLE_API_ISSUER:-}"
in_notary_key_file="${RAGO_MAC_NOTARY_KEY_FILE:-}"
in_notary_key_b64="${RAGO_MAC_NOTARY_KEY_BASE64:-}"
in_apple_api_key="${APPLE_API_KEY:-}"
in_notary_timeout="${RAGO_MAC_NOTARY_TIMEOUT:-}"

unset RAGO_MAC_CERT_P12_BASE64 APPLE_CERT_P12_BASE64 \
      RAGO_MAC_CERT_PASSWORD APPLE_CERT_PASSWORD \
      RAGO_MAC_NOTARY_KEY_BASE64 APPLE_API_KEY

normalize_single_line() {
    local name="$1" val="$2"
    [ -z "$val" ] && return 0
    RAGO_SL_NAME="$name" RAGO_SL_VAL="$val" node - <<'EOF' || return 1
const name = process.env.RAGO_SL_NAME;
const val = process.env.RAGO_SL_VAL || '';
if (/[\r\n\0]/.test(val)) {
    console.error(`error: ${name} must be a single-line value without newline characters.`);
    process.exit(1);
}
const trimmed = val.trim();
if (!trimmed) {
    console.error(`error: ${name} is set to whitespace only; unset it or provide a valid value.`);
    process.exit(1);
}
process.stdout.write(trimmed);
EOF
}

reject_whitespace_multiline() {
    local name="$1" val="$2"
    [ -z "$val" ] && return 0
    RAGO_ML_NAME="$name" RAGO_ML_VAL="$val" node - <<'EOF' || return 1
if (!(process.env.RAGO_ML_VAL || '').trim()) {
    console.error(`error: ${process.env.RAGO_ML_NAME} is set to whitespace only; unset it or provide a valid value.`);
    process.exit(1);
}
EOF
}

resolve_single_line_alias() {
    local p_name="$1" p_raw="$2" a_name="$3" a_raw="$4"
    local p_val="" a_val=""
    if [ -n "$p_raw" ]; then
        p_val="$(normalize_single_line "$p_name" "$p_raw")" || return 1
    fi
    if [ -n "$a_raw" ]; then
        a_val="$(normalize_single_line "$a_name" "$a_raw")" || return 1
    fi
    if [ -n "$p_val" ] && [ -n "$a_val" ] && [ "$p_val" != "$a_val" ]; then
        echo "error: conflicting values in $p_name and $a_name; specify only one or ensure they match." >&2
        return 1
    fi
    if [ -n "$p_val" ]; then printf '%s' "$p_val"; else printf '%s' "$a_val"; fi
}

decode_b64_to_file() {
    local label="$1" raw_b64="$2" out_path="$3"
    RAGO_B64_INPUT="$raw_b64" node - "$label" "$out_path" <<'EOF' || return 1
const fs = require('fs');
const [label, outPath] = process.argv.slice(2);
const cleaned = (process.env.RAGO_B64_INPUT || '').replace(/\s+/g, '');
if (!cleaned || cleaned.length % 4 !== 0 || !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(cleaned)) {
    console.error(`error: ${label} does not contain valid Base64 data`);
    process.exit(1);
}
const buf = Buffer.from(cleaned, 'base64');
if (buf.length === 0) {
    console.error(`error: ${label} decoded to zero bytes`);
    process.exit(1);
}
fs.writeFileSync(outPath, buf, { mode: 0o600 });
EOF
    chmod 600 "$out_path"
}

# Execute a single `security` subcommand via `security -q -i` stdin so passwords never appear in process argv (`ps`).
run_security_stdin() {
    local subcmd="$1"
    shift
    RAGO_SEC_SUBCMD="$subcmd" node - "$@" <<'EOF' | security -q -i >/dev/null
const q = (s) => '"' + s.replace(/\\/g, '\\\\').replace(/"/g, '\\"') + '"';
const subcmd = process.env.RAGO_SEC_SUBCMD;
const args = process.argv.slice(2).map((arg) => {
    if (arg.startsWith('@ENV:')) {
        const val = process.env[arg.slice(5)] || '';
        if (/[\r\n\0]/.test(val)) {
            console.error('error: keychain/certificate password must not contain CR/LF/NUL characters.');
            process.exit(1);
        }
        return q(val);
    }
    return q(arg);
});
process.stdout.write(`${subcmd} ${args.join(' ')}\n`);
EOF
}

# --- Fail-fast validation of signing and notarization configuration ---
sign_id="$(resolve_single_line_alias RAGO_MAC_SIGN_ID "$in_sign_id" APPLE_DEVELOPER_ID "$in_apple_dev_id")"
cert_p12="$(normalize_single_line RAGO_MAC_CERT_P12 "$in_cert_p12")"

reject_whitespace_multiline RAGO_MAC_CERT_P12_BASE64 "$in_cert_p12_b64"
reject_whitespace_multiline APPLE_CERT_P12_BASE64 "$in_apple_p12_b64"
if [ -n "$in_cert_p12_b64" ] && [ -n "$in_apple_p12_b64" ] && [ "$in_cert_p12_b64" != "$in_apple_p12_b64" ]; then
    echo "error: conflicting values in RAGO_MAC_CERT_P12_BASE64 and APPLE_CERT_P12_BASE64; specify only one or ensure they match." >&2
    exit 1
fi
cert_p12_b64="${in_cert_p12_b64:-$in_apple_p12_b64}"
unset in_cert_p12_b64 in_apple_p12_b64

if [ -n "$in_cert_pass" ] && [ -n "$in_apple_cert_pass" ] && [ "$in_cert_pass" != "$in_apple_cert_pass" ]; then
    echo "error: conflicting values in RAGO_MAC_CERT_PASSWORD and APPLE_CERT_PASSWORD; specify only one or ensure they match." >&2
    exit 1
fi
cert_password="${in_cert_pass:-$in_apple_cert_pass}"
unset in_cert_pass in_apple_cert_pass

notary_profile="$(normalize_single_line RAGO_MAC_NOTARY_PROFILE "$in_notary_profile")"
notary_key_id="$(resolve_single_line_alias RAGO_MAC_NOTARY_KEY_ID "$in_notary_key_id" APPLE_API_KEY_ID "$in_apple_key_id")"
notary_issuer="$(resolve_single_line_alias RAGO_MAC_NOTARY_ISSUER "$in_notary_issuer" APPLE_API_ISSUER "$in_apple_issuer")"
notary_key_file="$(normalize_single_line RAGO_MAC_NOTARY_KEY_FILE "$in_notary_key_file")"

reject_whitespace_multiline RAGO_MAC_NOTARY_KEY_BASE64 "$in_notary_key_b64"
reject_whitespace_multiline APPLE_API_KEY "$in_apple_api_key"
if [ -n "$in_notary_key_b64" ] && [ -n "$in_apple_api_key" ] && [ "$in_notary_key_b64" != "$in_apple_api_key" ]; then
    echo "error: conflicting values in RAGO_MAC_NOTARY_KEY_BASE64 and APPLE_API_KEY; specify only one or ensure they match." >&2
    exit 1
fi
notary_key_inline=""
notary_key_inline_source=""
if [ -n "$in_notary_key_b64" ]; then
    notary_key_inline="$in_notary_key_b64"
    notary_key_inline_source="RAGO_MAC_NOTARY_KEY_BASE64"
elif [ -n "$in_apple_api_key" ]; then
    notary_key_inline="$in_apple_api_key"
    notary_key_inline_source="APPLE_API_KEY"
fi
unset in_notary_key_b64 in_apple_api_key

notary_timeout_raw="$(normalize_single_line RAGO_MAC_NOTARY_TIMEOUT "$in_notary_timeout")"

has_p12_vars=0
if [ -n "$cert_p12" ] || [ -n "$cert_p12_b64" ] || [ -n "$cert_password" ]; then
    has_p12_vars=1
fi

has_notary_api_vars=0
if [ -n "$notary_key_id" ] || [ -n "$notary_issuer" ] || [ -n "$notary_key_file" ] || [ -n "$notary_key_inline" ]; then
    has_notary_api_vars=1
fi

sign_enabled=0
resolved_sign_sha1=""
resolved_sign_cn=""
notary_mode=""
resolved_p12=""
resolved_p8=""
codesign_keychain_args=()
notary_auth_args=()
entitlements="$root/install/macosx/entitlements.plist"

# Resolve and verify a `Developer ID Application:` identity (NFC/NFD-safe, rejecting Apple Distribution / Development / `-`).
resolve_codesign_identity() {
    local kc_arg="${1:-}"
    local id_out
    if [ "$allow_untrusted_test_cert" -eq 1 ]; then
        if [ -n "$kc_arg" ]; then
            id_out="$(security find-identity -p codesigning "$kc_arg" 2>&1 || true)"
        else
            id_out="$(security find-identity -p codesigning 2>&1 || true)"
        fi
    else
        if [ -n "$kc_arg" ]; then
            id_out="$(security find-identity -v -p codesigning "$kc_arg" 2>&1 || true)"
        else
            id_out="$(security find-identity -v -p codesigning 2>&1 || true)"
        fi
    fi

    RAGO_ID_QUERY="$sign_id" RAGO_ID_LIST="$id_out" node - <<'EOF'
const queryRaw = (process.env.RAGO_ID_QUERY || '').normalize('NFC').trim();
const listText = process.env.RAGO_ID_LIST || '';
const isSha1Query = /^[0-9A-Fa-f]{40}$/.test(queryRaw);

if (!isSha1Query) {
    if (/^(Apple Distribution|Apple Development|Mac Developer|3rd Party Mac Developer|Developer ID Installer)\b/i.test(queryRaw)) {
        console.error(`error: '${queryRaw}' is not a Developer ID Application certificate. Direct macOS .app/.dmg distribution requires 'Developer ID Application: <Team> (<TEAM_ID>)' (see docs/MacOsCodeSigning.md §1).`);
        process.exit(1);
    }
    if (!queryRaw.startsWith('Developer ID Application:')) {
        console.error("error: RAGO_MAC_SIGN_ID must be either a 'Developer ID Application: <Name> (<TEAM_ID>)' common name or its 40-character hexadecimal SHA-1 fingerprint.");
        process.exit(1);
    }
}

const entries = [];
for (const line of listText.split(/\r?\n/)) {
    const m = line.match(/^\s*\d+\)\s+([0-9A-Fa-f]{40})\s+"([^"]+)"/);
    if (m) {
        entries.push({ sha1: m[1].toUpperCase(), cn: m[2].normalize('NFC') });
    }
}

const matches = entries.filter((e) =>
    isSha1Query ? e.sha1 === queryRaw.toUpperCase() : e.cn === queryRaw
);

if (matches.length === 0) {
    console.error(`error: signing identity '${queryRaw}' was not found among valid codesigning identities (ensure the 'Developer ID Application' certificate, private key, and Developer ID G2 intermediate CA are installed, or provide RAGO_MAC_CERT_P12_BASE64 + RAGO_MAC_CERT_PASSWORD).`);
    process.exit(1);
}

const chosen = matches[0];
if (!chosen.cn.startsWith('Developer ID Application:')) {
    console.error(`error: matched certificate '${chosen.cn}' is not a 'Developer ID Application:' identity; cannot sign direct macOS distribution artifacts with it.`);
    process.exit(1);
}

const uniqueSha1 = [...new Set(matches.map((m) => m.sha1))];
if (uniqueSha1.length > 1) {
    console.error(`error: multiple certificates match '${queryRaw}' (${uniqueSha1.join(', ')}); pass the exact 40-character SHA-1 thumbprint in RAGO_MAC_SIGN_ID to disambiguate.`);
    process.exit(1);
}

process.stdout.write(`${chosen.sha1}\t${chosen.cn}`);
EOF
}

if [ -z "$sign_id" ]; then
    unset cert_password cert_p12_b64 notary_key_inline
    if [ "$has_p12_vars" -eq 1 ]; then
        echo "error: certificate P12 variables are set, but RAGO_MAC_SIGN_ID (or APPLE_DEVELOPER_ID) is missing." >&2
        exit 1
    fi
    if [ -n "$notary_profile" ] || [ "$has_notary_api_vars" -eq 1 ] || [ -n "$notary_timeout_raw" ]; then
        echo "error: macOS notarization variables are set, but RAGO_MAC_SIGN_ID (or APPLE_DEVELOPER_ID) is missing." >&2
        exit 1
    fi
else
    sign_enabled=1
    if [ ! -f "$entitlements" ]; then
        echo "error: entitlements file not found: $entitlements" >&2
        exit 1
    fi
    plutil -lint "$entitlements" >/dev/null

    if [ "$has_p12_vars" -eq 1 ]; then
        if [ -n "$cert_p12" ] && [ -n "$cert_p12_b64" ]; then
            echo "error: specify either RAGO_MAC_CERT_P12 or RAGO_MAC_CERT_P12_BASE64, not both." >&2
            exit 1
        fi
        if [ -z "$cert_p12" ] && [ -z "$cert_p12_b64" ]; then
            echo "error: RAGO_MAC_CERT_PASSWORD is set, but RAGO_MAC_CERT_P12 / RAGO_MAC_CERT_P12_BASE64 is missing." >&2
            exit 1
        fi
        if [ -z "$cert_password" ]; then
            echo "error: certificate P12 is provided, but RAGO_MAC_CERT_PASSWORD is missing." >&2
            exit 1
        fi
        command -v openssl >/dev/null 2>&1 || { echo "error: required tool 'openssl' not found on PATH" >&2; exit 1; }

        sign_dir="$ci_tmp/sign"
        mkdir -p "$sign_dir"
        chmod 700 "$sign_dir"

        if [ -n "$cert_p12" ]; then
            if [ ! -f "$cert_p12" ] || [ ! -s "$cert_p12" ]; then
                echo "error: RAGO_MAC_CERT_P12 file not found or empty: $cert_p12" >&2
                exit 1
            fi
            resolved_p12="$cert_p12"
        else
            temp_p12_file="$sign_dir/codesign.p12"
            decode_b64_to_file "RAGO_MAC_CERT_P12_BASE64" "$cert_p12_b64" "$temp_p12_file"
            resolved_p12="$temp_p12_file"
        fi
        unset cert_p12_b64

        # Capture existing user Keychain search list (preserving paths with spaces/UTF-8) before touching any Keychain.
        prev_keychains=()
        while IFS= read -r kc_line; do
            kc_path="$(printf '%s\n' "$kc_line" | sed -E 's/^[[:space:]]*"(.*)"[[:space:]]*$/\1/')"
            [ -n "$kc_path" ] && prev_keychains+=("$kc_path")
        done < <(security list-keychains -d user)

        ephemeral_kc="$sign_dir/codesign.keychain-db"
        ephemeral_kc_list_modified=1
        ephemeral_kc_pass="$(openssl rand -hex 32)"

        RAGO_KC_PASS="$ephemeral_kc_pass" run_security_stdin create-keychain -p "@ENV:RAGO_KC_PASS" "$ephemeral_kc"
        chmod 600 "$ephemeral_kc" 2>/dev/null || true
        security set-keychain-settings -lut 21600 "$ephemeral_kc"
        RAGO_KC_PASS="$ephemeral_kc_pass" run_security_stdin unlock-keychain -p "@ENV:RAGO_KC_PASS" "$ephemeral_kc"
        RAGO_P12_PASS="$cert_password" run_security_stdin import "$resolved_p12" \
            -k "$ephemeral_kc" -P "@ENV:RAGO_P12_PASS" -x \
            -T /usr/bin/codesign -T /usr/bin/security -f pkcs12
        RAGO_KC_PASS="$ephemeral_kc_pass" run_security_stdin set-key-partition-list \
            -S apple-tool:,apple:,codesign: -s -k "@ENV:RAGO_KC_PASS" "$ephemeral_kc"
        unset ephemeral_kc_pass cert_password

        security list-keychains -d user -s "$ephemeral_kc" ${prev_keychains[@]+"${prev_keychains[@]}"}
        codesign_keychain_args=(--keychain "$ephemeral_kc")

        # Immediately erase decoded temporary .p12 once imported into the ephemeral Keychain.
        wipe_file "$temp_p12_file"
        temp_p12_file=""

        resolved_pair="$(resolve_codesign_identity "$ephemeral_kc")"
    else
        unset cert_password cert_p12_b64
        resolved_pair="$(resolve_codesign_identity "")"
    fi
    resolved_sign_sha1="$(printf '%s' "$resolved_pair" | cut -f1)"
    resolved_sign_cn="$(printf '%s' "$resolved_pair" | cut -f2-)"

    # Validate optional notarization settings
    if [ -n "$notary_profile" ] && [ "$has_notary_api_vars" -eq 1 ]; then
        unset notary_key_inline
        echo "error: conflicting notarization modes: specify either RAGO_MAC_NOTARY_PROFILE or App Store Connect API Key variables, not both." >&2
        exit 1
    fi

    if [ -n "$notary_profile" ]; then
        unset notary_key_inline
        notary_mode="profile"
        notary_auth_args=(--keychain-profile "$notary_profile")
    elif [ "$has_notary_api_vars" -eq 1 ]; then
        if [ -n "$notary_key_file" ] && [ -n "$notary_key_inline" ]; then
            unset notary_key_inline
            echo "error: specify either RAGO_MAC_NOTARY_KEY_FILE or $notary_key_inline_source, not both." >&2
            exit 1
        fi
        if [ -z "$notary_key_id" ] || [ -z "$notary_issuer" ] || { [ -z "$notary_key_file" ] && [ -z "$notary_key_inline" ]; }; then
            unset notary_key_inline
            echo "error: incomplete App Store Connect Notary API credentials; RAGO_MAC_NOTARY_KEY_ID, RAGO_MAC_NOTARY_ISSUER, and RAGO_MAC_NOTARY_KEY_BASE64 (or RAGO_MAC_NOTARY_KEY_FILE / APPLE_API_KEY) are all required." >&2
            exit 1
        fi
        RAGO_CHK_KID="$notary_key_id" RAGO_CHK_ISS="$notary_issuer" node - <<'EOF'
if (!/^[A-Za-z0-9]{6,32}$/.test(process.env.RAGO_CHK_KID || '')) {
    console.error('error: RAGO_MAC_NOTARY_KEY_ID must be a single-line alphanumeric Key ID (6-32 characters).');
    process.exit(1);
}
if (!/^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$/.test(process.env.RAGO_CHK_ISS || '')) {
    console.error('error: RAGO_MAC_NOTARY_ISSUER must be a valid UUID (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx).');
    process.exit(1);
}
EOF

        if [ -n "$notary_key_file" ]; then
            unset notary_key_inline
            if [ ! -f "$notary_key_file" ] || [ ! -s "$notary_key_file" ]; then
                echo "error: RAGO_MAC_NOTARY_KEY_FILE file not found or empty: $notary_key_file" >&2
                exit 1
            fi
            resolved_p8="$notary_key_file"
        else
            sign_dir="$ci_tmp/sign"
            mkdir -p "$sign_dir"
            chmod 700 "$sign_dir"
            temp_p8_file="$sign_dir/AuthKey_${notary_key_id}.p8"
            if [ "$notary_key_inline_source" = "APPLE_API_KEY" ] && printf '%s' "$notary_key_inline" | grep -q -- '-----BEGIN'; then
                RAGO_PEM_INPUT="$notary_key_inline" node - "$temp_p8_file" <<'EOF'
const fs = require('fs');
fs.writeFileSync(process.argv[2], (process.env.RAGO_PEM_INPUT || '').trim() + '\n', { mode: 0o600 });
EOF
                chmod 600 "$temp_p8_file"
            else
                decode_b64_to_file "$notary_key_inline_source" "$notary_key_inline" "$temp_p8_file"
            fi
            unset notary_key_inline
            resolved_p8="$temp_p8_file"
        fi

        if ! grep -Eq -- '-----BEGIN (EC )?PRIVATE KEY-----' "$resolved_p8"; then
            echo "error: App Store Connect Notary API key does not contain a valid PEM PKCS#8 private key." >&2
            exit 1
        fi
        notary_mode="api_key"
        notary_auth_args=(--key "$resolved_p8" --key-id "$notary_key_id" --issuer "$notary_issuer")
    elif [ -n "$notary_timeout_raw" ]; then
        unset notary_key_inline
        echo "error: RAGO_MAC_NOTARY_TIMEOUT is set, but no notarization credentials (RAGO_MAC_NOTARY_PROFILE or API key triple) were configured." >&2
        exit 1
    else
        unset notary_key_inline
    fi

    if [ -n "$notary_mode" ]; then
        for tool in xcrun spctl; do
            command -v "$tool" >/dev/null 2>&1 || { echo "error: required notarization tool '$tool' not found on PATH" >&2; exit 1; }
        done
        xcrun --find notarytool >/dev/null 2>&1 || { echo "error: 'xcrun notarytool' not available (install Xcode or Command Line Tools)" >&2; exit 1; }
        xcrun --find stapler >/dev/null 2>&1 || { echo "error: 'xcrun stapler' not available (install Xcode or Command Line Tools)" >&2; exit 1; }
    fi
fi

notary_timeout="${notary_timeout_raw:-30m}"
if [ -n "$notary_mode" ]; then
    RAGO_CHK_TIMEOUT="$notary_timeout" node - <<'EOF'
if (!/^[0-9]+[smh]?$/.test(process.env.RAGO_CHK_TIMEOUT || '')) {
    console.error('error: RAGO_MAC_NOTARY_TIMEOUT must be an integer with optional s/m/h suffix (e.g. 30m, 1800s, 1h).');
    process.exit(1);
}
EOF
fi

if [ "$sign_enabled" -eq 1 ]; then
    kc_desc="local-keychain"
    [ -n "$ephemeral_kc" ] && kc_desc="ephemeral-keychain"
    notary_desc="${notary_mode:-disabled}"
    echo "==> macOS signing: enabled (identity='$resolved_sign_cn' [$resolved_sign_sha1], keychain=$kc_desc, notarization=$notary_desc)"
    if [ -z "$notary_mode" ]; then
        echo "warn: Developer ID signing is enabled without notarization credentials; Gatekeeper requires notarization for internet distribution." >&2
    fi
else
    echo "warn: RAGO_MAC_SIGN_ID not set; keeping ad-hoc signature and skipping notarization (local/PR build)." >&2
fi

if [ "$validate_signing_only" -eq 1 ]; then
    node "$root/ci/generate-sbom.mjs" --verify-source-licenses
    echo "==> signing configuration validation passed."
    exit 0
fi

# --- Host build & packaging prerequisites ---
: "${VCPKG_ROOT:?set VCPKG_ROOT to your vcpkg checkout}"
: "${Qt6_DIR:?set Qt6_DIR to your Qt6 prefix (e.g. ~/Qt/6.12.0/macos)}"
[ -d "$VCPKG_ROOT" ] || { echo "error: VCPKG_ROOT directory does not exist: $VCPKG_ROOT" >&2; exit 1; }
[ -d "$Qt6_DIR" ] || { echo "error: Qt6_DIR directory does not exist: $Qt6_DIR" >&2; exit 1; }
VCPKG_ROOT="$(cd "$VCPKG_ROOT" && pwd)"
export VCPKG_ROOT
if [ -f "$Qt6_DIR/Qt6Config.cmake" ]; then
    Qt6_DIR="$(cd "$Qt6_DIR/../../.." && pwd)"
elif [ ! -d "$Qt6_DIR/lib/cmake/Qt6" ]; then
    qt_cfg="$(find "$Qt6_DIR" -maxdepth 5 -name "Qt6Config.cmake" 2>/dev/null | sort -V | tail -n 1 || true)"
    [ -n "$qt_cfg" ] && Qt6_DIR="$(cd "$(dirname "$qt_cfg")/../../.." && pwd)"
else
    Qt6_DIR="$(cd "$Qt6_DIR" && pwd)"
fi
export Qt6_DIR
[ -f "$Qt6_DIR/lib/cmake/Qt6/Qt6Config.cmake" ] || { echo "error: Qt6Config.cmake not found under Qt6_DIR ($Qt6_DIR)" >&2; exit 1; }
[ -x "$Qt6_DIR/bin/macdeployqt" ] || { echo "error: macdeployqt not found at $Qt6_DIR/bin/macdeployqt" >&2; exit 1; }
for tool in cmake ninja git otool install_name_tool tar hdiutil; do
    command -v "$tool" >/dev/null 2>&1 || { echo "error: required tool '$tool' not found on PATH" >&2; exit 1; }
done

# Isolate unit tests and headless QML checks from the host user's ~/.ragoshell and login Keychain.
export RAGOSHELL_CONFIG_DIR="$ci_tmp/cfg"
export RAGOSHELL_KEYSTORE=file
mkdir -p "$RAGOSHELL_CONFIG_DIR"

echo "==> configure + build ($preset, version $version)"
cmake --preset "$preset"
cmake --build --preset "$preset"

echo "==> unit tests"
"$root/build/$preset/src/ragoshell-unit-tests/rago_unit_tests"

echo "==> translations (up to date, placeholders, plural forms)"
cmake --build --preset "$preset" --target update_translations
git -C "$root" diff --exit-code --stat -- src/ragoshell-app/i18n || {
    echo "translation files are out of date: build the update_translations target and commit src/ragoshell-app/i18n" >&2
    exit 1
}
node "$root/ci/check-i18n.mjs"

echo "==> staging"
stage="$root/dist/$product_id-$version-$plat"
rm -rf "$stage"; mkdir -p "$stage/licenses"
# Clean any leftover pre-QJSEngine files from incremental build bundles before copying
rm -rf "$root/build/$preset/src/ragoshell-app/$app/Contents/MacOS/node" \
       "$root/build/$preset/src/ragoshell-app/$app/Contents/MacOS/rago-shell" \
       "$root/build/$preset/src/ragoshell-app/$app/Contents/Resources/rago-shell"
if [ ! -x "$root/build/$preset/src/ragoshell-app/$app/Contents/MacOS/$product_name" ]; then
    echo "error: expected app binary not built: $app/Contents/MacOS/$product_name" >&2
    exit 1
fi
if [ ! -x "$root/build/$preset/src/ragoshell-app/$mcp_bin" ]; then
    echo "error: expected MCP binary not built: $mcp_bin" >&2
    exit 1
fi
ditto "$root/build/$preset/src/ragoshell-app/$app" "$stage/$app"
# MCP server next to the app binary so it shares the embedded frameworks
cp "$root/build/$preset/src/ragoshell-app/$mcp_bin" "$stage/$app/Contents/MacOS/"
# Bundle GPLv3, LGPLv3, embedded third-party notices, and vcpkg port copyrights
# both inside the app Contents/Resources/licenses/ (before codesign so they are covered by the bundle seal)
# and alongside the .app in the .dmg and .tar.gz archives.
rm -rf "$stage/$app/Contents/Resources/licenses" "$stage/licenses"
cmake --install "$root/build/$preset" --component licenses --prefix "$stage"
cp -R "$stage/$app/Contents/Resources/licenses" "$stage/licenses"
node "$root/ci/generate-sbom.mjs" --verify-licenses "$stage/$app/Contents/Resources/licenses" --require-vcpkg-copyrights
node "$root/ci/generate-sbom.mjs" --verify-licenses "$stage/licenses" --require-vcpkg-copyrights

echo "==> macdeployqt"
# Qt 6.8.x macdeployqt can return non-zero after logging missing optional SQL client libs (libmimerapi/libpq);
# run it for both executables, strip unused SQL drivers, then strictly verify embedded frameworks and linkage.
"$Qt6_DIR/bin/macdeployqt" "$stage/$app" \
    -qmldir="$root/src/ragoshell-app/qml" \
    -executable="$stage/$app/Contents/MacOS/$mcp_bin" || echo "info: macdeployqt exited non-zero; verifying bundle linkage..."
rm -f "$stage/$app/Contents/PlugIns/sqldrivers/libqsql"{mimer,odbc,psql,mysql,oci,db2,ibase}*.dylib 2>/dev/null || true

# Bundle libqoffscreen.dylib into Contents/PlugIns/platforms/ before signing so headless --check-qml
# loads an in-bundle plugin signed with the same Team ID (Hardened Runtime Library Validation).
if [ -f "$Qt6_DIR/plugins/platforms/libqoffscreen.dylib" ] && [ ! -f "$stage/$app/Contents/PlugIns/platforms/libqoffscreen.dylib" ]; then
    mkdir -p "$stage/$app/Contents/PlugIns/platforms"
    cp "$Qt6_DIR/plugins/platforms/libqoffscreen.dylib" "$stage/$app/Contents/PlugIns/platforms/"
    chmod u+w "$stage/$app/Contents/PlugIns/platforms/libqoffscreen.dylib"
    while IFS= read -r dep; do
        [ -n "$dep" ] || continue
        rel="${dep#@rpath/}"
        install_name_tool -change "$dep" "@loader_path/../../Frameworks/$rel" \
            "$stage/$app/Contents/PlugIns/platforms/libqoffscreen.dylib"
    done < <(otool -L "$stage/$app/Contents/PlugIns/platforms/libqoffscreen.dylib" | awk '/^[[:space:]]*@rpath\// { print $1 }' | sort -u)
fi

for fw in QtCore QtQuick; do
    if [ ! -d "$stage/$app/Contents/Frameworks/$fw.framework" ]; then
        echo "error: macdeployqt did not embed $fw.framework into $app" >&2
        exit 1
    fi
done

# Verify no Mach-O executable, framework, or plugin inside the bundle still links against host Qt6_DIR or VCPKG_ROOT.
while IFS= read -r -d '' macho_candidate; do
    otool_out="$(otool -L "$macho_candidate" 2>/dev/null || true)"
    [ -n "$otool_out" ] || continue
    if printf '%s\n' "$otool_out" | grep -Fq "$Qt6_DIR"; then
        echo "error: $macho_candidate still links against host Qt6_DIR ($Qt6_DIR) after macdeployqt" >&2
        exit 1
    fi
    if printf '%s\n' "$otool_out" | grep -Fq "$VCPKG_ROOT"; then
        echo "error: $macho_candidate still links against host VCPKG_ROOT ($VCPKG_ROOT) after macdeployqt" >&2
        exit 1
    fi
done < <(find "$stage/$app/Contents" -type f \( -perm +111 -o -name "*.dylib" -o -name "*.so" \) -print0)

# Strip any Finder/quarantine extended attributes before sealing the bundle signature.
xattr -cr "$stage/$app"

if [ "$sign_enabled" -eq 1 ]; then
    echo "==> codesign inside-out (Developer ID: $resolved_sign_cn [$resolved_sign_sha1])"
    find "$stage/$app/Contents" -type f \( -name "*.dylib" -o -name "*.so" \) \
        -exec codesign --force --options runtime --timestamp \
            ${codesign_keychain_args[@]+"${codesign_keychain_args[@]}"} \
            -s "$resolved_sign_sha1" {} +
    if [ -d "$stage/$app/Contents/PlugIns" ]; then
        find "$stage/$app/Contents/PlugIns" -maxdepth 2 -type d -name "*.bundle" \
            -exec codesign --force --options runtime --timestamp \
                ${codesign_keychain_args[@]+"${codesign_keychain_args[@]}"} \
                -s "$resolved_sign_sha1" {} +
    fi
    if [ -d "$stage/$app/Contents/Frameworks" ]; then
        for fw in "$stage/$app/Contents/Frameworks"/*.framework; do
            [ -d "$fw" ] || continue
            codesign --force --options runtime --timestamp \
                ${codesign_keychain_args[@]+"${codesign_keychain_args[@]}"} \
                -s "$resolved_sign_sha1" "$fw"
        done
    fi
    codesign --force --options runtime --timestamp --entitlements "$entitlements" \
        --identifier "$bundle_id.mcp" \
        ${codesign_keychain_args[@]+"${codesign_keychain_args[@]}"} \
        -s "$resolved_sign_sha1" "$stage/$app/Contents/MacOS/$mcp_bin"
    codesign --force --options runtime --timestamp --entitlements "$entitlements" \
        ${codesign_keychain_args[@]+"${codesign_keychain_args[@]}"} \
        -s "$resolved_sign_sha1" "$stage/$app"
    codesign --verify --deep --strict --verbose=2 "$stage/$app"
else
    echo "==> codesign inside-out (ad hoc)"
    find "$stage/$app/Contents" -type f \( -name "*.dylib" -o -name "*.so" \) \
        -exec codesign --force --sign - {} +
    if [ -d "$stage/$app/Contents/PlugIns" ]; then
        find "$stage/$app/Contents/PlugIns" -maxdepth 2 -type d -name "*.bundle" \
            -exec codesign --force --sign - {} +
    fi
    if [ -d "$stage/$app/Contents/Frameworks" ]; then
        for fw in "$stage/$app/Contents/Frameworks"/*.framework; do
            [ -d "$fw" ] || continue
            codesign --force --sign - "$fw"
        done
    fi
    codesign --force --identifier "$bundle_id.mcp" --sign - "$stage/$app/Contents/MacOS/$mcp_bin"
    codesign --force --sign - "$stage/$app"
    codesign --verify --deep --strict "$stage/$app"
fi

echo "==> QML runtime load check (English, ar, ja)"
rm -rf "$RAGOSHELL_CONFIG_DIR" && mkdir -p "$RAGOSHELL_CONFIG_DIR"
for lang_arg in "" "ar" "ja"; do
    args=("--check-qml")
    if [ -n "$lang_arg" ]; then
        args+=("--lang" "$lang_arg")
    fi
    QT_QPA_PLATFORM=offscreen \
        "$stage/$app/Contents/MacOS/$product_name" "${args[@]}"
done
codesign --verify --deep --strict "$stage/$app"

echo "==> packaging .dmg ($stage.dmg)"
dmg_out="$stage.dmg"
dmg_stage="$ci_tmp/dmg-root"
rm -rf "$dmg_stage" "$dmg_out"
mkdir -p "$dmg_stage"
ditto "$stage/$app" "$dmg_stage/$app"
ln -s /Applications "$dmg_stage/Applications"
cp -R "$stage/licenses" "$dmg_stage/licenses"
node "$root/ci/generate-sbom.mjs" --verify-licenses "$dmg_stage/$app/Contents/Resources/licenses" --require-vcpkg-copyrights
node "$root/ci/generate-sbom.mjs" --verify-licenses "$dmg_stage/licenses" --require-vcpkg-copyrights
xattr -cr "$dmg_stage"
hdiutil create -volname "$product_name" -srcfolder "$dmg_stage" -ov -format UDZO "$dmg_out" >/dev/null
rm -rf "$dmg_stage"

if [ "$sign_enabled" -eq 1 ]; then
    echo "==> codesign .dmg (Developer ID: $resolved_sign_cn [$resolved_sign_sha1])"
    codesign --force --timestamp --identifier "$bundle_id.dmg" \
        ${codesign_keychain_args[@]+"${codesign_keychain_args[@]}"} \
        -s "$resolved_sign_sha1" "$dmg_out"
    codesign --verify --verbose=2 "$dmg_out"
else
    codesign --force --identifier "$bundle_id.dmg" --sign - "$dmg_out"
    codesign --verify "$dmg_out"
fi

if [ -n "$notary_mode" ]; then
    echo "==> notarization (xcrun notarytool submit, mode=$notary_mode, timeout=$notary_timeout)"
    notary_json="$ci_tmp/notary-result.json"
    submit_rc=0
    xcrun notarytool submit "$dmg_out" "${notary_auth_args[@]}" \
        --wait --timeout "$notary_timeout" --output-format json > "$notary_json" || submit_rc=$?

    notary_status="Unknown"
    notary_id=""
    notary_msg=""
    if [ -s "$notary_json" ]; then
        notary_parsed="$(node - "$notary_json" <<'EOF' || true
const fs = require('fs');
try {
    const data = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
    const clean = (v) => String(v || '').replace(/[\r\n\t]+/g, ' ').trim();
    const status = clean(data.status) || 'Unknown';
    const id = /^[0-9A-Za-z-]+$/.test(clean(data.id)) ? clean(data.id) : '';
    const msg = clean(data.message);
    process.stdout.write(`${status}\t${id}\t${msg}`);
} catch (_) {
    process.exit(1);
}
EOF
)"
        if [ -n "$notary_parsed" ]; then
            notary_status="$(printf '%s' "$notary_parsed" | cut -f1)"
            notary_id="$(printf '%s' "$notary_parsed" | cut -f2)"
            notary_msg="$(printf '%s' "$notary_parsed" | cut -f3-)"
        fi
    fi

    echo "    notarytool submission exit=$submit_rc id=${notary_id:-unknown} status=$notary_status ${notary_msg:+($notary_msg)}"
    if [ "$submit_rc" -ne 0 ] || [ "$notary_status" != "Accepted" ]; then
        echo "error: Apple notarization did not accept the submission (exit=$submit_rc, status=$notary_status)." >&2
        if [ -n "$notary_id" ]; then
            echo "==> fetching notarytool log for submission $notary_id" >&2
            xcrun notarytool log "$notary_id" "${notary_auth_args[@]}" >&2 || true
        elif [ -s "$notary_json" ]; then
            cat "$notary_json" >&2
        fi
        wipe_file "$temp_p8_file"
        temp_p8_file=""
        exit 1
    fi

    # Wipe temporary .p8 immediately once notarization has completed.
    wipe_file "$temp_p8_file"
    temp_p8_file=""

    echo "==> stapler staple + validate (.dmg and .app)"
    xcrun stapler staple "$dmg_out"
    xcrun stapler validate "$dmg_out"
    xcrun stapler staple "$stage/$app"
    xcrun stapler validate "$stage/$app"

    echo "==> Gatekeeper spctl verification (.app and .dmg)"
    spctl -a -vv -t execute "$stage/$app"
    spctl -a -vv -t open --context context:primary-signature "$dmg_out"
fi

echo "==> packaging .tar.gz ($stage.tar.gz)"
tar_out="$stage.tar.gz"
rm -f "$tar_out"
COPYFILE_DISABLE=1 tar -C "$stage" -czf "$tar_out" "$app" licenses

for artifact in "$dmg_out" "$tar_out"; do
    if [ ! -f "$artifact" ] || [ ! -s "$artifact" ]; then
        echo "error: expected release artifact missing or empty: $artifact" >&2
        exit 1
    fi
done

echo "==> done:"
ls -lh "$dmg_out" "$tar_out"
