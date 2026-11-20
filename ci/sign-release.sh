#!/usr/bin/env bash
# ci/sign-release.sh -- Sign and verify releases.json & SHA256SUMS.txt with Minisign (Ed25519).
#
# Usage:
#   RAGO_MINISIGN_SECRET_KEY_FILE=/path/to/minisign.key \
#   RAGO_MINISIGN_PUBKEY="RW..." \
#     ./ci/sign-release.sh [--dist dist] [--version 2.1.0]
#
#   ./ci/sign-release.sh --verify-only [--dist dist] [--version 2.1.0]
#
# Security guarantees (docs/ReleasePipeline.md §3 & §4):
# - Never reads, sources, or prints ci/.env.
# - Reads product identity and default public key strictly from cmake/RagoShellDefaults.cmake.
# - Secret keys from environment variables are materialized only into a 0700 temp directory
#   with 0600 permissions and zero-wiped (dd if=/dev/zero) on exit.
# - Binds `release:<version>\ttimestamp:<UTC ISO-8601>\tfile:<basename>` in the signed
#   Minisign trusted comment (-t).
# - Runs mandatory post-sign verification (`minisign -Vm` + cryptographic trusted-comment
#   claim verification) before exiting 0.

set -euo pipefail
set +x

root="$(cd "$(dirname "$0")/.." && pwd)"

brand() { sed -n "s/^set($1 \"\\(.*\\)\")\$/\\1/p" "$root/cmake/RagoShellDefaults.cmake"; }
product_id="$(brand RAGO_PRODUCT_ID)"
product_name="$(brand RAGO_PRODUCT_NAME)"
default_pubkey="$(brand RAGO_MINISIGN_PUBKEY)"

if [ -z "$product_id" ] || [ -z "$product_name" ]; then
    echo "error: failed to read brand defaults from cmake/RagoShellDefaults.cmake" >&2
    exit 1
fi

verify_only=0
verify_node_only=0
allow_test_pubkey=0
dist_dir="$root/dist"
version_arg=""
timestamp_arg=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --verify-only)
            verify_only=1
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
        --dist)
            dist_dir="${2:?error: --dist requires a directory path}"
            shift 2
            ;;
        --version)
            version_arg="${2:?error: --version requires a version string}"
            shift 2
            ;;
        --timestamp)
            timestamp_arg="${2:?error: --timestamp requires a UTC ISO-8601 timestamp}"
            shift 2
            ;;
        --help|-h)
            cat <<'EOF'
Usage: ./ci/sign-release.sh [options]

Options:
  --dist <dir>               Release directory containing releases.json & SHA256SUMS.txt (default: ./dist)
  --version <ver>            Expected release version (defaults to latestVersion in releases.json)
  --timestamp <ISO-8601Z>    UTC timestamp for trusted comment (default: current UTC time)
  --verify-only              Verify existing .minisig signatures without signing
  --verify-node-only         With --verify-only, verify using Node Ed25519/BLAKE2b verifier only

Environment:
  RAGO_MINISIGN_SECRET_KEY       Minisign secret key content (raw .key text or Base64-encoded .key)
  RAGO_MINISIGN_SECRET_KEY_FILE  Path to Minisign secret key file (mutually exclusive with _SECRET_KEY)
  RAGO_MINISIGN_PASSWORD         Optional password for scrypt-encrypted Minisign secret keys
  RAGO_MINISIGN_PUBKEY           Minisign public key (RW... or 2-line .pub content; overrides cmake default)
  RAGO_MINISIGN_PUBKEY_FILE      Path to Minisign .pub file
EOF
            exit 0
            ;;
        *)
            echo "error: unknown argument '$1' (run ./ci/sign-release.sh --help)" >&2
            exit 1
            ;;
    esac
done

if [ "$verify_node_only" -eq 1 ] && [ "$verify_only" -ne 1 ]; then
    echo "error: --verify-node-only is only allowed together with --verify-only." >&2
    exit 1
fi

command -v node >/dev/null 2>&1 || { echo "error: required tool 'node' not found on PATH" >&2; exit 1; }

# Guard against misspelled RAGO_MINISIGN_* environment variable names.
node - <<'EOF'
const allowed = new Set([
  'RAGO_MINISIGN_SECRET_KEY',
  'RAGO_MINISIGN_SECRET_KEY_FILE',
  'RAGO_MINISIGN_PASSWORD',
  'RAGO_MINISIGN_PUBKEY',
  'RAGO_MINISIGN_PUBKEY_FILE',
]);
for (const key of Object.keys(process.env)) {
  if (key.startsWith('RAGO_MINISIGN_') && !allowed.has(key)) {
    console.error(`error: unrecognized environment variable "${key}".`);
    process.exit(1);
  }
}
EOF

if [ ! -d "$dist_dir" ]; then
    echo "error: dist directory does not exist: $dist_dir" >&2
    exit 1
fi
dist_dir="$(cd "$dist_dir" && pwd)"

manifest_file="$dist_dir/releases.json"
checksums_file="$dist_dir/SHA256SUMS.txt"
for f in "$manifest_file" "$checksums_file"; do
    if [ ! -f "$f" ] || [ -L "$f" ]; then
        echo "error: required regular file missing or symlink: $f" >&2
        exit 1
    fi
done

# Ephemeral 0700 workspace with deterministic zero-wipe cleanup.
umask 077
sign_tmp="$(mktemp -d "${TMPDIR:-/tmp}/${product_id}-minisign.XXXXXX")"
chmod 700 "$sign_tmp"

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

cleanup() {
    if [ -n "${sign_tmp:-}" ] && [ -d "$sign_tmp" ]; then
        local tf
        while IFS= read -r -d '' tf; do
            wipe_file "$tf"
        done < <(find "$sign_tmp" -type f -print0 2>/dev/null || true)
        rm -rf "$sign_tmp"
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# Resolve & verify manifest + SHA256SUMS.txt integrity against dist/ binaries first.
if [ -n "$version_arg" ]; then
    node "$root/ci/generate-releases-json.mjs" --verify-only --dist "$dist_dir" --version "$version_arg"
else
    node "$root/ci/generate-releases-json.mjs" --verify-only --dist "$dist_dir"
fi

version="$(RAGO_MANIFEST_FILE="$manifest_file" node -e '
const fs = require("node:fs");
const m = JSON.parse(fs.readFileSync(process.env.RAGO_MANIFEST_FILE, "utf8"));
process.stdout.write(m.latestVersion);
')"

# Resolve public key (cmake/RagoShellDefaults.cmake is authoritative once populated;
# env RAGO_MINISIGN_PUBKEY / RAGO_MINISIGN_PUBKEY_FILE must match it unless --allow-test-pubkey is set).
default_pubkey_b64=""
if [ -n "$default_pubkey" ]; then
    default_pubkey_b64="$(node "$root/ci/lib/minisign.mjs" extract-pubkey --pubkey "$default_pubkey")"
fi

pubkey_b64=""
if [ -n "${RAGO_MINISIGN_PUBKEY:-}" ]; then
    pubkey_b64="$(node "$root/ci/lib/minisign.mjs" extract-pubkey --pubkey "$RAGO_MINISIGN_PUBKEY")"
elif [ -n "${RAGO_MINISIGN_PUBKEY_FILE:-}" ]; then
    [ -f "$RAGO_MINISIGN_PUBKEY_FILE" ] && [ ! -L "$RAGO_MINISIGN_PUBKEY_FILE" ] || {
        echo "error: RAGO_MINISIGN_PUBKEY_FILE does not exist or is a symlink: $RAGO_MINISIGN_PUBKEY_FILE" >&2
        exit 1
    }
    pubkey_b64="$(node "$root/ci/lib/minisign.mjs" extract-pubkey --file "$RAGO_MINISIGN_PUBKEY_FILE")"
elif [ -n "$default_pubkey_b64" ]; then
    pubkey_b64="$default_pubkey_b64"
else
    echo "error: Minisign public key is not configured. Set RAGO_MINISIGN_PUBKEY in cmake/RagoShellDefaults.cmake (or pass RAGO_MINISIGN_PUBKEY / RAGO_MINISIGN_PUBKEY_FILE)." >&2
    exit 1
fi

if [ -n "$default_pubkey_b64" ] && [ "$allow_test_pubkey" -eq 0 ] && [ "$pubkey_b64" != "$default_pubkey_b64" ]; then
    echo "error: RAGO_MINISIGN_PUBKEY does not match RAGO_MINISIGN_PUBKEY in cmake/RagoShellDefaults.cmake (pass --allow-test-pubkey only in automated tests)." >&2
    exit 1
fi

run_minisign_sign_one() {
    local sk_path="$1"
    local sk_mode="$2"
    local target_path="$3"
    local sig_path="$4"
    local trusted_comment="$5"
    local untrusted_comment="$6"

    if [ -L "$sig_path" ]; then
        echo "error: refusing to write signature over symbolic link: $sig_path" >&2
        exit 1
    fi
    rm -f "$sig_path"

    if [ "$sk_mode" = "unencrypted" ]; then
        minisign -S -s "$sk_path" -m "$target_path" -x "$sig_path" \
            -t "$trusted_comment" -c "$untrusted_comment" >/dev/null
        return 0
    fi

    # Encrypted ("Sc") secret key: Minisign opens /dev/tty directly rather than reading stdin.
    if [ -n "${RAGO_MINISIGN_PASSWORD:-}" ]; then
        if [[ "$RAGO_MINISIGN_PASSWORD" =~ [$'\r'$'\n'] ]]; then
            echo "error: RAGO_MINISIGN_PASSWORD must not contain CR or LF characters." >&2
            exit 1
        fi
        command -v python3 >/dev/null 2>&1 || {
            echo "error: python3 is required to feed RAGO_MINISIGN_PASSWORD via PTY to minisign." >&2
            exit 1
        }
        python3 - "$sk_path" "$target_path" "$sig_path" "$trusted_comment" "$untrusted_comment" <<'PY'
import errno
import fcntl
import os
import pty
import select
import subprocess
import sys
import termios

sk_path, target_path, sig_path, trusted_comment, untrusted_comment = sys.argv[1:6]
password = os.environ.get("RAGO_MINISIGN_PASSWORD", "")

master_fd, slave_fd = pty.openpty()
attrs = termios.tcgetattr(slave_fd)
attrs[3] &= ~(termios.ECHO | termios.ECHONL)
termios.tcsetattr(slave_fd, termios.TCSANOW, attrs)

def child_setup():
    os.setsid()
    fcntl.ioctl(0, termios.TIOCSCTTY, 0)

proc = subprocess.Popen(
    [
        "minisign", "-S",
        "-s", sk_path,
        "-m", target_path,
        "-x", sig_path,
        "-t", trusted_comment,
        "-c", untrusted_comment,
    ],
    stdin=slave_fd,
    stdout=slave_fd,
    stderr=slave_fd,
    preexec_fn=child_setup,
    close_fds=True,
)
os.close(slave_fd)

sent_pw = False
output_chunks = []
try:
    while True:
        rlist, _, _ = select.select([master_fd], [], [], 30.0)
        if not rlist:
            proc.kill()
            raise RuntimeError("Timed out waiting for minisign signing process.")
        try:
            chunk = os.read(master_fd, 4096)
        except OSError as exc:
            if exc.errno == errno.EIO:
                break
            raise
        if not chunk:
            break
        output_chunks.append(chunk)
        joined = b"".join(output_chunks)
        if (not sent_pw) and (b"Password:" in joined or b"password:" in joined):
            os.write(master_fd, (password + "\n").encode("utf-8"))
            sent_pw = True
finally:
    os.close(master_fd)

rc = proc.wait()
if rc != 0:
    msg = b"".join(output_chunks).decode("utf-8", errors="replace").strip()
    if password:
        msg = msg.replace(password, "[REDACTED]")
    sys.stderr.write(f"error: minisign signing failed (exit {rc}): {msg}\n")
    sys.exit(rc)
PY
    elif [ -t 0 ]; then
        minisign -S -s "$sk_path" -m "$target_path" -x "$sig_path" \
            -t "$trusted_comment" -c "$untrusted_comment"
    else
        echo "error: Minisign secret key is password-encrypted (Sc), but RAGO_MINISIGN_PASSWORD is not set and no interactive TTY is available." >&2
        exit 1
    fi
}

if [ "$verify_only" -eq 0 ]; then
    command -v minisign >/dev/null 2>&1 || {
        echo "error: 'minisign' CLI binary not found on PATH (install via 'brew install minisign' or 'apt-get install minisign')." >&2
        exit 1
    }

    if [ -n "${RAGO_MINISIGN_SECRET_KEY:-}" ] && [ -n "${RAGO_MINISIGN_SECRET_KEY_FILE:-}" ]; then
        echo "error: set either RAGO_MINISIGN_SECRET_KEY or RAGO_MINISIGN_SECRET_KEY_FILE, not both." >&2
        exit 1
    fi
    if [ -z "${RAGO_MINISIGN_SECRET_KEY:-}" ] && [ -z "${RAGO_MINISIGN_SECRET_KEY_FILE:-}" ]; then
        echo "error: signing requires RAGO_MINISIGN_SECRET_KEY or RAGO_MINISIGN_SECRET_KEY_FILE." >&2
        exit 1
    fi

    sk_file="$sign_tmp/release.key"
    if [ -n "${RAGO_MINISIGN_SECRET_KEY_FILE:-}" ]; then
        if [ ! -f "$RAGO_MINISIGN_SECRET_KEY_FILE" ] || [ -L "$RAGO_MINISIGN_SECRET_KEY_FILE" ]; then
            echo "error: RAGO_MINISIGN_SECRET_KEY_FILE must be an existing regular file (not a symlink)." >&2
            exit 1
        fi
        cp "$RAGO_MINISIGN_SECRET_KEY_FILE" "$sk_file"
        chmod 600 "$sk_file"
    else
        # Materialize raw 2-line .key text or Base64-encoded .key file without shell expansion.
        RAGO_SK_OUT="$sk_file" node - <<'EOF'
const fs = require('node:fs');
const raw = (process.env.RAGO_MINISIGN_SECRET_KEY || '').trim();
let content = raw;
if (!raw.includes('untrusted comment:') && !raw.includes('\n')) {
  const decoded = Buffer.from(raw, 'base64').toString('utf8');
  if (decoded.includes('untrusted comment:')) {
    content = decoded;
  }
}
fs.writeFileSync(process.env.RAGO_SK_OUT, content.endsWith('\n') ? content : `${content}\n`, {
  encoding: 'utf8',
  mode: 0o600,
});
EOF
    fi

    sk_mode="$(node "$root/ci/lib/minisign.mjs" inspect-secret-key --file "$sk_file")"
    utc_ts="${timestamp_arg:-$(date -u +%Y-%m-%dT%H:%M:%SZ)}"
    if ! [[ "$utc_ts" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$ ]]; then
        echo "error: invalid UTC ISO-8601 timestamp '$utc_ts' (expected YYYY-MM-DDTHH:MM:SSZ)." >&2
        exit 1
    fi

    for target_file in "$manifest_file" "$checksums_file"; do
        base_name="$(basename "$target_file")"
        trusted_comment="$(printf 'release:%s\ttimestamp:%s\tfile:%s' "$version" "$utc_ts" "$base_name")"
        untrusted_comment="${product_name} ${version} ${base_name} Ed25519 signature"
        echo "==> signing $base_name (release:$version, timestamp:$utc_ts)"
        run_minisign_sign_one "$sk_file" "$sk_mode" "$target_file" "${target_file}.minisig" \
            "$trusted_comment" "$untrusted_comment"
        chmod 644 "${target_file}.minisig"
    done

    wipe_file "$sk_file"
fi

# Mandatory verification gate (`minisign -Vm` + cryptographic trusted-comment binding check).
if [ "$verify_node_only" -eq 0 ]; then
    command -v minisign >/dev/null 2>&1 || {
        echo "error: 'minisign' CLI binary is required for signature verification (-Vm)." >&2
        exit 1
    }
fi

for target_file in "$manifest_file" "$checksums_file"; do
    base_name="$(basename "$target_file")"
    sig_file="${target_file}.minisig"
    if [ ! -f "$sig_file" ] || [ -L "$sig_file" ]; then
        echo "error: missing or symlink signature file: $sig_file" >&2
        exit 1
    fi
    if [ "$verify_node_only" -eq 0 ]; then
        minisign -Vm "$target_file" -x "$sig_file" -P "$pubkey_b64" >/dev/null
    fi
    node "$root/ci/lib/minisign.mjs" verify \
        --file "$target_file" \
        --sig "$sig_file" \
        --pubkey "$pubkey_b64" \
        --version "$version" \
        --expected-file "$base_name"
done

echo "==> minisign verification passed for releases.json & SHA256SUMS.txt (${product_name} ${version})"
