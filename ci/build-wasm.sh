#!/usr/bin/env bash
# ci/build-wasm.sh -- WebAssembly Live Playground (Mod A) Release build, CSP/Brotli/license
# verification, and deploy-receive.sh `demo` archive packager (Linux & macOS hosts).
#
#   EMSDK=/path/to/emsdk \
#   VCPKG_ROOT=/path/to/vcpkg \
#   Qt6_DIR=/path/to/host/Qt \
#   QT_WASM_PATH=/path/to/Qt/wasm_multithread \
#   ./ci/build-wasm.sh [--preset wasm-multi|wasm-single] [--mode playground|gateway] [version]
#
# Standalone validation modes (used by CI self-tests and pre-deploy verification):
#   ./ci/build-wasm.sh --validate-stage-only <stage_dir>
#   ./ci/build-wasm.sh --verify-archive-only <archive.tar.gz>
#
# Produces in dist/wasm/:
#   1. <product id>-wasm-demo.tar.gz         (ready for `ssh deploy@<host> demo < ...`)
#   2. <product id>-wasm-demo.tar.gz.sha256  (SHA-256 digest)

set -euo pipefail
set +x
export LC_ALL=C
export COPYFILE_DISABLE=1

root="$(cd "$(dirname "$0")/.." && pwd)"

# Single-source product brand (cmake/RagoShellDefaults.cmake, docs/Brand.md). Never read ci/.env.
brand() { sed -n "s/^[[:space:]]*set($1 \"\\(.*\\)\")\$/\\1/p" "$root/cmake/RagoShellDefaults.cmake"; }
cmake_ver() { sed -n "s/^set($1 \"\\(.*\\)\")\$/\\1/p" "$root/CMakeLists.txt"; }

product_id="$(brand RAGO_PRODUCT_ID)"
product_name="$(brand RAGO_PRODUCT_NAME)"
default_version="$(cmake_ver PROJECT_VERSION_MAJOR).$(cmake_ver PROJECT_VERSION_MINOR).$(cmake_ver PROJECT_VERSION_PATCH)$(cmake_ver PROJECT_VERSION_BUILD)"

preset="${RAGO_WASM_PRESET:-wasm-multi}"
wasm_mode="${RAGO_WASM_MODE:-playground}"
out_dir="${RAGO_WASM_OUTPUT_DIR:-$root/dist/wasm}"
validate_stage_dir=""
verify_archive_path=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --preset)
            [ "$#" -ge 2 ] || { echo "error: --preset requires a value (wasm-multi or wasm-single)" >&2; exit 1; }
            preset="$2"
            shift 2
            ;;
        --mode)
            [ "$#" -ge 2 ] || { echo "error: --mode requires a value (playground or gateway)" >&2; exit 1; }
            wasm_mode="$2"
            shift 2
            ;;
        --output-dir)
            [ "$#" -ge 2 ] || { echo "error: --output-dir requires a directory path" >&2; exit 1; }
            out_dir="$2"
            shift 2
            ;;
        --validate-stage-only)
            [ "$#" -ge 2 ] || { echo "error: --validate-stage-only requires a directory path" >&2; exit 1; }
            validate_stage_dir="$2"
            shift 2
            ;;
        --verify-archive-only)
            [ "$#" -ge 2 ] || { echo "error: --verify-archive-only requires a .tar.gz path" >&2; exit 1; }
            verify_archive_path="$2"
            shift 2
            ;;
        --help|-h)
            cat <<EOF
Usage: ./ci/build-wasm.sh [options] [version]

Options:
  --preset <wasm-multi|wasm-single>   CMake WASM preset (default: wasm-multi)
  --mode <playground|gateway>         Default WASM runtime mode in wasm-boot.js (default: playground)
  --output-dir <dir>                  Output directory (default: dist/wasm)
  --validate-stage-only <dir>         Validate an existing WASM stage directory without building
  --verify-archive-only <tar.gz>      Verify an existing WASM demo archive against deploy-receive.sh rules
EOF
            exit 0
            ;;
        --)
            shift
            break
            ;;
        --*)
            echo "error: unknown option '$1' (run with --help for usage)" >&2
            exit 1
            ;;
        *)
            break
            ;;
    esac
done

if [ -n "$validate_stage_dir" ] && [ -n "$verify_archive_path" ]; then
    echo "error: --validate-stage-only and --verify-archive-only are mutually exclusive." >&2
    exit 1
fi
if [ "$#" -gt 1 ]; then
    echo "error: unexpected extra argument(s); usage: ./ci/build-wasm.sh [options] [version]" >&2
    exit 1
fi

version="${1:-${default_version:-dev}}"
case "$version" in
    [vV][0-9]*) version="${version#[vV]}" ;;
esac

command -v node >/dev/null 2>&1 || { echo "error: required tool 'node' not found on PATH" >&2; exit 1; }

# Reject unrecognized RAGO_WASM_* environment variables and validate inputs strictly.
RAGO_WASM_VER_CHECK="$version" RAGO_WASM_PRESET_CHECK="$preset" RAGO_WASM_MODE_CHECK="$wasm_mode" node - <<'EOF'
const allowed = new Set([
    'RAGO_WASM_PRESET',
    'RAGO_WASM_MODE',
    'RAGO_WASM_OUTPUT_DIR',
    'RAGO_WASM_VER_CHECK',
    'RAGO_WASM_PRESET_CHECK',
    'RAGO_WASM_MODE_CHECK',
]);
for (const key of Object.keys(process.env)) {
    if (key.startsWith('RAGO_WASM_') && !allowed.has(key)) {
        console.error(`error: unrecognized WASM environment variable '${key}'.`);
        process.exit(1);
    }
}
const ver = process.env.RAGO_WASM_VER_CHECK || '';
if (!/^[A-Za-z0-9][A-Za-z0-9._+-]{0,63}$/.test(ver) || ver.includes('..')) {
    console.error('error: invalid version string (allowed: alphanumeric, ., _, +, -; no slashes or "..").');
    process.exit(1);
}
const preset = process.env.RAGO_WASM_PRESET_CHECK || '';
if (preset !== 'wasm-multi' && preset !== 'wasm-single') {
    console.error(`error: unsupported WASM preset '${preset}' (expected 'wasm-multi' or 'wasm-single').`);
    process.exit(1);
}
const mode = process.env.RAGO_WASM_MODE_CHECK || '';
if (mode !== 'playground' && mode !== 'gateway') {
    console.error(`error: unsupported WASM mode '${mode}' (expected 'playground' or 'gateway').`);
    process.exit(1);
}
EOF

thread_args=()
if [ "$preset" = "wasm-multi" ]; then
    thread_args+=(--require-threads)
fi

if [ -n "$validate_stage_dir" ]; then
    node "$root/ci/package-wasm-demo.mjs" --validate-stage-only "$validate_stage_dir" "${thread_args[@]}"
    exit 0
fi

if [ -n "$verify_archive_path" ]; then
    node "$root/ci/package-wasm-demo.mjs" --verify-archive-only "$verify_archive_path"
    exit 0
fi

# Isolated temporary staging workspace (0700) cleaned deterministically on exit.
ci_tmp="$(mktemp -d "${TMPDIR:-/tmp}/${product_id}-wasm-ci.XXXXXX")"
chmod 700 "$ci_tmp"
cleanup() {
    set +e
    if [ -n "${ci_tmp:-}" ] && [ -d "$ci_tmp" ]; then
        rm -rf "$ci_tmp"
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

for req_tool in cmake ninja; do
    command -v "$req_tool" >/dev/null 2>&1 || { echo "error: required tool '$req_tool' not found on PATH" >&2; exit 1; }
done

if [ -z "${EMSDK:-}" ]; then
    echo "error: EMSDK is not set (install Emscripten 5.0.5 via emsdk and export EMSDK=/path/to/emsdk)." >&2
    exit 1
fi
em_toolchain="$EMSDK/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake"
emcc_bin="$EMSDK/upstream/emscripten/emcc"
if [ ! -f "$em_toolchain" ] || [ ! -x "$emcc_bin" ]; then
    echo "error: Emscripten toolchain not found under EMSDK=$EMSDK (expected $em_toolchain)." >&2
    exit 1
fi
if [ ! -f "$EMSDK/.emscripten" ]; then
    echo "error: $EMSDK/.emscripten is missing (run: emsdk install 5.0.5 && emsdk activate 5.0.5)." >&2
    exit 1
fi
export PATH="$EMSDK/upstream/emscripten:$EMSDK:$PATH"

if [ -z "${VCPKG_ROOT:-}" ] || [ ! -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]; then
    echo "error: VCPKG_ROOT must point to a valid vcpkg checkout ($VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake)." >&2
    exit 1
fi

if [ -z "${Qt6_DIR:-}" ] || [ ! -d "$Qt6_DIR/bin" ] || [ -f "$Qt6_DIR/Qt6Config.cmake" ]; then
    echo "error: Qt6_DIR must point to the host desktop Qt 6 prefix root containing bin/ (not lib/cmake/Qt6)." >&2
    exit 1
fi

if [ -z "${QT_WASM_PATH:-}" ]; then
    qt_parent="$(cd "$Qt6_DIR/.." && pwd)"
    if [ "$preset" = "wasm-multi" ]; then
        QT_WASM_PATH="$qt_parent/wasm_multithread"
    else
        QT_WASM_PATH="$qt_parent/wasm_singlethread"
    fi
fi
if [ ! -d "$QT_WASM_PATH/lib/cmake/Qt6" ]; then
    echo "error: QT_WASM_PATH ($QT_WASM_PATH) does not contain lib/cmake/Qt6." >&2
    exit 1
fi

export EMSDK VCPKG_ROOT Qt6_DIR QT_WASM_PATH

# Verify Emscripten version matches Qt's baked-in QT_EMCC_VERSION before spending time compiling vcpkg ports.
qconfig_h="$QT_WASM_PATH/include/QtCore/qconfig.h"
expected_emcc=""
if [ -f "$qconfig_h" ]; then
    expected_emcc="$(sed -n 's/^#define[[:space:]]*QT_EMCC_VERSION[[:space:]]*"\([0-9][0-9.]*\)".*/\1/p' "$qconfig_h" | head -n 1)"
fi
actual_emcc="$("$emcc_bin" --version 2>&1 | head -n 1 | sed -n 's/.*[^0-9]\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p')"
if [ -n "$expected_emcc" ] && [ "$actual_emcc" != "$expected_emcc" ]; then
    echo "error: Qt WASM ($QT_WASM_PATH) requires Emscripten $expected_emcc, but emcc reports '${actual_emcc:-unknown}'." >&2
    echo "       Run: emsdk install $expected_emcc && emsdk activate $expected_emcc" >&2
    exit 1
fi

echo "==> Building $product_name WebAssembly ($preset, mode=$wasm_mode, version=$version, emcc=${actual_emcc:-unknown})"

echo "==> Checking translations & source license notices"
node "$root/ci/check-i18n.mjs"
node "$root/ci/generate-sbom.mjs" --verify-source-licenses

echo "==> Configuring CMake preset '$preset'"
cmake --preset "$preset" "-DRAGO_WASM_MODE=$wasm_mode"

echo "==> Compiling target '$product_id' and WASM loader/Brotli assets"
cmake --build --preset "$preset" --target "$product_id" "${product_id}_wasm_assets"

build_dir="$root/build/$preset"
app_bin_dir="$build_dir/src/${product_id}-app"
stage_dir="$ci_tmp/stage"
mkdir -p "$stage_dir"
chmod 755 "$stage_dir"

echo "==> Installing licenses/ component into stage"
cmake --install "$build_dir" --prefix "$stage_dir" --component licenses

echo "==> Staging WebAssembly runtime assets"
required_assets=(
    "index.html"
    "wasm-boot.js"
    "qtloader.js"
    "${product_id}.js"
    "${product_id}.wasm"
    "app_icon.png"
)
for asset in "${required_assets[@]}"; do
    src_file="$app_bin_dir/$asset"
    if [ ! -f "$src_file" ] || [ -L "$src_file" ]; then
        echo "error: expected build output '$src_file' is missing or a symlink" >&2
        exit 1
    fi
    cp -f "$src_file" "$stage_dir/$asset"
done

optional_assets=(
    "${product_id}.wasm.br"
    "${product_id}.worker.js"
    "qt_plugins.json"
    "qt_qml_imports.json"
)
for opt_asset in "${optional_assets[@]}"; do
    if [ -f "$app_bin_dir/$opt_asset" ] && [ ! -L "$app_bin_dir/$opt_asset" ]; then
        cp -f "$app_bin_dir/$opt_asset" "$stage_dir/$opt_asset"
    fi
done
if [ -d "$app_bin_dir/qt" ] && [ ! -L "$app_bin_dir/qt" ]; then
    cp -R "$app_bin_dir/qt" "$stage_dir/qt"
fi

archive_out="$out_dir/${product_id}-wasm-demo.tar.gz"
echo "==> Validating stage & packaging $archive_out"
node "$root/ci/package-wasm-demo.mjs" \
    --stage "$stage_dir" \
    --output "$archive_out" \
    --require-vcpkg-copyrights \
    "${thread_args[@]}"

# Cross-check with system tar using the exact command pipeline in ragoshell-infra/scripts/deploy-receive.sh.
if command -v tar >/dev/null 2>&1; then
    tar -tvzf "$archive_out" > "$ci_tmp/tar.long"
    tar -tzf "$archive_out" > "$ci_tmp/tar.names"
    if grep -qv '^[-d]' "$ci_tmp/tar.long"; then
        echo "error: system tar reported non-regular/non-directory entry in $archive_out" >&2
        exit 1
    fi
    if grep -qE '^/|(^|/)\.\.(/|$)' "$ci_tmp/tar.names"; then
        echo "error: system tar reported unsafe path in $archive_out" >&2
        exit 1
    fi
    grep -qxE '(\./)?index\.html' "$ci_tmp/tar.names" || {
        echo "error: index.html missing at top of $archive_out" >&2
        exit 1
    }
    grep -qE '^(\./)?[A-Za-z0-9][A-Za-z0-9._-]*\.wasm$' "$ci_tmp/tar.names" || {
        echo "error: top-level .wasm missing in $archive_out" >&2
        exit 1
    }
fi

echo "==> Done: $archive_out"
