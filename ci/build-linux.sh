#!/usr/bin/env bash
# ci/build-linux.sh -- Linux x64 build, test, tar.gz and self-contained AppImage packaging.
#
#   VCPKG_ROOT=/path/to/vcpkg Qt6_DIR=/path/to/Qt/6.12.0/gcc_64 ./ci/build-linux.sh [--validate-packaging-only] [version]
#
# Produces in dist/:
#   1. <product id>-<version>-linux-x64.AppImage  (self-contained: Qt 6 libs, QML modules, Wayland/XCB/offscreen/TLS/image/icon
#                                                  plugins, <product id>, <product id>-mcp via --mcp or symlink, .desktop,
#                                                  hicolor icons, AppStream metainfo, and licenses/)
#   2. <product id>-<version>-linux-x64.tar.gz    (archive containing <product id>, <product id>-mcp, .desktop, metainfo,
#                                                  icons/, and licenses/; expects system Qt or custom runtime)
#
# Optional environment:
#   RAGO_LINUX_TOOLS_DIR   Cacheable directory for pinned, SHA-256-verified AppImage tooling (defaults to an ephemeral temp dir).

set -euo pipefail
set +x
root="$(cd "$(dirname "$0")/.." && pwd)"

# Pinned external AppImage packaging tool (immutable upstream release tag + SHA-256 digest).
# Never execute an unverified download; run with --appimage-extract-and-run so /dev/fuse is not required in CI/Docker.
readonly APPIMAGETOOL_TAG="1.9.1"
readonly APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/${APPIMAGETOOL_TAG}/appimagetool-x86_64.AppImage"
readonly APPIMAGETOOL_SHA256="ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"

# The brand has one source (cmake/RagoShellDefaults.cmake, docs/Brand.md); file names and metadata read it from there.
brand() { sed -n "s/^[[:space:]]*set($1 \"\\(.*\\)\")\$/\\1/p" "$root/cmake/RagoShellDefaults.cmake"; }
cmake_ver() { sed -n "s/^set($1 \"\\(.*\\)\")\$/\\1/p" "$root/CMakeLists.txt"; }

product_id="$(brand RAGO_PRODUCT_ID)"
product_name="$(brand RAGO_PRODUCT_NAME)"
bundle_id="$(brand RAGO_BUNDLE_ID)"
product_url="$(brand RAGO_PRODUCT_URL)"
for required_brand in product_id product_name bundle_id product_url; do
    if [ -z "${!required_brand}" ]; then
        echo "error: failed to read $required_brand from cmake/RagoShellDefaults.cmake" >&2
        exit 1
    fi
done

default_version="$(cmake_ver PROJECT_VERSION_MAJOR).$(cmake_ver PROJECT_VERSION_MINOR).$(cmake_ver PROJECT_VERSION_PATCH)$(cmake_ver PROJECT_VERSION_BUILD)"

validate_packaging_only=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --validate-packaging-only)
            validate_packaging_only=1
            shift
            ;;
        *)
            break
            ;;
    esac
done
if [ "$#" -gt 1 ]; then
    echo "error: unexpected extra argument(s); usage: ./ci/build-linux.sh [--validate-packaging-only] [version]" >&2
    exit 1
fi
version="${1:-${default_version:-dev}}"
case "$version" in
    [vV][0-9]*) version="${version#[vV]}" ;;
esac
plat=linux-x64
preset=linux-release
mcp_bin="$product_id-mcp"
desktop_src="$root/install/linux/$product_id.desktop"
metainfo_src="$root/install/linux/$bundle_id.metainfo.xml"

command -v node >/dev/null 2>&1 || { echo "error: required tool 'node' not found on PATH" >&2; exit 1; }

# Strictly validate version (no whitespace, '/', '\', or '..' traversal) and RAGO_LINUX_* environment variables.
RAGO_BUILD_VERSION_CHECK="$version" node - <<'EOF'
const ver = process.env.RAGO_BUILD_VERSION_CHECK || '';
if (!/^[A-Za-z0-9][A-Za-z0-9._+-]{0,63}$/.test(ver) || ver.includes('..')) {
    console.error('error: invalid version string (allowed: alphanumeric, ., _, +, -; no whitespace, slashes, or "..").');
    process.exit(1);
}
const allowedRagoLinux = new Set(['RAGO_LINUX_TOOLS_DIR']);
for (const key of Object.keys(process.env)) {
    if (key.startsWith('RAGO_LINUX_') && !allowedRagoLinux.has(key)) {
        console.error(`error: unrecognized Linux packaging variable '${key}' (allowed: RAGO_LINUX_TOOLS_DIR).`);
        process.exit(1);
    }
}
const toolsDir = process.env.RAGO_LINUX_TOOLS_DIR;
if (toolsDir !== undefined && toolsDir !== '') {
    if (/[\r\n\0]/.test(toolsDir) || !toolsDir.trim()) {
        console.error('error: RAGO_LINUX_TOOLS_DIR must be a valid single-line path when set.');
        process.exit(1);
    }
}
EOF

# Isolate tests, temporary downloads, and headless QML checks from the host user's ~/.ragoshell, ~/.config, and Secret Service keyring.
ci_tmp="$(mktemp -d "${TMPDIR:-/tmp}/${product_id}-linux-ci.XXXXXX")"
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

sha256_hex() {
    local target="$1"
    [ -f "$target" ] || return 1
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$target" | awk '{print tolower($1)}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$target" | awk '{print tolower($1)}'
    else
        node -e '
            const fs = require("fs");
            const crypto = require("crypto");
            const buf = fs.readFileSync(process.argv[1]);
            process.stdout.write(crypto.createHash("sha256").update(buf).digest("hex") + "\n");
        ' "$target"
    fi
}

verify_sha256() {
    local target="$1"
    local expected
    expected="$(printf '%s' "$2" | tr '[:upper:]' '[:lower:]')"
    if [[ ! "$expected" =~ ^[0-9a-f]{64}$ ]]; then
        echo "error: invalid expected SHA-256 digest '$expected'" >&2
        return 1
    fi
    if [ ! -s "$target" ]; then
        echo "error: file missing or empty for SHA-256 verification: $target" >&2
        return 1
    fi
    local actual
    actual="$(sha256_hex "$target")" || return 1
    if [ "$actual" != "$expected" ]; then
        echo "error: SHA-256 mismatch for $target (expected $expected, got $actual)" >&2
        return 1
    fi
    return 0
}

validate_freedesktop_files() {
    local d_file="$1"
    local m_file="$2"
    [ -f "$d_file" ] || { echo "error: desktop file not found: $d_file" >&2; exit 1; }
    [ -f "$m_file" ] || { echo "error: AppStream metainfo file not found: $m_file" >&2; exit 1; }

    # Verify brand, IDs, license, and launchable linkage against cmake/RagoShellDefaults.cmake.
    RAGO_CHK_ID="$product_id" \
    RAGO_CHK_NAME="$product_name" \
    RAGO_CHK_BUNDLE="$bundle_id" \
    RAGO_CHK_URL="$product_url" \
    node - "$d_file" "$m_file" <<'EOF'
const fs = require('fs');
const [desktopPath, metainfoPath] = process.argv.slice(2);
const desktop = fs.readFileSync(desktopPath, 'utf8');
const metainfo = fs.readFileSync(metainfoPath, 'utf8');
const id = process.env.RAGO_CHK_ID;
const name = process.env.RAGO_CHK_NAME;
const bundle = process.env.RAGO_CHK_BUNDLE;
const url = process.env.RAGO_CHK_URL;

const mustDesktop = [
    ['[Desktop Entry]', desktop.includes('[Desktop Entry]')],
    [`Name=${name}`, new RegExp(`^Name=${name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}$`, 'm').test(desktop)],
    [`Exec=${id}`, new RegExp(`^Exec=${id}(?:\\s|$)`, 'm').test(desktop)],
    [`Icon=${id}`, new RegExp(`^Icon=${id}$`, 'm').test(desktop)],
    [`StartupWMClass=${id}`, new RegExp(`^StartupWMClass=${id}$`, 'm').test(desktop)],
];
for (const [label, ok] of mustDesktop) {
    if (!ok) {
        console.error(`error: ${desktopPath} missing expected entry: ${label}`);
        process.exit(1);
    }
}

const tag = (t) => {
    const m = metainfo.match(new RegExp(`<${t}>([^<]+)</${t}>`));
    return m ? m[1].trim() : '';
};
const checks = [
    [`<id>${bundle}</id>`, tag('id') === bundle],
    [`<name>${name}</name>`, tag('name') === name],
    ['<project_license>GPL-3.0-only</project_license>', tag('project_license') === 'GPL-3.0-only'],
    ['<metadata_license>', Boolean(tag('metadata_license'))],
    [`<launchable type="desktop-id">${id}.desktop</launchable>`,
        new RegExp(`<launchable\\s+type="desktop-id">\\s*${id}\\.desktop\\s*</launchable>`).test(metainfo)],
    [`<url type="homepage">${url}</url>`,
        new RegExp(`<url\\s+type="homepage">\\s*${url.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}\\s*</url>`).test(metainfo)],
    [`<binary>${id}</binary>`, new RegExp(`<binary>\\s*${id}\\s*</binary>`).test(metainfo)],
    [`<binary>${id}-mcp</binary>`, new RegExp(`<binary>\\s*${id}-mcp\\s*</binary>`).test(metainfo)],
    ['MongoDB trademark notice', metainfo.includes('MongoDB is a registered trademark of MongoDB, Inc.')],
];
for (const [label, ok] of checks) {
    if (!ok) {
        console.error(`error: ${metainfoPath} failed check: ${label}`);
        process.exit(1);
    }
}
EOF

    if command -v xmllint >/dev/null 2>&1; then
        xmllint --noout "$m_file"
    fi
    if command -v desktop-file-validate >/dev/null 2>&1; then
        desktop-file-validate "$d_file"
    fi
    if command -v appstreamcli >/dev/null 2>&1; then
        appstreamcli validate --no-net "$m_file"
    fi
}

ensure_appimagetool() {
    local tools_dir="${RAGO_LINUX_TOOLS_DIR:-$ci_tmp/tools}"
    mkdir -p "$tools_dir"
    local cached_bin="$tools_dir/appimagetool-${APPIMAGETOOL_TAG}-x86_64.AppImage"

    if [ -f "$cached_bin" ]; then
        if verify_sha256 "$cached_bin" "$APPIMAGETOOL_SHA256" 2>/dev/null; then
            chmod 0755 "$cached_bin"
            printf '%s\n' "$cached_bin"
            return 0
        fi
        echo "warning: cached appimagetool failed SHA-256 check; removing and re-downloading." >&2
        rm -f "$cached_bin"
    fi

    local dl_tmp="$ci_tmp/appimagetool-${APPIMAGETOOL_TAG}.download.$$"
    rm -f "$dl_tmp"
    echo "==> downloading appimagetool ${APPIMAGETOOL_TAG} (pinned SHA-256: ${APPIMAGETOOL_SHA256})" >&2
    curl --fail --location --silent --show-error --retry 3 --proto '=https' --tlsv1.2 \
        -o "$dl_tmp" "$APPIMAGETOOL_URL"
    chmod 0600 "$dl_tmp"

    if ! verify_sha256 "$dl_tmp" "$APPIMAGETOOL_SHA256"; then
        rm -f "$dl_tmp"
        echo "error: refusing to execute unverified appimagetool binary." >&2
        exit 1
    fi

    chmod 0755 "$dl_tmp"
    mv -f "$dl_tmp" "$cached_bin"
    verify_sha256 "$cached_bin" "$APPIMAGETOOL_SHA256"
    printf '%s\n' "$cached_bin"
}

echo "==> validating Freedesktop desktop & AppStream metainfo files"
validate_freedesktop_files "$desktop_src" "$metainfo_src"

if [ "$validate_packaging_only" -eq 1 ]; then
    node "$root/ci/generate-sbom.mjs" --verify-source-licenses
    # Self-test SHA-256 helper (FIPS 180-4 "abc" vector + tamper rejection) so CI/local gates verify checksum enforcement.
    probe="$ci_tmp/sha256-probe.bin"
    printf 'abc' > "$probe"
    expected_probe="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    verify_sha256 "$probe" "$expected_probe"
    if verify_sha256 "$probe" "0000000000000000000000000000000000000000000000000000000000000000" 2>/dev/null; then
        echo "error: SHA-256 verifier failed to reject a mismatched digest" >&2
        exit 1
    fi
    if [ -n "${RAGO_LINUX_TOOLS_DIR:-}" ]; then
        appimagetool_checked="$(ensure_appimagetool)"
        echo "==> verified pinned appimagetool in cache: $appimagetool_checked"
    fi
    echo "==> packaging metadata & guard validation passed ($product_id $version)"
    exit 0
fi

host_os="$(uname -s)"
host_arch="$(uname -m)"
if [ "$host_os" != "Linux" ] || [ "$host_arch" != "x86_64" ]; then
    echo "error: ci/build-linux.sh builds Linux x86_64 packages (current host: ${host_os}/${host_arch})." >&2
    exit 1
fi

: "${VCPKG_ROOT:?set VCPKG_ROOT to your vcpkg checkout}"
: "${Qt6_DIR:?set Qt6_DIR to your Qt6 prefix (e.g. ~/Qt/6.12.0/gcc_64)}"
[ -d "$VCPKG_ROOT" ] || { echo "error: VCPKG_ROOT directory does not exist: $VCPKG_ROOT" >&2; exit 1; }
[ -d "$Qt6_DIR" ] || { echo "error: Qt6_DIR directory does not exist: $Qt6_DIR" >&2; exit 1; }
if [ -f "$Qt6_DIR/Qt6Config.cmake" ]; then
    Qt6_DIR="$(cd "$Qt6_DIR/../../.." && pwd)"
elif [ ! -d "$Qt6_DIR/lib/cmake/Qt6" ]; then
    qt_cfg="$(find "$Qt6_DIR" -maxdepth 5 -name "Qt6Config.cmake" 2>/dev/null | sort -V | tail -n 1 || true)"
    [ -n "$qt_cfg" ] && Qt6_DIR="$(cd "$(dirname "$qt_cfg")/../../.." && pwd)"
fi
Qt6_DIR="$(cd "$Qt6_DIR" && pwd)"
export Qt6_DIR
[ -f "$Qt6_DIR/lib/cmake/Qt6/Qt6Config.cmake" ] || { echo "error: Qt6Config.cmake not found under Qt6_DIR ($Qt6_DIR)" >&2; exit 1; }

for tool in cmake ninja git node tar curl file patchelf ldd readelf; do
    command -v "$tool" >/dev/null 2>&1 || { echo "error: required tool '$tool' not found on PATH" >&2; exit 1; }
done

qmlimportscanner=""
for candidate in "$Qt6_DIR/libexec/qmlimportscanner" "$Qt6_DIR/bin/qmlimportscanner"; do
    if [ -x "$candidate" ]; then
        qmlimportscanner="$candidate"
        break
    fi
done
[ -n "$qmlimportscanner" ] || { echo "error: qmlimportscanner not found under Qt6_DIR ($Qt6_DIR)" >&2; exit 1; }

build_ld_path="$Qt6_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LD_LIBRARY_PATH="$build_ld_path"

export RAGOSHELL_CONFIG_DIR="$ci_tmp/cfg"
export RAGOSHELL_KEYSTORE=file
export XDG_CONFIG_HOME="$ci_tmp/xdg-config"
export XDG_DATA_HOME="$ci_tmp/xdg-data"
export XDG_CACHE_HOME="$ci_tmp/xdg-cache"
mkdir -p "$RAGOSHELL_CONFIG_DIR" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME"

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

# Install once into a clean FHS tree so CMake replaces build-tree RPATHs with $ORIGIN/../lib.
install_root="$ci_tmp/cmake-install"
DESTDIR="$install_root" cmake --install "$root/build/$preset" --prefix /usr

for bin in "$product_id" "$mcp_bin"; do
    if [ ! -x "$install_root/usr/bin/$bin" ]; then
        echo "error: expected installed executable not found: $bin" >&2
        exit 1
    fi
done

echo "==> staging tar.gz layout"
mkdir -p "$root/dist"
stage="$root/dist/$product_id-$version-$plat"
rm -rf "$stage"
mkdir -p "$stage"
cp "$install_root/usr/bin/$product_id" "$install_root/usr/bin/$mcp_bin" "$stage/"
cp "$desktop_src" "$metainfo_src" "$stage/"
cp -r "$root/install/linux/icons" "$stage/"
cmake --install "$root/build/$preset" --component licenses --prefix "$stage"
node "$root/ci/generate-sbom.mjs" --verify-licenses "$stage/licenses" --require-vcpkg-copyrights

echo "==> QML runtime load check on staged binary (English, ar, ja)"
rm -rf "$RAGOSHELL_CONFIG_DIR" && mkdir -p "$RAGOSHELL_CONFIG_DIR"
for lang_arg in "" "ar" "ja"; do
    args=("--check-qml")
    if [ -n "$lang_arg" ]; then
        args+=("--lang" "$lang_arg")
    fi
    LD_LIBRARY_PATH="$build_ld_path" QT_PLUGIN_PATH="$Qt6_DIR/plugins" QT_QPA_PLATFORM=offscreen \
        "$stage/$product_id" "${args[@]}"
done

echo "==> packaging tar.gz"
node "$root/ci/generate-sbom.mjs" --verify-licenses "$stage/licenses" --require-vcpkg-copyrights
rm -f "$stage.tar.gz"
tar -C "$root/dist" -czf "$stage.tar.gz" "$(basename "$stage")"
rm -rf "$stage"

echo "==> assembling self-contained AppDir"
appdir="$ci_tmp/AppDir"
rm -rf "$appdir"
mkdir -p "$appdir"
cp -a "$install_root/usr" "$appdir/usr"
rm -rf "$appdir/usr/licenses"
mkdir -p "$appdir/usr/lib" "$appdir/usr/plugins" "$appdir/usr/qml"
cmake --install "$root/build/$preset" --component licenses --prefix "$appdir"
node "$root/ci/generate-sbom.mjs" --verify-licenses "$appdir/licenses" --require-vcpkg-copyrights

ln -s "usr/share/applications/$product_id.desktop" "$appdir/$product_id.desktop"
ln -s "usr/share/icons/hicolor/256x256/apps/$product_id.png" "$appdir/$product_id.png"
ln -s "$product_id.png" "$appdir/.DirIcon"

cat > "$appdir/usr/bin/qt.conf" <<'EOF'
[Paths]
Prefix = ..
Plugins = plugins
Imports = qml
Qml2Imports = qml
EOF

cat > "$appdir/AppRun" <<EOF
#!/usr/bin/env bash
set -euo pipefail
HERE="\$(dirname "\$(readlink -f "\$0")")"
export XDG_DATA_DIRS="\$HERE/usr/share\${XDG_DATA_DIRS:+:\$XDG_DATA_DIRS}"
invoked_as="\$(basename "\${ARGV0:-\$0}")"
if [ "\$invoked_as" = "$mcp_bin" ]; then
    exec "\$HERE/usr/bin/$mcp_bin" "\$@"
fi
if [ "\${1:-}" = "--mcp" ]; then
    shift
    exec "\$HERE/usr/bin/$mcp_bin" "\$@"
fi
exec "\$HERE/usr/bin/$product_id" "\$@"
EOF
chmod 0755 "$appdir/AppRun"

echo "==> deploying Qt 6 plugins, QML modules, and shared libraries into AppDir"
"$qmlimportscanner" -rootPath "$root/src/ragoshell-app/qml" -importPath "$Qt6_DIR/qml" > "$ci_tmp/qml-imports.json"

RAGO_QT_DIR="$Qt6_DIR" \
RAGO_APPDIR="$appdir" \
RAGO_QML_JSON="$ci_tmp/qml-imports.json" \
RAGO_BUILD_LD="$build_ld_path" \
RAGO_PRODUCT_ID="$product_id" \
RAGO_PRODUCT_NAME="$product_name" \
node - <<'EOF'
const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');

const qtDir = fs.realpathSync(process.env.RAGO_QT_DIR);
const qtLibDir = path.join(qtDir, 'lib');
const qtPluginsDir = path.join(qtDir, 'plugins');
const qtQmlDir = path.join(qtDir, 'qml');
const appDir = fs.realpathSync(process.env.RAGO_APPDIR);
const usrDir = path.join(appDir, 'usr');
const dstLibDir = path.join(usrDir, 'lib');
const dstPluginsDir = path.join(usrDir, 'plugins');
const dstQmlDir = path.join(usrDir, 'qml');
const productId = process.env.RAGO_PRODUCT_ID;
const productName = process.env.RAGO_PRODUCT_NAME;
const buildLd = process.env.RAGO_BUILD_LD || qtLibDir;

const copyFileDereferenced = (src, dst, mode = 0o755) => {
    const real = fs.realpathSync(src);
    fs.mkdirSync(path.dirname(dst), { recursive: true });
    fs.copyFileSync(real, dst);
    fs.chmodSync(dst, mode);
};

// 1. Copy required Qt 6 plugin families (Wayland, XCB, offscreen, TLS, imageformats, iconengines, input, XDG portal).
const copyPluginFiles = (subdir, filterFn, required = false) => {
    const srcDir = path.join(qtPluginsDir, subdir);
    if (!fs.existsSync(srcDir)) {
        if (required) {
            throw new Error(`Required Qt plugin directory missing: ${srcDir}`);
        }
        return;
    }
    let copied = 0;
    for (const entry of fs.readdirSync(srcDir, { withFileTypes: true })) {
        if (!entry.isFile() && !entry.isSymbolicLink()) continue;
        if (!entry.name.endsWith('.so')) continue;
        if (filterFn && !filterFn(entry.name)) continue;
        copyFileDereferenced(path.join(srcDir, entry.name), path.join(dstPluginsDir, subdir, entry.name));
        copied++;
    }
    if (required && copied === 0) {
        throw new Error(`No matching Qt plugins found in required directory: ${srcDir}`);
    }
};

const wantedPlatforms = new Set([
    'libqxcb.so',
    'libqwayland-generic.so',
    'libqwayland-egl.so',
    'libqoffscreen.so',
    'libqminimal.so',
]);
copyPluginFiles('platforms', (n) => wantedPlatforms.has(n), true);
for (const mustPlatform of ['libqxcb.so', 'libqoffscreen.so']) {
    if (!fs.existsSync(path.join(dstPluginsDir, 'platforms', mustPlatform))) {
        throw new Error(`Required Qt platform plugin missing: platforms/${mustPlatform}`);
    }
}
copyPluginFiles('xcbglintegrations', null, false);
copyPluginFiles('wayland-graphics-integration-client', null, false);
copyPluginFiles('wayland-decoration-client', null, false);
copyPluginFiles('wayland-shell-integration', null, false);
copyPluginFiles('tls', null, true);
if (!fs.existsSync(path.join(dstPluginsDir, 'tls', 'libqopensslbackend.so'))) {
    throw new Error('Required Qt TLS plugin missing: tls/libqopensslbackend.so');
}
copyPluginFiles('imageformats', (n) => n !== 'libqpdf.so', true);
copyPluginFiles('iconengines', null, true);
copyPluginFiles('platforminputcontexts', (n) => n !== 'libqtvirtualkeyboardplugin.so', false);
// Bundle XDG Desktop Portal theme integration while omitting libqgtk3.so (avoids host GTK3/Pango ABI clashes).
copyPluginFiles('platformthemes', (n) => n === 'libqxdgdesktopportal.so', false);
copyPluginFiles('networkinformation', (n) => n === 'libqnetworkmanager.so', false);

// 2. Copy QML modules discovered by qmlimportscanner (excluding Apple/Windows-only styles and sibling unimported modules).
const skipQmlModulePrefixes = [
    'QtQuick/Controls/macOS',
    'QtQuick/Controls/iOS',
    'QtQuick/Controls/FluentWinUI3',
];
const copyQmlModuleTree = (srcModDir, dstModDir, isRoot = true) => {
    if (!fs.existsSync(srcModDir)) return;
    if (!isRoot && fs.existsSync(path.join(srcModDir, 'qmldir'))) {
        // Separate child QML module: copied only if qmlimportscanner explicitly listed its relativePath.
        return;
    }
    fs.mkdirSync(dstModDir, { recursive: true });
    for (const entry of fs.readdirSync(srcModDir, { withFileTypes: true })) {
        const s = path.join(srcModDir, entry.name);
        const d = path.join(dstModDir, entry.name);
        const stat = fs.statSync(s);
        if (stat.isDirectory()) {
            copyQmlModuleTree(s, d, false);
        } else if (stat.isFile()) {
            if (entry.name.endsWith('.debug') || entry.name.endsWith('.a') || entry.name.endsWith('.prl')) continue;
            const mode = entry.name.endsWith('.so') ? 0o755 : 0o644;
            copyFileDereferenced(s, d, mode);
        }
    }
};

const qmlImports = JSON.parse(fs.readFileSync(process.env.RAGO_QML_JSON, 'utf8'));
const seenRelPaths = new Set();
for (const item of qmlImports) {
    const rel = item.relativePath;
    if (!rel || item.name === productName || seenRelPaths.has(rel)) continue;
    if (skipQmlModulePrefixes.some((p) => rel === p || rel.startsWith(p + '/'))) continue;
    const srcMod = path.join(qtQmlDir, rel);
    if (!fs.existsSync(srcMod)) continue;
    seenRelPaths.add(rel);
    copyQmlModuleTree(srcMod, path.join(dstQmlDir, rel), true);
}
if (fs.existsSync(path.join(qtQmlDir, 'QtCore')) && !seenRelPaths.has('QtCore')) {
    copyQmlModuleTree(path.join(qtQmlDir, 'QtCore'), path.join(dstQmlDir, 'QtCore'), true);
}

// 3. Collect all ELF executables and plugins, resolve recursive shared library dependencies via ldd,
//    and bundle non-base-OS libraries into AppDir/usr/lib.
// Base system / GPU / display-socket / NSS / fontconfig libraries that must come from the host OS per AppImage spec:
const hostExcludedSoname = /^(?:linux-vdso\.so|ld-linux|libc\.so|libm\.so|libdl\.so|librt\.so|libpthread\.so|libresolv\.so|libnsl\.so|libutil\.so|libcrypt\.so|libanl\.so|libBrokenLocale\.so|libthread_db\.so|libgcc_s\.so|libstdc\+\+\.so|libGL\.so|libGLX\.so|libOpenGL\.so|libGLdispatch\.so|libEGL\.so|libGLESv1_CM\.so|libGLESv2\.so|libvulkan\.so|libdrm\.so|libgbm\.so|libX11\.so|libX11-xcb\.so|libxcb\.so\.1$|libwayland-client\.so|libwayland-cursor\.so|libwayland-egl\.so|libwayland-server\.so|libfontconfig\.so|libfreetype\.so|libharfbuzz\.so|libdbus-1\.so|libsystemd\.so|libudev\.so|libglib-2\.0\.so|libgobject-2\.0\.so|libgio-2\.0\.so|libgmodule-2\.0\.so|libgthread-2\.0\.so|libexpat\.so|libz\.so|libselinux\.so|libmount\.so|libblkid\.so|libuuid\.so|libp11-kit\.so|libcom_err\.so|libkrb5\.so|libkrb5support\.so|libk5crypto\.so|libgssapi_krb5\.so|libkeyutils\.so|libcap\.so|libgpg-error\.so|libsecret-1\.so|libssl\.so|libcrypto\.so)/;

const walkFiles = (dir, out = []) => {
    if (!fs.existsSync(dir)) return out;
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
        const full = path.join(dir, entry.name);
        if (entry.isSymbolicLink()) continue;
        if (entry.isDirectory()) walkFiles(full, out);
        else if (entry.isFile()) out.push(full);
    }
    return out;
};

const isElf = (filePath) => {
    const fd = fs.openSync(filePath, 'r');
    try {
        const hdr = Buffer.alloc(4);
        const n = fs.readSync(fd, hdr, 0, 4, 0);
        return n === 4 && hdr[0] === 0x7f && hdr[1] === 0x45 && hdr[2] === 0x4c && hdr[3] === 0x46;
    } finally {
        fs.closeSync(fd);
    }
};

const queue = [
    path.join(usrDir, 'bin', productId),
    path.join(usrDir, 'bin', `${productId}-mcp`),
    ...walkFiles(dstPluginsDir).filter(isElf),
    ...walkFiles(dstQmlDir).filter(isElf),
];
const processedElf = new Set();
const bundledSonames = new Set();
const lddEnv = { ...process.env, LD_LIBRARY_PATH: `${dstLibDir}:${buildLd}` };

while (queue.length > 0) {
    const current = queue.shift();
    if (processedElf.has(current)) continue;
    processedElf.add(current);

    const out = execFileSync('ldd', [current], { env: lddEnv, encoding: 'utf8' });
    for (const rawLine of out.split('\n')) {
        const line = rawLine.trim();
        if (!line) continue;
        if (line.includes('=> not found')) {
            throw new Error(`Unresolved shared library in ${current}: ${line}`);
        }
        const m = line.match(/^(\S+)\s+=>\s+(\/\S+)\s+\(0x[0-9a-fA-F]+\)$/);
        if (!m) continue;
        const [, soname, resolvedPath] = m;
        const realResolved = fs.realpathSync(resolvedPath);
        const fromQtPrefix = realResolved === qtDir || realResolved.startsWith(qtDir + path.sep);

        if (!fromQtPrefix && hostExcludedSoname.test(soname)) {
            continue;
        }
        if (bundledSonames.has(soname)) continue;
        bundledSonames.add(soname);

        const dstSo = path.join(dstLibDir, soname);
        copyFileDereferenced(realResolved, dstSo, 0o755);
        queue.push(dstSo);
    }
}

// 4. Rewrite RUNPATH on every ELF inside AppDir/usr to relative $ORIGIN paths only, and verify zero host paths remain.
const allElfs = walkFiles(usrDir).filter(isElf);
for (const elf of allElfs) {
    const elfDir = path.dirname(elf);
    const relToLib = path.relative(elfDir, dstLibDir).split(path.sep).join('/');
    const rpath = relToLib === '' ? '$ORIGIN' : `$ORIGIN/${relToLib}`;
    execFileSync('patchelf', ['--set-rpath', rpath, elf], { stdio: 'inherit' });
}

for (const elf of allElfs) {
    const printed = execFileSync('patchelf', ['--print-rpath', elf], { encoding: 'utf8' }).trim();
    for (const entry of printed.split(':').filter(Boolean)) {
        if (!entry.startsWith('$ORIGIN')) {
            throw new Error(`Non-relocatable RPATH '${entry}' remained in ${elf}`);
        }
    }
}
console.log(`==> bundled ${bundledSonames.size} shared libraries and patched ${allElfs.length} ELF files in AppDir`);
EOF

validate_freedesktop_files \
    "$appdir/usr/share/applications/$product_id.desktop" \
    "$appdir/usr/share/metainfo/$bundle_id.metainfo.xml"
node "$root/ci/generate-sbom.mjs" --verify-licenses "$appdir/licenses" --require-vcpkg-copyrights

appimagetool_bin="$(ensure_appimagetool)"
appimage_out="$root/dist/$product_id-$version-$plat.AppImage"
rm -f "$appimage_out"

echo "==> building AppImage ($appimage_out)"
ARCH=x86_64 VERSION="$version" APPIMAGE_EXTRACT_AND_RUN=1 \
    "$appimagetool_bin" --appimage-extract-and-run --no-appstream "$appdir" "$appimage_out"
chmod 0755 "$appimage_out"

if [ ! -s "$appimage_out" ] || [ ! -s "$stage.tar.gz" ]; then
    echo "error: expected Linux release artifacts were not produced in $root/dist" >&2
    exit 1
fi

echo "==> isolated AppImage verification (LD_LIBRARY_PATH and QT_PLUGIN_PATH unset; en, ar, ja + MCP)"
rm -rf "$RAGOSHELL_CONFIG_DIR" && mkdir -p "$RAGOSHELL_CONFIG_DIR"
ld_debug_log="$ci_tmp/appimage-ld-debug.log"
for lang_arg in "" "ar" "ja"; do
    args=("--appimage-extract-and-run" "--check-qml")
    if [ -n "$lang_arg" ]; then
        args+=("--lang" "$lang_arg")
    fi
    if [ -z "$lang_arg" ]; then
        env -u LD_LIBRARY_PATH \
            -u QT_PLUGIN_PATH \
            -u QML2_IMPORT_PATH \
            -u QML_IMPORT_PATH \
            -u QT_QPA_PLATFORM_PLUGIN_PATH \
            -u Qt6_DIR \
            APPIMAGE_EXTRACT_AND_RUN=1 \
            QT_QPA_PLATFORM=offscreen \
            LD_DEBUG=libs \
            LD_DEBUG_OUTPUT="$ld_debug_log" \
            "$appimage_out" "${args[@]}"
    else
        env -u LD_LIBRARY_PATH \
            -u QT_PLUGIN_PATH \
            -u QML2_IMPORT_PATH \
            -u QML_IMPORT_PATH \
            -u QT_QPA_PLATFORM_PLUGIN_PATH \
            -u Qt6_DIR \
            APPIMAGE_EXTRACT_AND_RUN=1 \
            QT_QPA_PLATFORM=offscreen \
            "$appimage_out" "${args[@]}"
    fi
done

if grep -Fq "$Qt6_DIR" "$ld_debug_log"* 2>/dev/null; then
    echo "error: AppImage loaded a library from host Qt6_DIR ($Qt6_DIR) instead of bundled AppDir/usr/lib!" >&2
    grep -Fn "$Qt6_DIR" "$ld_debug_log"* >&2 || true
    exit 1
fi

# Verify companion MCP server reachability via --mcp and via ARGV0 symlink without host Qt paths.
env -u LD_LIBRARY_PATH \
    -u QT_PLUGIN_PATH \
    -u QML2_IMPORT_PATH \
    -u QML_IMPORT_PATH \
    -u QT_QPA_PLATFORM_PLUGIN_PATH \
    -u Qt6_DIR \
    APPIMAGE_EXTRACT_AND_RUN=1 \
    "$appimage_out" --appimage-extract-and-run --mcp --status >/dev/null

ln -sf "$appimage_out" "$ci_tmp/$mcp_bin"
env -u LD_LIBRARY_PATH \
    -u QT_PLUGIN_PATH \
    -u QML2_IMPORT_PATH \
    -u QML_IMPORT_PATH \
    -u QT_QPA_PLATFORM_PLUGIN_PATH \
    -u Qt6_DIR \
    APPIMAGE_EXTRACT_AND_RUN=1 \
    "$ci_tmp/$mcp_bin" --status >/dev/null

echo "==> done:"
echo "    $appimage_out"
echo "    $stage.tar.gz"
