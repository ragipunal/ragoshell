#!/usr/bin/env bash
# ci/build-android.sh -- Android arm64 Release build, signing, APK + AAB packaging, and 16 KB alignment gate.
#
#   VCPKG_ROOT=/path/to/vcpkg \
#   Qt6_DIR=/path/to/host/Qt \
#   Qt6_DIR_ANDROID=/path/to/Qt/android_arm64_v8a \
#   ANDROID_SDK_ROOT=/path/to/android/sdk \
#   ANDROID_NDK_ROOT=/path/to/android/sdk/ndk/x.y.z \
#   ./ci/build-android.sh [--validate-signing-only] [version]
#
# Produces in dist/:
#   1. <product id>-<version>-android-arm64.apk  (direct download / sideload APK; v2+v3 signed when keystore is set)
#   2. <product id>-<version>-android-arm64.aab  (Google Play App Bundle; upload-key signed when keystore is set)
#
# Enforces 16 KB ELF LOAD segment alignment (p_align >= 0x4000) across the build tree and inside both .apk and .aab
# archives by default (RAGO_ANDROID_STRICT_ALIGN=1). See docs/AndroidCodeSigning.md.

set -euo pipefail
set +x
root="$(cd "$(dirname "$0")/.." && pwd)"

# The brand has one source (cmake/RagoShellDefaults.cmake, docs/Brand.md); file names and identifiers read it from there.
brand() { sed -n "s/^[[:space:]]*set($1 \"\\(.*\\)\")\$/\\1/p" "$root/cmake/RagoShellDefaults.cmake"; }
cmake_ver() { sed -n "s/^set($1 \"\\(.*\\)\")\$/\\1/p" "$root/CMakeLists.txt"; }
cmake_cache_default() { sed -n "s/^[[:space:]]*set($1 \"\\([^\"]*\\)\".*/\\1/p" "$root/CMakeLists.txt" | head -n 1; }
product_id="$(brand RAGO_PRODUCT_ID)"
product_name="$(brand RAGO_PRODUCT_NAME)"
bundle_id="$(brand RAGO_BUNDLE_ID)"
min_sdk="$(cmake_cache_default RAGO_ANDROID_MIN_SDK)"
min_sdk="${min_sdk:-28}"
default_version="$(cmake_ver PROJECT_VERSION_MAJOR).$(cmake_ver PROJECT_VERSION_MINOR).$(cmake_ver PROJECT_VERSION_PATCH)$(cmake_ver PROJECT_VERSION_BUILD)"

validate_signing_only=0
check_align_only=0
sign_packages_only=0
align_targets=()
sign_in_apk=""
sign_in_aab=""
sign_out_apk=""
sign_out_aab=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --validate-signing-only)
            validate_signing_only=1
            shift
            ;;
        --check-align-only)
            check_align_only=1
            shift
            while [ "$#" -gt 0 ]; do
                case "$1" in
                    --*)
                        break
                        ;;
                    -*)
                        echo "error: alignment target path must not start with '-': $1" >&2
                        exit 1
                        ;;
                    *)
                        align_targets+=("$1")
                        shift
                        ;;
                esac
            done
            ;;
        --sign-packages-only)
            if [ "$#" -lt 5 ]; then
                echo "error: --sign-packages-only requires 4 arguments: <in_apk> <in_aab> <out_apk> <out_aab>" >&2
                exit 1
            fi
            sign_packages_only=1
            sign_in_apk="$2"
            sign_in_aab="$3"
            sign_out_apk="$4"
            sign_out_aab="$5"
            shift 5
            ;;
        --*)
            echo "error: unknown flag '$1'; usage: ./ci/build-android.sh [--validate-signing-only] [version]" >&2
            exit 1
            ;;
        *)
            break
            ;;
    esac
done

mode_count=$((validate_signing_only + check_align_only + sign_packages_only))
if [ "$mode_count" -gt 1 ]; then
    echo "error: --validate-signing-only, --check-align-only, and --sign-packages-only are mutually exclusive." >&2
    exit 1
fi
if [ "$check_align_only" -eq 1 ] && [ "${#align_targets[@]}" -eq 0 ]; then
    echo "error: --check-align-only requires at least one .so, .apk, .aab, or directory path." >&2
    exit 1
fi
if [ "$#" -gt 1 ]; then
    echo "error: unexpected extra argument(s); usage: ./ci/build-android.sh [--validate-signing-only] [version]" >&2
    exit 1
fi

version="${1:-${default_version:-dev}}"
case "$version" in
    [vV][0-9]*) version="${version#[vV]}" ;;
esac
plat=android-arm64

# Isolated temporary workspace (0700) and deterministic zero-wipe cleanup of decoded keystore files.
ci_tmp="$(mktemp -d "${TMPDIR:-/tmp}/${product_id}-android-ci.XXXXXX")"
chmod 700 "$ci_tmp"
temp_keystore_file=""

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
    wipe_file "$temp_keystore_file"
    temp_keystore_file=""
    wipe_dir_files "$ci_tmp/sign"
    if [ -n "${ci_tmp:-}" ] && [ -d "$ci_tmp" ]; then
        rm -rf "$ci_tmp"
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

command -v node >/dev/null 2>&1 || { echo "error: required tool 'node' not found on PATH" >&2; exit 1; }

# On macOS, if JAVA_HOME is not set and the default system JDK is >= 25, prefer an installed LTS JDK 21 or 17
# so Gradle / AGP / R8 / apksigner run without Java 25 bytecode or Conscrypt native-access issues.
if [ "$(uname -s)" = "Darwin" ] && [ -z "${JAVA_HOME:-}" ] && [ -x /usr/libexec/java_home ]; then
    java_major="$(java -version 2>&1 | sed -n 's/.*version "\([0-9][0-9]*\).*/\1/p' | head -n 1 || true)"
    if [ -n "$java_major" ] && [ "$java_major" -ge 25 ] 2>/dev/null; then
        for pref_jdk in 21 17; do
            candidate_jh="$(/usr/libexec/java_home -v "$pref_jdk" 2>/dev/null || true)"
            if [ -n "$candidate_jh" ] && [ -d "$candidate_jh" ]; then
                export JAVA_HOME="$candidate_jh"
                export PATH="$JAVA_HOME/bin:$PATH"
                break
            fi
        done
    fi
fi

# Validate version string strictly and reject unrecognized RAGO_ANDROID_* variables (prevents silent typos).
strict_align="${RAGO_ANDROID_STRICT_ALIGN:-1}"
require_signing="${RAGO_ANDROID_REQUIRE_SIGNING:-0}"
RAGO_BUILD_VERSION_CHECK="$version" RAGO_STRICT_ALIGN_CHECK="$strict_align" RAGO_REQ_SIGN_CHECK="$require_signing" node - <<'EOF'
const ver = process.env.RAGO_BUILD_VERSION_CHECK || '';
if (!/^[A-Za-z0-9][A-Za-z0-9._+-]{0,63}$/.test(ver) || ver.includes('..')) {
    console.error('error: invalid version string (allowed: alphanumeric, ., _, +, -; no slashes or "..").');
    process.exit(1);
}
const strictAlign = process.env.RAGO_STRICT_ALIGN_CHECK || '';
if (strictAlign !== '0' && strictAlign !== '1') {
    console.error("error: RAGO_ANDROID_STRICT_ALIGN must be '0' or '1'.");
    process.exit(1);
}
const reqSign = process.env.RAGO_REQ_SIGN_CHECK || '';
if (reqSign !== '0' && reqSign !== '1') {
    console.error("error: RAGO_ANDROID_REQUIRE_SIGNING must be '0' or '1'.");
    process.exit(1);
}
const allowedRagoAndroid = new Set([
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
    'RAGO_ANDROID_COMPILE_SDK',
]);
for (const key of Object.keys(process.env)) {
    if (key.startsWith('RAGO_ANDROID_') && !allowedRagoAndroid.has(key)) {
        console.error(`error: unrecognized Android variable '${key}' (check spelling in docs/AndroidCodeSigning.md).`);
        process.exit(1);
    }
}
EOF

# Capture signing secrets into local variables and immediately unset them from the exported environment
# so CMake, Ninja, vcpkg, Gradle, and Node child processes never inherit keystore credentials.
in_ks_file="${RAGO_ANDROID_KEYSTORE:-}"
in_ks_b64="${RAGO_ANDROID_KEYSTORE_BASE64:-}"
in_alias_ks_b64="${ANDROID_KEYSTORE_BASE64:-}"
in_ks_pass="${RAGO_ANDROID_KEYSTORE_PASS:-}"
in_alias_ks_pass="${ANDROID_KEYSTORE_PASSWORD:-}"
in_key_alias="${RAGO_ANDROID_KEY_ALIAS:-}"
in_alias_key_alias="${ANDROID_KEY_ALIAS:-}"
in_key_pass="${RAGO_ANDROID_KEY_PASS:-}"
in_alias_key_pass="${ANDROID_KEY_PASSWORD:-}"
in_build_tools_dir="${RAGO_ANDROID_BUILD_TOOLS_DIR:-}"
in_readelf_override="${RAGO_ANDROID_READELF:-}"

unset RAGO_ANDROID_KEYSTORE RAGO_ANDROID_KEYSTORE_BASE64 ANDROID_KEYSTORE_BASE64 \
      RAGO_ANDROID_KEYSTORE_PASS ANDROID_KEYSTORE_PASSWORD \
      RAGO_ANDROID_KEY_ALIAS ANDROID_KEY_ALIAS \
      RAGO_ANDROID_KEY_PASS ANDROID_KEY_PASSWORD

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

validate_secret_no_ctrl() {
    local name="$1" val="$2"
    [ -z "$val" ] && return 0
    RAGO_SEC_NAME="$name" RAGO_SEC_VAL="$val" node - <<'EOF' || return 1
const name = process.env.RAGO_SEC_NAME;
const val = process.env.RAGO_SEC_VAL || '';
if (/[\r\n\0]/.test(val)) {
    console.error(`error: ${name} must not contain CR, LF, or NUL characters.`);
    process.exit(1);
}
if (!val.trim()) {
    console.error(`error: ${name} is set to whitespace only; unset it or provide a valid password.`);
    process.exit(1);
}
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
const fs = require('node:fs');
const [label, outPath] = process.argv.slice(2);
const cleaned = (process.env.RAGO_B64_INPUT || '').replace(/\s+/g, '');
if (!cleaned || cleaned.length % 4 !== 0 || !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(cleaned)) {
    console.error(`error: ${label} does not contain valid Base64 data.`);
    process.exit(1);
}
const buf = Buffer.from(cleaned, 'base64');
if (buf.length === 0) {
    console.error(`error: ${label} decoded to zero bytes.`);
    process.exit(1);
}
fs.writeFileSync(outPath, buf, { mode: 0o600 });
EOF
    chmod 600 "$out_path"
}

resolve_jdk_tool() {
    local name="$1"
    if [ -n "${JAVA_HOME:-}" ] && [ -x "$JAVA_HOME/bin/$name" ]; then
        printf '%s\n' "$JAVA_HOME/bin/$name"
        return 0
    fi
    if command -v "$name" >/dev/null 2>&1; then
        command -v "$name"
        return 0
    fi
    return 1
}

resolve_android_sdk_guess() {
    if [ -n "${ANDROID_SDK_ROOT:-}" ] && [ -d "$ANDROID_SDK_ROOT" ]; then
        printf '%s\n' "$ANDROID_SDK_ROOT"
        return 0
    fi
    if [ -n "${ANDROID_HOME:-}" ] && [ -d "$ANDROID_HOME" ]; then
        printf '%s\n' "$ANDROID_HOME"
        return 0
    fi
    local cand
    for cand in "$HOME/Library/Android/sdk" "/usr/local/lib/android/sdk" "$HOME/Android/Sdk"; do
        if [ -d "$cand" ]; then
            printf '%s\n' "$cand"
            return 0
        fi
    done
    return 1
}

zipalign_cmd=""
apksigner_cmd=""
keytool_cmd=""
jarsigner_cmd=""

resolve_signing_tools() {
    keytool_cmd="$(resolve_jdk_tool keytool || true)"
    [ -n "$keytool_cmd" ] || { echo "error: required JDK tool 'keytool' not found on PATH or JAVA_HOME/bin." >&2; exit 1; }
    jarsigner_cmd="$(resolve_jdk_tool jarsigner || true)"
    [ -n "$jarsigner_cmd" ] || { echo "error: required JDK tool 'jarsigner' not found on PATH or JAVA_HOME/bin." >&2; exit 1; }

    if [ -n "$in_build_tools_dir" ]; then
        local bt_dir
        bt_dir="$(normalize_single_line RAGO_ANDROID_BUILD_TOOLS_DIR "$in_build_tools_dir")"
        [ -d "$bt_dir" ] || { echo "error: RAGO_ANDROID_BUILD_TOOLS_DIR does not exist: $bt_dir" >&2; exit 1; }
        [ -x "$bt_dir/zipalign" ] || { echo "error: zipalign not found under RAGO_ANDROID_BUILD_TOOLS_DIR ($bt_dir)" >&2; exit 1; }
        [ -x "$bt_dir/apksigner" ] || { echo "error: apksigner not found under RAGO_ANDROID_BUILD_TOOLS_DIR ($bt_dir)" >&2; exit 1; }
        zipalign_cmd="$bt_dir/zipalign"
        apksigner_cmd="$bt_dir/apksigner"
        return 0
    fi

    local sdk_guess=""
    sdk_guess="$(resolve_android_sdk_guess || true)"
    if [ -n "$sdk_guess" ] && [ -d "$sdk_guess/build-tools" ]; then
        local best_bt=""
        best_bt="$(RAGO_BT_ROOT="$sdk_guess/build-tools" node - <<'EOF'
const fs = require('node:fs');
const path = require('node:path');
const root = process.env.RAGO_BT_ROOT;
const entries = fs.readdirSync(root, { withFileTypes: true })
    .filter((d) => d.isDirectory())
    .map((d) => d.name)
    .sort((a, b) => b.localeCompare(a, undefined, { numeric: true, sensitivity: 'base' }));
for (const name of entries) {
    const dir = path.join(root, name);
    const za = path.join(dir, 'zipalign');
    const as = path.join(dir, 'apksigner');
    try {
        fs.accessSync(za, fs.constants.X_OK);
        fs.accessSync(as, fs.constants.X_OK);
        process.stdout.write(dir);
        break;
    } catch {}
}
EOF
)"
        if [ -n "$best_bt" ]; then
            zipalign_cmd="$best_bt/zipalign"
            apksigner_cmd="$best_bt/apksigner"
            return 0
        fi
    fi

    if command -v zipalign >/dev/null 2>&1 && command -v apksigner >/dev/null 2>&1; then
        zipalign_cmd="$(command -v zipalign)"
        apksigner_cmd="$(command -v apksigner)"
        return 0
    fi

    echo "error: Android signing is enabled via RAGO_ANDROID_*, but 'zipalign' and 'apksigner' were not found under ANDROID_SDK_ROOT/build-tools or PATH (or set RAGO_ANDROID_BUILD_TOOLS_DIR)." >&2
    exit 1
}

resolve_readelf_tool() {
    if [ -n "$in_readelf_override" ]; then
        local ov
        ov="$(normalize_single_line RAGO_ANDROID_READELF "$in_readelf_override")"
        if [ -x "$ov" ]; then
            printf '%s\n' "$ov"
            return 0
        fi
        if command -v "$ov" >/dev/null 2>&1; then
            command -v "$ov"
            return 0
        fi
        echo "error: RAGO_ANDROID_READELF points to a missing or non-executable tool: $ov" >&2
        exit 1
    fi

    if command -v llvm-readelf >/dev/null 2>&1; then
        command -v llvm-readelf
        return 0
    fi
    if command -v readelf >/dev/null 2>&1; then
        command -v readelf
        return 0
    fi

    local ndk_roots=()
    if [ -n "${ANDROID_NDK_ROOT:-}" ] && [ -d "$ANDROID_NDK_ROOT" ]; then
        ndk_roots+=("$ANDROID_NDK_ROOT")
    fi
    if [ -n "${ANDROID_NDK_HOME:-}" ] && [ -d "$ANDROID_NDK_HOME" ]; then
        ndk_roots+=("$ANDROID_NDK_HOME")
    fi
    local sdk_guess=""
    sdk_guess="$(resolve_android_sdk_guess || true)"
    if [ -n "$sdk_guess" ] && [ -d "$sdk_guess/ndk" ]; then
        while IFS= read -r d; do
            [ -n "$d" ] && ndk_roots+=("$d")
        done < <(ls -1d "$sdk_guess/ndk/"* 2>/dev/null | sort -r || true)
    fi

    local nr cand
    for nr in "${ndk_roots[@]+"${ndk_roots[@]}"}"; do
        if [ -d "$nr/toolchains/llvm/prebuilt" ]; then
            for cand in "$nr"/toolchains/llvm/prebuilt/*/bin/llvm-readelf "$nr"/toolchains/llvm/prebuilt/*/bin/llvm-readelf.exe; do
                if [ -x "$cand" ]; then
                    printf '%s\n' "$cand"
                    return 0
                fi
            done
            cand="$(find -L "$nr/toolchains/llvm/prebuilt" -maxdepth 4 -name "llvm-readelf" -type f 2>/dev/null | head -n 1 || true)"
            if [ -n "$cand" ] && [ -x "$cand" ]; then
                printf '%s\n' "$cand"
                return 0
            fi
        fi
    done
    return 1
}

# Check 16 KB ELF LOAD alignment on a single .so file (returns 0 if aligned, 1 if misaligned or unreadable).
check_single_elf_so() {
    local readelf_bin="$1"
    local so_file="$2"
    local label="$3"
    case "$so_file" in
        -*) so_file="./$so_file" ;;
    esac
    local report=""
    if ! report="$("$readelf_bin" -lW "$so_file" 2>/dev/null)"; then
        echo "error: failed to read ELF program headers from $label" >&2
        return 1
    fi
    local verdict=""
    verdict="$(printf '%s\n' "$report" | awk '
        function parse_align(s,    i, c, v, n) {
            if (s ~ /^0[xX][0-9A-Fa-f]+$/) {
                sub(/^0[xX]/, "", s)
                v = 0
                for (i = 1; i <= length(s); i++) {
                    c = tolower(substr(s, i, 1))
                    n = index("0123456789abcdef", c) - 1
                    if (n < 0) return -1
                    v = v * 16 + n
                }
                return v
            }
            if (s ~ /^[0-9]+$/) {
                return s + 0
            }
            return -1
        }
        function is_pow2(v) {
            if (v < 1) return 0
            while (v > 1) {
                if ((v % 2) != 0) return 0
                v = int(v / 2)
            }
            return 1
        }
        BEGIN { load_count = 0; bad = "" }
        $1 == "LOAD" {
            load_count++
            val = parse_align($NF)
            if (val < 16384 || !is_pow2(val)) {
                bad = $NF
                exit
            }
        }
        END {
            if (bad != "") {
                print "MISALIGNED:" bad
            } else if (load_count == 0) {
                print "NO_LOAD"
            } else {
                print "OK:" load_count
            }
        }
    ')"
    case "$verdict" in
        OK:*)
            return 0
            ;;
        MISALIGNED:*)
            echo "error: $label has ELF LOAD alignment ${verdict#MISALIGNED:} (expected power-of-two >= 0x4000 / 16 KB)" >&2
            return 1
            ;;
        *)
            echo "error: $label contains no valid ELF LOAD program headers" >&2
            return 1
            ;;
    esac
}

# Safely extract */lib/*/*.so or lib/*/*.so from a ZIP/APK/AAB archive into $2, guarding against Zip Slip,
# symlinks, and (for .apk when RAGO_ANDROID_STRICT_ALIGN=1) unaligned uncompressed .so ZIP offsets.
extract_archive_native_libs() {
    local archive_path="$1"
    local dest_dir="$2"
    mkdir -p "$dest_dir"
    if command -v python3 >/dev/null 2>&1; then
        python3 - "$archive_path" "$dest_dir" "$strict_align" <<'PY'
import os, struct, sys, zipfile

archive_path, dest_dir, strict_align = sys.argv[1], os.path.realpath(sys.argv[2]), sys.argv[3]
is_apk = archive_path.lower().endswith(".apk")
count = 0
with open(archive_path, "rb") as raw_fp, zipfile.ZipFile(raw_fp, "r") as zf:
    for info in zf.infolist():
        name = info.filename
        if name.startswith("/") or "\\" in name:
            raise SystemExit(f"error: unsafe path in archive {archive_path}: {name}")
        parts = [p for p in name.split("/") if p]
        if any(p == ".." for p in parts):
            raise SystemExit(f"error: path traversal entry in archive {archive_path}: {name}")
        # Reject UNIX symlinks encoded in external_attr (S_IFLNK = 0o120000)
        mode = (info.external_attr >> 16) & 0o170000
        if mode == 0o120000:
            raise SystemExit(f"error: symlink entry not allowed in archive {archive_path}: {name}")
        if info.is_dir() or not name.endswith(".so"):
            continue
        if not (name.startswith("lib/") or name.startswith("base/lib/")):
            continue
        if is_apk and strict_align == "1":
            if info.compress_type != 0:
                raise SystemExit(f"error: native library {name} in APK {archive_path} is compressed (method={info.compress_type}); Android 16 KB page alignment requires uncompressed ZIP_STORED .so entries.")
            raw_fp.seek(info.header_offset + 26)
            hdr_tail = raw_fp.read(4)
            if len(hdr_tail) != 4:
                raise SystemExit(f"error: truncated ZIP local header for {name} in {archive_path}")
            fname_len, extra_len = struct.unpack("<HH", hdr_tail)
            data_offset = info.header_offset + 30 + fname_len + extra_len
            if (data_offset % 16384) != 0:
                raise SystemExit(f"error: uncompressed .so entry {name} in APK {archive_path} sits at ZIP offset 0x{data_offset:x} (not 16 KB / 0x4000 aligned; run zipalign -P 16).")
        out_path = os.path.realpath(os.path.join(dest_dir, *parts))
        if not out_path.startswith(dest_dir + os.sep):
            raise SystemExit(f"error: escaped destination while extracting {name}")
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        with zf.open(info, "r") as src, open(out_path, "wb") as dst:
            while True:
                chunk = src.read(65536)
                if not chunk:
                    break
                dst.write(chunk)
        count += 1
print(count)
PY
        return $?
    fi

    # Pure Node.js fallback (uses built-in fs + zlib; avoids Info-ZIP unzip symlink/wildcard/stdin pitfalls).
    RAGO_ARCHIVE_PATH="$archive_path" RAGO_DEST_DIR="$dest_dir" RAGO_STRICT_ALIGN="$strict_align" node - <<'EOF'
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const archivePath = process.env.RAGO_ARCHIVE_PATH;
const destDir = fs.realpathSync(process.env.RAGO_DEST_DIR);
const strictAlign = process.env.RAGO_STRICT_ALIGN === '1';
const isApk = archivePath.toLowerCase().endsWith('.apk');
const buf = fs.readFileSync(archivePath);

let eocdOff = -1;
const minEocd = Math.max(0, buf.length - 65557);
for (let i = buf.length - 22; i >= minEocd; i--) {
    if (buf.readUInt32LE(i) === 0x06054b50) {
        eocdOff = i;
        break;
    }
}
if (eocdOff < 0) {
    console.error(`error: invalid ZIP central directory in ${archivePath}`);
    process.exit(1);
}
const totalEntries = buf.readUInt16LE(eocdOff + 10);
const cdOffset = buf.readUInt32LE(eocdOff + 16);
let pos = cdOffset;
let count = 0;

for (let idx = 0; idx < totalEntries; idx++) {
    if (pos + 46 > buf.length || buf.readUInt32LE(pos) !== 0x02014b50) {
        console.error(`error: corrupt central directory entry in ${archivePath}`);
        process.exit(1);
    }
    const method = buf.readUInt16LE(pos + 10);
    const expectedCrc = buf.readUInt32LE(pos + 16);
    const compSize = buf.readUInt32LE(pos + 20);
    const uncompSize = buf.readUInt32LE(pos + 24);
    const nameLen = buf.readUInt16LE(pos + 28);
    const extraLen = buf.readUInt16LE(pos + 30);
    const commentLen = buf.readUInt16LE(pos + 32);
    const extAttr = buf.readUInt32LE(pos + 38);
    const lfhOffset = buf.readUInt32LE(pos + 42);
    const name = buf.toString('utf8', pos + 46, pos + 46 + nameLen);
    pos += 46 + nameLen + extraLen + commentLen;

    if (name.startsWith('/') || name.includes('\\')) {
        console.error(`error: unsafe path in archive ${archivePath}: ${name}`);
        process.exit(1);
    }
    const parts = name.split('/').filter(Boolean);
    if (parts.includes('..')) {
        console.error(`error: path traversal entry in archive ${archivePath}: ${name}`);
        process.exit(1);
    }
    const unixMode = (extAttr >>> 16) & 0o170000;
    if (unixMode === 0o120000) {
        console.error(`error: symlink entry not allowed in archive ${archivePath}: ${name}`);
        process.exit(1);
    }
    if (name.endsWith('/') || !name.endsWith('.so')) continue;
    if (!(name.startsWith('lib/') || name.startsWith('base/lib/'))) continue;

    if (lfhOffset + 30 > buf.length || buf.readUInt32LE(lfhOffset) !== 0x04034b50) {
        console.error(`error: invalid local header for ${name} in ${archivePath}`);
        process.exit(1);
    }
    const lfhNameLen = buf.readUInt16LE(lfhOffset + 26);
    const lfhExtraLen = buf.readUInt16LE(lfhOffset + 28);
    const dataOffset = lfhOffset + 30 + lfhNameLen + lfhExtraLen;
    if (isApk && strictAlign) {
        if (method !== 0) {
            console.error(`error: native library ${name} in APK ${archivePath} is compressed (method=${method}); expected uncompressed ZIP_STORED.`);
            process.exit(1);
        }
        if ((dataOffset % 16384) !== 0) {
            console.error(`error: uncompressed .so entry ${name} in APK ${archivePath} sits at ZIP offset 0x${dataOffset.toString(16)} (not 16 KB / 0x4000 aligned).`);
            process.exit(1);
        }
    }
    const compSlice = buf.subarray(dataOffset, dataOffset + compSize);
    const rawData = method === 0 ? compSlice : method === 8 ? zlib.inflateRawSync(compSlice) : null;
    if (!rawData || rawData.length !== uncompSize || (zlib.crc32(rawData) >>> 0) !== expectedCrc) {
        console.error(`error: failed to decompress or verify CRC32 for ${name} in ${archivePath}`);
        process.exit(1);
    }
    const outPath = path.resolve(destDir, ...parts);
    if (!outPath.startsWith(destDir + path.sep)) {
        console.error(`error: escaped destination while extracting ${name}`);
        process.exit(1);
    }
    fs.mkdirSync(path.dirname(outPath), { recursive: true });
    fs.writeFileSync(outPath, rawData);
    count++;
}
console.log(count);
EOF
}

run_elf_16k_alignment_gate() {
    local targets=("$@")
    local readelf_bin=""
    readelf_bin="$(resolve_readelf_tool || true)"
    if [ -z "$readelf_bin" ]; then
        if [ "$strict_align" = "1" ]; then
            echo "error: llvm-readelf / readelf not found; required for 16 KB ELF alignment gate (RAGO_ANDROID_STRICT_ALIGN=1)." >&2
            return 1
        fi
        echo "warn: llvm-readelf / readelf not found; skipped 16 KB alignment check (RAGO_ANDROID_STRICT_ALIGN=0)." >&2
        return 0
    fi

    local issues=0
    local total_checked=0
    local idx=0
    local target

    for target in "${targets[@]}"; do
        idx=$((idx + 1))
        if [ -d "$target" ]; then
            local dir_count=0
            while IFS= read -r -d '' so_file; do
                [ -L "$so_file" ] && continue
                dir_count=$((dir_count + 1))
                total_checked=$((total_checked + 1))
                if ! check_single_elf_so "$readelf_bin" "$so_file" "$so_file"; then
                    issues=$((issues + 1))
                fi
            done < <(find "$target" -name "*.so" -type f -print0 2>/dev/null || true)
            if [ "$dir_count" -eq 0 ]; then
                echo "error: no .so shared libraries found in directory $target" >&2
                issues=$((issues + 1))
            fi
        elif [ -f "$target" ]; then
            case "$target" in
                *.apk|*.aab)
                    local ext_dir="$ci_tmp/align-extract-$idx"
                    local extracted_count=""
                    if ! extracted_count="$(extract_archive_native_libs "$target" "$ext_dir")"; then
                        issues=$((issues + 1))
                        rm -rf "$ext_dir"
                        continue
                    fi
                    if [ -z "$extracted_count" ] || [ "$extracted_count" -eq 0 ] 2>/dev/null; then
                        echo "error: archive $target contains zero native .so libraries under lib/ (expected arm64-v8a native payload)." >&2
                        issues=$((issues + 1))
                        rm -rf "$ext_dir"
                        continue
                    fi
                    while IFS= read -r -d '' so_file; do
                        local rel="${so_file#"$ext_dir"/}"
                        total_checked=$((total_checked + 1))
                        if ! check_single_elf_so "$readelf_bin" "$so_file" "$target!$rel"; then
                            issues=$((issues + 1))
                        fi
                    done < <(find "$ext_dir" -name "*.so" -type f -print0 2>/dev/null || true)
                    rm -rf "$ext_dir"
                    ;;
                *.so|*.so.*)
                    total_checked=$((total_checked + 1))
                    if ! check_single_elf_so "$readelf_bin" "$target" "$target"; then
                        issues=$((issues + 1))
                    fi
                    ;;
                *)
                    echo "error: unsupported alignment target '$target' (expected directory, .so, .apk, or .aab)." >&2
                    issues=$((issues + 1))
                    ;;
            esac
        else
            echo "error: alignment target does not exist: $target" >&2
            issues=$((issues + 1))
        fi
    done

    if [ "$issues" -eq 0 ] && [ "$total_checked" -gt 0 ]; then
        echo "==> 16 KB ELF LOAD alignment check passed ($total_checked .so binary inspection(s))."
        return 0
    fi

    if [ "$strict_align" = "1" ]; then
        echo "error: 16 KB ELF LOAD alignment gate failed with $issues issue(s) (RAGO_ANDROID_STRICT_ALIGN=1)." >&2
        return 1
    fi
    echo "warn: 16 KB ELF LOAD alignment check reported $issues issue(s), continuing because RAGO_ANDROID_STRICT_ALIGN=0." >&2
    return 0
}

if [ "$check_align_only" -eq 1 ]; then
    run_elf_16k_alignment_gate "${align_targets[@]}"
    exit $?
fi

# --- Fail-fast validation of Android signing configuration ---
ks_file="$(normalize_single_line RAGO_ANDROID_KEYSTORE "$in_ks_file")"

reject_whitespace_multiline RAGO_ANDROID_KEYSTORE_BASE64 "$in_ks_b64"
reject_whitespace_multiline ANDROID_KEYSTORE_BASE64 "$in_alias_ks_b64"
if [ -n "$in_ks_b64" ] && [ -n "$in_alias_ks_b64" ] && [ "$in_ks_b64" != "$in_alias_ks_b64" ]; then
    echo "error: conflicting values in RAGO_ANDROID_KEYSTORE_BASE64 and ANDROID_KEYSTORE_BASE64; specify only one or ensure they match." >&2
    exit 1
fi
ks_b64="${in_ks_b64:-$in_alias_ks_b64}"
unset in_ks_b64 in_alias_ks_b64

validate_secret_no_ctrl RAGO_ANDROID_KEYSTORE_PASS "$in_ks_pass"
validate_secret_no_ctrl ANDROID_KEYSTORE_PASSWORD "$in_alias_ks_pass"
if [ -n "$in_ks_pass" ] && [ -n "$in_alias_ks_pass" ] && [ "$in_ks_pass" != "$in_alias_ks_pass" ]; then
    echo "error: conflicting values in RAGO_ANDROID_KEYSTORE_PASS and ANDROID_KEYSTORE_PASSWORD; specify only one or ensure they match." >&2
    exit 1
fi
ks_pass="${in_ks_pass:-$in_alias_ks_pass}"
unset in_ks_pass in_alias_ks_pass

key_alias="$(resolve_single_line_alias RAGO_ANDROID_KEY_ALIAS "$in_key_alias" ANDROID_KEY_ALIAS "$in_alias_key_alias")"

validate_secret_no_ctrl RAGO_ANDROID_KEY_PASS "$in_key_pass"
validate_secret_no_ctrl ANDROID_KEY_PASSWORD "$in_alias_key_pass"
if [ -n "$in_key_pass" ] && [ -n "$in_alias_key_pass" ] && [ "$in_key_pass" != "$in_alias_key_pass" ]; then
    echo "error: conflicting values in RAGO_ANDROID_KEY_PASS and ANDROID_KEY_PASSWORD; specify only one or ensure they match." >&2
    exit 1
fi
key_pass="${in_key_pass:-$in_alias_key_pass}"
unset in_key_pass in_alias_key_pass

signing_enabled=0
resolved_keystore=""
effective_key_pass=""

if [ -n "$ks_file" ] || [ -n "$ks_b64" ] || [ -n "$ks_pass" ] || [ -n "$key_alias" ] || [ -n "$key_pass" ]; then
    signing_enabled=1

    if [ -n "$ks_file" ] && [ -n "$ks_b64" ]; then
        echo "error: specify either RAGO_ANDROID_KEYSTORE or RAGO_ANDROID_KEYSTORE_BASE64, not both." >&2
        exit 1
    fi
    if [ -z "$ks_file" ] && [ -z "$ks_b64" ]; then
        echo "error: Android signing variables are partially set, but neither RAGO_ANDROID_KEYSTORE nor RAGO_ANDROID_KEYSTORE_BASE64 was provided." >&2
        exit 1
    fi
    if [ -z "$ks_pass" ]; then
        echo "error: RAGO_ANDROID_KEYSTORE* is set, but RAGO_ANDROID_KEYSTORE_PASS is missing." >&2
        exit 1
    fi
    if [ -z "$key_alias" ]; then
        echo "error: RAGO_ANDROID_KEYSTORE* is set, but RAGO_ANDROID_KEY_ALIAS is missing." >&2
        exit 1
    fi
    case "$key_alias" in
        -*)
            echo "error: RAGO_ANDROID_KEY_ALIAS must not start with '-'." >&2
            exit 1
            ;;
    esac
    effective_key_pass="${key_pass:-$ks_pass}"

    resolve_signing_tools

    mkdir -p "$ci_tmp/sign"
    chmod 700 "$ci_tmp/sign"
    if [ -n "$ks_file" ]; then
        [ -f "$ks_file" ] || { echo "error: RAGO_ANDROID_KEYSTORE file does not exist: $ks_file" >&2; exit 1; }
        resolved_keystore="$(cd "$(dirname "$ks_file")" && pwd)/$(basename "$ks_file")"
    else
        temp_keystore_file="$ci_tmp/sign/upload.keystore"
        decode_b64_to_file "RAGO_ANDROID_KEYSTORE_BASE64" "$ks_b64" "$temp_keystore_file"
        resolved_keystore="$temp_keystore_file"
    fi
    unset ks_b64

    echo "==> validating Android upload keystore and alias '$key_alias'"
    kt_out=""
    if ! kt_out="$(RAGO_KS_PASS="$ks_pass" "$keytool_cmd" -list -v -keystore "$resolved_keystore" -alias "$key_alias" -storepass:env RAGO_KS_PASS 2>&1)"; then
        echo "error: failed to read alias '$key_alias' from Android keystore (check keystore format, password, and alias)." >&2
        exit 1
    fi
    if ! printf '%s\n' "$kt_out" | grep -qi "PrivateKeyEntry"; then
        echo "error: keystore alias '$key_alias' is not a PrivateKeyEntry (cannot sign APK/AAB with a certificate-only entry)." >&2
        exit 1
    fi

    # Probe private-key password upfront with jarsigner before running a full C++/Qt build.
    probe_dir="$ci_tmp/sign/probe"
    mkdir -p "$probe_dir"
    printf 'probe\n' > "$probe_dir/probe.txt"
    (cd "$probe_dir" && "$(resolve_jdk_tool jar)" cf probe.jar probe.txt)
    if ! RAGO_KS_PASS="$ks_pass" RAGO_KEY_PASS="$effective_key_pass" \
        "$jarsigner_cmd" \
        -keystore "$resolved_keystore" \
        -storepass:env RAGO_KS_PASS \
        -keypass:env RAGO_KEY_PASS \
        -digestalg SHA-256 \
        "$probe_dir/probe.jar" \
        "$key_alias" >/dev/null 2>&1; then
        rm -rf "$probe_dir"
        echo "error: failed to unlock private key for alias '$key_alias' (verify RAGO_ANDROID_KEY_PASS / RAGO_ANDROID_KEYSTORE_PASS)." >&2
        exit 1
    fi
    rm -rf "$probe_dir"
    echo "==> Android signing configuration validated (zipalign: $zipalign_cmd | apksigner: $apksigner_cmd)"
else
    if [ "$require_signing" = "1" ]; then
        echo "error: RAGO_ANDROID_REQUIRE_SIGNING=1 requires RAGO_ANDROID_KEYSTORE* signing credentials (refusing to produce unsigned release APK/AAB)." >&2
        exit 1
    fi
    echo "warn: RAGO_ANDROID_KEYSTORE* not set; producing unsigned/debug-signed APK and AAB (local/PR build)." >&2
fi

sign_and_verify_android_packages() {
    local in_apk="$1"
    local in_aab="$2"
    local target_apk="$3"
    local target_aab="$4"

    [ -f "$in_apk" ] || { echo "error: input APK not found: $in_apk" >&2; return 1; }
    [ -f "$in_aab" ] || { echo "error: input AAB not found: $in_aab" >&2; return 1; }

    local work_dir="$ci_tmp/pkg-sign"
    rm -rf "$work_dir"
    mkdir -p "$work_dir"
    chmod 700 "$work_dir"

    local aligned_apk="$work_dir/aligned.apk"
    local signed_apk="$work_dir/signed.apk"
    local signing_aab="$work_dir/signing.aab"

    # 1. APK: zipalign BEFORE apksigner (enforce 16 KB page alignment -P 16 when RAGO_ANDROID_STRICT_ALIGN=1).
    local za_help=""
    za_help="$("$zipalign_cmd" 2>&1 || true)"
    if printf '%s\n' "$za_help" | grep -q -- '-P '; then
        echo "==> zipalign (-P 16 -f -v 4) APK"
        "$zipalign_cmd" -P 16 -f -v 4 "$in_apk" "$aligned_apk" >/dev/null
        "$zipalign_cmd" -c -P 16 4 "$aligned_apk" >/dev/null
    elif [ "$strict_align" = "1" ]; then
        echo "error: zipalign ($zipalign_cmd) does not support '-P 16' (install Android SDK Build-Tools >= 35 for 16 KB APK alignment)." >&2
        return 1
    else
        echo "==> zipalign (-p -f -v 4) APK"
        "$zipalign_cmd" -p -f -v 4 "$in_apk" "$aligned_apk" >/dev/null
        "$zipalign_cmd" -c -p 4 "$aligned_apk" >/dev/null
    fi

    echo "==> signing APK with apksigner (v2 + v3 schemes, alias '$key_alias')"
    RAGO_KS_PASS="$ks_pass" RAGO_KEY_PASS="$effective_key_pass" \
        "$apksigner_cmd" sign \
        --ks "$resolved_keystore" \
        --ks-key-alias "$key_alias" \
        --ks-pass env:RAGO_KS_PASS \
        --key-pass env:RAGO_KEY_PASS \
        --min-sdk-version "$min_sdk" \
        --v2-signing-enabled true \
        --v3-signing-enabled true \
        --out "$signed_apk" \
        "$aligned_apk"

    echo "==> verifying APK signature (apksigner verify --verbose --print-certs)"
    # First verify against the APK's manifest SDK range; then verify with --min-sdk-version 24
    # so Android's ApkVerifier does not short-circuit the v2 block when minSdkVersion >= 28.
    "$apksigner_cmd" verify --verbose --print-certs "$signed_apk" >/dev/null
    local apk_verify_out=""
    apk_verify_out="$("$apksigner_cmd" verify --min-sdk-version 24 --verbose --print-certs "$signed_apk")"
    printf '%s\n' "$apk_verify_out"
    if ! printf '%s\n' "$apk_verify_out" | grep -q "Verified using v2 scheme (APK Signature Scheme v2): true"; then
        echo "error: APK verification did not confirm APK Signature Scheme v2." >&2
        return 1
    fi
    if ! printf '%s\n' "$apk_verify_out" | grep -q "Verified using v3 scheme (APK Signature Scheme v3): true"; then
        echo "error: APK verification did not confirm APK Signature Scheme v3." >&2
        return 1
    fi

    # 2. AAB: sign with jarsigner (SHA-256) and strictly verify (rejecting 'jar is unsigned' zero-exit trap).
    cp "$in_aab" "$signing_aab"
    chmod 600 "$signing_aab"
    echo "==> signing AAB with jarsigner (SHA-256, alias '$key_alias')"
    RAGO_KS_PASS="$ks_pass" RAGO_KEY_PASS="$effective_key_pass" \
        "$jarsigner_cmd" \
        -keystore "$resolved_keystore" \
        -storepass:env RAGO_KS_PASS \
        -keypass:env RAGO_KEY_PASS \
        -digestalg SHA-256 \
        "$signing_aab" \
        "$key_alias" >/dev/null

    echo "==> verifying AAB signature (jarsigner -verify -verbose -certs)"
    local aab_verify_out=""
    # Note: Android upload certificates are self-signed (not chained to a JVM TLS CA), so do not pass -strict
    # (which exits 4 on self-signed certs); instead enforce exit 0, 'jar verified.', and zero unsigned entries.
    aab_verify_out="$("$jarsigner_cmd" -verify -verbose -certs "$signing_aab")"
    if printf '%s\n' "$aab_verify_out" | grep -Eqi "jar is unsigned|contains unsigned entries"; then
        echo "error: AAB verification reported unsigned content in bundle." >&2
        return 1
    fi
    if ! printf '%s\n' "$aab_verify_out" | grep -q "jar verified."; then
        echo "error: AAB verification did not report 'jar verified.'." >&2
        return 1
    fi

    mkdir -p "$(dirname "$target_apk")" "$(dirname "$target_aab")"
    chmod 644 "$signed_apk" "$signing_aab"
    mv -f "$signed_apk" "$target_apk"
    mv -f "$signing_aab" "$target_aab"
    rm -rf "$work_dir"
}

if [ "$validate_signing_only" -eq 1 ]; then
    echo "==> --validate-signing-only succeeded (signing_enabled=$signing_enabled)."
    exit 0
fi

if [ "$sign_packages_only" -eq 1 ]; then
    if [ "$signing_enabled" -ne 1 ]; then
        echo "error: --sign-packages-only requires RAGO_ANDROID_KEYSTORE* signing credentials." >&2
        exit 1
    fi
    sign_and_verify_android_packages "$sign_in_apk" "$sign_in_aab" "$sign_out_apk" "$sign_out_aab"
    exit 0
fi

normalize_qt_prefix() {
    local d="$1"
    if [ -d "$d/lib/cmake/Qt6" ]; then
        printf '%s\n' "$d"
    elif [ -f "$d/Qt6Config.cmake" ]; then
        (cd "$d/../../.." && pwd)
    else
        local cfg
        cfg="$(find "$d" -maxdepth 5 -name "Qt6Config.cmake" 2>/dev/null | sort -V | tail -n 1 || true)"
        if [ -n "$cfg" ]; then
            (cd "$(dirname "$cfg")/../../.." && pwd)
        else
            printf '%s\n' "$d"
        fi
    fi
}

: "${VCPKG_ROOT:?set VCPKG_ROOT to your vcpkg checkout}"
: "${Qt6_DIR:?set Qt6_DIR to your host Qt6 prefix (e.g. ~/Qt/6.12.0/gcc_64 or ~/Qt/6.12.0/macos)}"
[ -d "$VCPKG_ROOT" ] || { echo "error: VCPKG_ROOT directory does not exist: $VCPKG_ROOT" >&2; exit 1; }
[ -d "$Qt6_DIR" ] || { echo "error: Qt6_DIR directory does not exist: $Qt6_DIR" >&2; exit 1; }
export Qt6_DIR="$(normalize_qt_prefix "$Qt6_DIR")"
[ -f "$Qt6_DIR/lib/cmake/Qt6/Qt6Config.cmake" ] || { echo "error: host Qt6Config.cmake not found under Qt6_DIR ($Qt6_DIR)" >&2; exit 1; }
for tool in cmake ninja; do
    command -v "$tool" >/dev/null 2>&1 || { echo "error: required tool '$tool' not found on PATH" >&2; exit 1; }
done

# Auto-detect Qt6_DIR_ANDROID if not explicitly set
if [ -z "${Qt6_DIR_ANDROID:-}" ]; then
    parent_dir="$(cd "$Qt6_DIR/.." && pwd)"
    if [ -d "$parent_dir/android_arm64_v8a" ]; then
        export Qt6_DIR_ANDROID="$parent_dir/android_arm64_v8a"
    else
        echo "error: set Qt6_DIR_ANDROID to your Android Qt6 prefix (e.g. ~/Qt/6.12.0/android_arm64_v8a)" >&2
        exit 1
    fi
fi
[ -d "$Qt6_DIR_ANDROID" ] || { echo "error: Qt6_DIR_ANDROID directory does not exist: $Qt6_DIR_ANDROID" >&2; exit 1; }
export Qt6_DIR_ANDROID="$(normalize_qt_prefix "$Qt6_DIR_ANDROID")"
[ -f "$Qt6_DIR_ANDROID/lib/cmake/Qt6/qt.toolchain.cmake" ] || { echo "error: qt.toolchain.cmake not found under Qt6_DIR_ANDROID ($Qt6_DIR_ANDROID)" >&2; exit 1; }

# Detect host OS to choose the Release CMake configure and build presets
host_os="$(uname -s)"
case "$host_os" in
    Darwin)
        configure_preset=android-arm64-macos-release
        build_preset=android-arm64-macos-release
        export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}"
        ;;
    Linux)
        configure_preset=android-arm64-linux
        build_preset=android-arm64-linux
        export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-/usr/local/lib/android/sdk}"
        ;;
    *)
        echo "error: unsupported host OS for build-android.sh: $host_os (on Windows use ci/build-android.ps1)" >&2
        exit 1
        ;;
esac

# Auto-detect ANDROID_NDK_ROOT if not set
if [ -z "${ANDROID_NDK_ROOT:-}" ]; then
    if [ -n "${ANDROID_NDK_HOME:-}" ]; then
        export ANDROID_NDK_ROOT="$ANDROID_NDK_HOME"
    elif [ -d "$ANDROID_SDK_ROOT/ndk" ]; then
        latest_ndk="$(ls -1d "$ANDROID_SDK_ROOT/ndk/"* 2>/dev/null | sort -V | tail -n 1 || true)"
        if [ -n "$latest_ndk" ]; then
            export ANDROID_NDK_ROOT="$latest_ndk"
        fi
    fi
fi

: "${ANDROID_SDK_ROOT:?set ANDROID_SDK_ROOT to your Android SDK}"
: "${ANDROID_NDK_ROOT:?set ANDROID_NDK_ROOT to your Android NDK}"
[ -d "$ANDROID_SDK_ROOT" ] || { echo "error: ANDROID_SDK_ROOT directory does not exist: $ANDROID_SDK_ROOT" >&2; exit 1; }
[ -d "$ANDROID_NDK_ROOT" ] || { echo "error: ANDROID_NDK_ROOT directory does not exist: $ANDROID_NDK_ROOT" >&2; exit 1; }
export ANDROID_NDK_HOME="$ANDROID_NDK_ROOT"

echo "==> host: $host_os | configure: $configure_preset | build: $build_preset | version: $version | bundle: $bundle_id"
echo "==> Qt host: $Qt6_DIR"
echo "==> Qt Android: $Qt6_DIR_ANDROID"
echo "==> Android SDK: $ANDROID_SDK_ROOT"
echo "==> Android NDK: $ANDROID_NDK_ROOT"

echo "==> translation consistency & source license checks"
node "$root/ci/check-i18n.mjs"
node "$root/ci/generate-sbom.mjs" --verify-source-licenses
grep -q "THIRD-PARTY-NOTICES.txt" "$root/src/ragoshell-app/qml.qrc" || {
    echo "error: THIRD-PARTY-NOTICES.txt must remain embedded in src/ragoshell-app/qml.qrc for Android in-app license notices." >&2
    exit 1
}

build_dir="$root/build/$configure_preset"
mkdir -p "$root/dist"
out_apk="$root/dist/$product_id-$version-$plat.apk"
out_aab="$root/dist/$product_id-$version-$plat.aab"
rm -f "$out_apk" "$out_aab"
if [ -d "$build_dir" ]; then
    find "$build_dir" -type d \( -path "*/android-build/build/outputs/apk" -o -path "*/android-build/build/outputs/bundle" \) -exec rm -rf {} + 2>/dev/null || true
    find "$build_dir" -maxdepth 4 -type f \( -name "*.apk" -o -name "*.aab" \) -path "*/android-build/*" -delete 2>/dev/null || true
fi

echo "==> configure + Release build ($configure_preset)"
cmake --preset "$configure_preset" \
    -DANDROID_SDK_ROOT="$ANDROID_SDK_ROOT" \
    -DANDROID_NDK_ROOT="$ANDROID_NDK_ROOT"
cmake --build --preset "$build_preset"

echo "==> packaging Android APK (${product_id}_make_apk)"
cmake --build --preset "$build_preset" --target "${product_id}_make_apk" || cmake --build --preset "$build_preset" --target apk

echo "==> packaging Android App Bundle AAB (${product_id}_make_aab)"
cmake --build --preset "$build_preset" --target "${product_id}_make_aab" || cmake --build --preset "$build_preset" --target aab

locate_gradle_artifact() {
    local search_subdir="$1" ext="$2"
    local found=""
    found="$(find "$build_dir" -path "*/android-build/build/outputs/$search_subdir/*release*$ext" -type f 2>/dev/null | sort | head -n 1 || true)"
    if [ -z "$found" ]; then
        found="$(find "$build_dir" -path "*/android-build/build/outputs/$search_subdir/*$ext" -type f 2>/dev/null | sort | head -n 1 || true)"
    fi
    if [ -z "$found" ]; then
        found="$(find "$build_dir" -name "*$ext" -type f 2>/dev/null | sort | head -n 1 || true)"
    fi
    printf '%s' "$found"
}

raw_apk="$(locate_gradle_artifact apk .apk)"
raw_aab="$(locate_gradle_artifact bundle .aab)"

if [ -z "$raw_apk" ] || [ ! -f "$raw_apk" ]; then
    echo "error: APK file was not produced in $build_dir" >&2
    exit 1
fi
if [ -z "$raw_aab" ] || [ ! -f "$raw_aab" ]; then
    echo "error: AAB file was not produced in $build_dir" >&2
    exit 1
fi

if [ "$signing_enabled" -eq 1 ]; then
    sign_and_verify_android_packages "$raw_apk" "$raw_aab" "$out_apk" "$out_aab"
else
    cp "$raw_apk" "$out_apk"
    cp "$raw_aab" "$out_aab"
    chmod 644 "$out_apk" "$out_aab"
fi

echo "==> created APK: $out_apk"
echo "==> created AAB: $out_aab"

echo "==> running 16 KB ELF LOAD alignment gate (build tree + APK + AAB)"
run_elf_16k_alignment_gate "$build_dir" "$out_apk" "$out_aab"

[ -s "$out_apk" ] || { echo "error: final APK is missing or empty: $out_apk" >&2; exit 1; }
[ -s "$out_aab" ] || { echo "error: final AAB is missing or empty: $out_aab" >&2; exit 1; }

echo "==> done Android Release build step ($product_name $version)."
