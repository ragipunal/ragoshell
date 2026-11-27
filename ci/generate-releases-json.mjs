#!/usr/bin/env node
/**
 * Zero-dependency release manifest (`releases.json`) and checksum (`SHA256SUMS.txt`)
 * generator and verifier for RagoShell (Roadmap Phase 3.6, docs/ReleasePipeline.md).
 *
 * Security & single-source guarantees:
 * - Reads brand identity and domain strictly from `cmake/RagoShellDefaults.cmake`.
 * - NEVER reads, sources, or prints `ci/.env`.
 * - Enforces strict SemVer, HTTPS-only URLs, credential-free URLs, and host allowlist
 *   (`dl.<domain>`, `play.google.com`, `apps.apple.com`) matching `ragoshell-site`
 *   and `services/release-pulse-api/server.mjs`.
 * - Refuses symlinks, path traversal, 0-byte files, mixed-version stale artifacts,
 *   and missing required primary desktop installers (`-setup.exe`, `.dmg`, `.AppImage`).
 */

import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const DEFAULT_REPO_ROOT = path.resolve(__dirname, '..');

// SemVer 2.0.0 §9: a numeric pre-release identifier has no leading zero ("rc.01" is invalid); an alphanumeric one may
// contain digits and hyphens anywhere ("rc-2", "0a").
const SEMVER_PRE_ID = String.raw`(?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*)`;
const STRICT_SEMVER_RE = new RegExp(
  String.raw`^(?:0|[1-9]\d{0,3})\.(?:0|[1-9]\d{0,3})\.(?:0|[1-9]\d{0,3})(?:-${SEMVER_PRE_ID}(?:\.${SEMVER_PRE_ID})*)?$`
);
const DATE_YMD_RE = /^\d{4}-\d{2}-\d{2}$/;
const SHA256_HEX_RE = /^[0-9a-f]{64}$/;
const LOCALE_KEY_RE = /^[a-z]{2}(?:-[A-Z]{2})?$/;
const SAFE_BRAND_TOKEN_RE = /^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/;
const CHANGELOG_HEADING_RE = /^\S.*?\s+(\d+\.\d+(?:\.\d+)?(?:[-+][0-9A-Za-z.-]+)?)$/;
const UNDERLINE_DASH_RE = /^-{3,}$/;
const UNDERLINE_EQ_RE = /^={3,}$/;

const PACKAGE_EXTENSIONS = ['.tar.gz', '-setup.exe', '.exe', '.msi', '.zip', '.dmg', '.AppImage', '.apk', '.aab'];
const IGNORED_DIST_FILES = new Set([
  '.DS_Store',
  'releases.json',
  'releases.json.minisig',
  'SHA256SUMS.txt',
  'SHA256SUMS.txt.minisig',
  'RELEASE_NOTES.md',
]);

const PLATFORM_LABELS = {
  windows: 'Windows',
  macos: 'macOS',
  linux: 'Linux',
  android: 'Android',
  ios: 'App Store',
};

const VALID_PLATFORM_FORMATS = {
  windows: new Map([
    ['-setup.exe', { format: 'exe', primary: true }],
    ['.msi', { format: 'msi', primary: false }],
    ['.zip', { format: 'zip', primary: false }],
  ]),
  macos: new Map([
    ['.dmg', { format: 'dmg', primary: true }],
    ['.tar.gz', { format: 'tar.gz', primary: false }],
  ]),
  linux: new Map([
    ['.AppImage', { format: 'AppImage', primary: true }],
    ['.tar.gz', { format: 'tar.gz', primary: false }],
  ]),
  android: new Map([
    ['.apk', { format: 'apk', primary: true }],
    ['.aab', { format: 'aab', primary: false }],
  ]),
};

function escapeRegExp(str) {
  return str.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

/**
 * Normalizes a tag or version (e.g. "v2.1.0" -> "2.1.0") and validates strict SemVer.
 */
export function normalizeAndValidateVersion(rawVersion, fieldName = 'version') {
  if (typeof rawVersion !== 'string') {
    throw new Error(`Invalid ${fieldName}: expected a string.`);
  }
  let clean = rawVersion.trim();
  if (/^[vV]\d/.test(clean)) {
    clean = clean.slice(1);
  }
  if (
    clean.length === 0 ||
    clean.length > 32 ||
    clean.includes('..') ||
    !STRICT_SEMVER_RE.test(clean)
  ) {
    throw new Error(
      `Invalid ${fieldName} "${rawVersion}": expected strict SemVer (e.g. "2.1.0" or "2.1.0-rc.1", <= 32 chars).`
    );
  }
  return clean;
}

/**
 * Compares two validated SemVer strings (-1 if a < b, 0 if a == b, +1 if a > b).
 */
export function compareSemVer(lhs, rhs) {
  const a = normalizeAndValidateVersion(lhs, 'lhs version');
  const b = normalizeAndValidateVersion(rhs, 'rhs version');
  // core = before the first '-', pre-release = everything after it (it may contain more hyphens: "rc-2")
  const splitCore = (v) => {
    const dash = v.indexOf('-');
    return dash < 0 ? [v, ''] : [v.slice(0, dash), v.slice(dash + 1)];
  };
  const [aCore, aPre] = splitCore(a);
  const [bCore, bPre] = splitCore(b);
  const aNums = aCore.split('.').map((n) => Number.parseInt(n, 10));
  const bNums = bCore.split('.').map((n) => Number.parseInt(n, 10));

  for (let i = 0; i < 3; i += 1) {
    if (aNums[i] !== bNums[i]) {
      return aNums[i] < bNums[i] ? -1 : 1;
    }
  }
  if (!aPre && bPre) return 1;
  if (aPre && !bPre) return -1;
  if (aPre === bPre) return 0;

  // SemVer 2.0.0 §11.4.1: compare dot-separated pre-release identifiers numerically when both are digits.
  const aIds = aPre.split('.');
  const bIds = bPre.split('.');
  const len = Math.max(aIds.length, bIds.length);
  for (let i = 0; i < len; i += 1) {
    if (i >= aIds.length) return -1;
    if (i >= bIds.length) return 1;
    const x = aIds[i];
    const y = bIds[i];
    if (x === y) continue;
    const xNum = /^\d+$/.test(x);
    const yNum = /^\d+$/.test(y);
    if (xNum && yNum) {
      const xn = BigInt(x);
      const yn = BigInt(y);
      return xn < yn ? -1 : 1;
    }
    if (xNum && !yNum) return -1;
    if (!xNum && yNum) return 1;
    return x < y ? -1 : 1;
  }
  return 0;
}

/**
 * Reads product identity exclusively from `cmake/RagoShellDefaults.cmake`.
 */
export function readBrandDefaults(repoRoot = DEFAULT_REPO_ROOT) {
  const defaultsPath = path.join(path.resolve(repoRoot), 'cmake', 'RagoShellDefaults.cmake');
  const text = fs.readFileSync(defaultsPath, 'utf8');
  const values = Object.create(null);
  for (const line of text.split(/\r?\n/)) {
    const m = /^set\(([A-Z0-9_]+)\s+"([^"]*)"\)\s*$/.exec(line.trim());
    if (m) {
      values[m[1]] = m[2];
    }
  }

  const productName = values.RAGO_PRODUCT_NAME || '';
  const productId = values.RAGO_PRODUCT_ID || '';
  const githubOwner = values.RAGO_GITHUB_OWNER || productId;
  const bundleId = values.RAGO_BUNDLE_ID || '';
  const domain = values.RAGO_DOMAIN || '';
  const productUrl = values.RAGO_PRODUCT_URL || '';
  const minisignPubKey = values.RAGO_MINISIGN_PUBKEY || '';

  if (
    !productName ||
    !SAFE_BRAND_TOKEN_RE.test(productId) ||
    !SAFE_BRAND_TOKEN_RE.test(bundleId) ||
    !SAFE_BRAND_TOKEN_RE.test(domain) ||
    !productUrl
  ) {
    throw new Error(`Incomplete or invalid brand defaults in ${defaultsPath}.`);
  }

  const dlHost = `dl.${domain}`;
  const allowedHosts = [dlHost, 'play.google.com', 'apps.apple.com'];

  return {
    productName,
    productId,
    githubOwner,
    bundleId,
    domain,
    productUrl,
    minisignPubKey,
    dlHost,
    defaultDlBaseUrl: `https://${dlHost}`,
    defaultAllDownloadsUrl: `https://${dlHost}/releases/`,
    allowedHosts,
  };
}

/**
 * Reads default PROJECT_VERSION from top-level CMakeLists.txt.
 */
export function readDefaultCmakeVersion(repoRoot = DEFAULT_REPO_ROOT) {
  const cmakePath = path.join(path.resolve(repoRoot), 'CMakeLists.txt');
  const text = fs.readFileSync(cmakePath, 'utf8');
  const readVar = (name) => {
    const m = new RegExp(`^set\\(${escapeRegExp(name)}\\s+"([^"]*)"\\)`, 'm').exec(text);
    return m ? m[1] : '';
  };
  const maj = readVar('PROJECT_VERSION_MAJOR');
  const min = readVar('PROJECT_VERSION_MINOR');
  const pat = readVar('PROJECT_VERSION_PATCH');
  const bld = readVar('PROJECT_VERSION_BUILD');
  if (!maj || !min || !pat) {
    throw new Error(`Could not determine PROJECT_VERSION from ${cmakePath}.`);
  }
  return normalizeAndValidateVersion(`${maj}.${min}.${pat}${bld}`, 'CMake PROJECT_VERSION');
}

/**
 * Validates that a URL uses https://, contains no credentials, has no traversal sequences,
 * fits within 1024 characters, and resolves to an allowlisted hostname.
 */
export function validateAllowedHttpsUrl(rawUrl, allowedHosts, label = 'URL') {
  if (typeof rawUrl !== 'string') {
    throw new Error(`Invalid ${label}: expected string.`);
  }
  const trimmed = rawUrl.trim();
  if (trimmed.length === 0 || trimmed.length > 1024) {
    throw new Error(`Invalid ${label}: must be 1..1024 characters.`);
  }
  if (/[\x00-\x20\x7f\\]/.test(trimmed)) {
    throw new Error(`Invalid ${label}: contains whitespace, backslash, or control characters.`);
  }
  const schemeEnd = trimmed.indexOf('://');
  if (schemeEnd > 0) {
    const afterHostSlash = trimmed.indexOf('/', schemeEnd + 3);
    if (afterHostSlash >= 0) {
      const rawPath = trimmed.slice(afterHostSlash).split(/[?#]/, 1)[0];
      if (/(?:^|\/)\.\.(?:\/|$)/.test(rawPath) || /%2e%2e/i.test(rawPath) || rawPath.includes('//')) {
        throw new Error(`Invalid ${label} "${trimmed}": path traversal or double slash detected.`);
      }
    }
  }

  let parsed;
  try {
    parsed = new URL(trimmed);
  } catch {
    throw new Error(`Invalid ${label} "${trimmed}": malformed URL.`);
  }

  if (parsed.protocol !== 'https:') {
    throw new Error(`Invalid ${label} "${trimmed}": scheme must be https://.`);
  }
  if (parsed.username !== '' || parsed.password !== '') {
    throw new Error(`Invalid ${label}: embedded credentials are forbidden.`);
  }
  if (!Array.isArray(allowedHosts) || !allowedHosts.includes(parsed.hostname)) {
    throw new Error(
      `Invalid ${label} host "${parsed.hostname}": allowed hosts are [${allowedHosts.join(', ')}].`
    );
  }
  if (parsed.pathname.includes('..') || parsed.pathname.includes('//') || parsed.pathname.includes('\\')) {
    throw new Error(`Invalid ${label} "${trimmed}": path traversal or double slash detected.`);
  }

  return parsed.href;
}

export function validatePublishedDate(dateStr) {
  if (typeof dateStr !== 'string' || !DATE_YMD_RE.test(dateStr.trim())) {
    throw new Error(`Invalid publishedAt "${dateStr}": expected YYYY-MM-DD.`);
  }
  const clean = dateStr.trim();
  const parsed = new Date(`${clean}T00:00:00Z`);
  if (Number.isNaN(parsed.getTime()) || parsed.toISOString().slice(0, 10) !== clean) {
    throw new Error(`Invalid publishedAt "${dateStr}": not a valid calendar date.`);
  }
  return clean;
}

/**
 * Parses the repository CHANGELOG using the same strict grammar as `ragoshell-site/src/lib/changelog.ts`.
 */
export function parseChangelog(text) {
  const lines = text.split(/\r?\n/).map((l) => l.replace(/\s+$/, ''));
  const releases = [];
  const fail = (lineIdx, msg) => {
    throw new Error(`CHANGELOG line ${lineIdx + 1}: ${msg}: "${lines[lineIdx] ?? ''}"`);
  };

  let i = 0;
  while (i < lines.length && lines[i] === '') i += 1;
  if (i >= lines.length || !UNDERLINE_EQ_RE.test(lines[i + 1] ?? '')) {
    fail(Math.min(i, Math.max(0, lines.length - 1)), 'expected title underlined with "="');
  }
  i += 2;

  let current = null;
  let introLines = [];
  let lastBullet = -1;

  const finishIntro = () => {
    if (current && introLines.length > 0) {
      current.intro = introLines.join(' ');
    }
    introLines = [];
  };

  for (; i < lines.length; i += 1) {
    const line = lines[i];
    if (line === '') {
      lastBullet = -1;
      continue;
    }
    if (!/^\s/.test(line) && !line.startsWith('- ') && UNDERLINE_DASH_RE.test(lines[i + 1] ?? '')) {
      finishIntro();
      const m = CHANGELOG_HEADING_RE.exec(line);
      if (!m) fail(i, 'release heading without a version');
      current = { version: m[1], intro: '', items: [] };
      releases.push(current);
      lastBullet = -1;
      i += 1;
      continue;
    }
    if (!current) fail(i, 'text before first release heading');
    if (line.startsWith('- ')) {
      finishIntro();
      current.items.push(line.slice(2).trim());
      lastBullet = current.items.length - 1;
    } else if (/^\s+\S/.test(line)) {
      if (lastBullet < 0) fail(i, 'indented continuation line without a bullet');
      current.items[lastBullet] += ` ${line.trim()}`;
    } else if (current.items.length === 0) {
      introLines.push(line.trim());
    } else {
      fail(i, 'unexpected text after release bullets');
    }
  }
  finishIntro();

  if (releases.length === 0) {
    throw new Error('CHANGELOG contains no release sections.');
  }
  for (const r of releases) {
    if (r.items.length === 0) {
      throw new Error(`CHANGELOG release ${r.version} contains no bullet items.`);
    }
  }
  return releases;
}

function validatePlainNoteString(text, label) {
  if (typeof text !== 'string') {
    throw new Error(`Invalid ${label}: expected string.`);
  }
  const clean = text.trim();
  if (clean.length === 0 || clean.length > 2000) {
    throw new Error(`Invalid ${label}: must be 1..2000 characters.`);
  }
  if (/[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]/.test(clean)) {
    throw new Error(`Invalid ${label}: contains forbidden control characters.`);
  }
  if (/<\/?[a-z!][\s\S]*?>/i.test(clean)) {
    throw new Error(`Invalid ${label}: raw HTML markup is forbidden in releaseNotes.`);
  }
  return clean;
}

export function validateReleaseNotes(notes) {
  if (typeof notes === 'string') {
    return { en: validatePlainNoteString(notes, 'releaseNotes') };
  }
  if (!notes || typeof notes !== 'object' || Array.isArray(notes)) {
    throw new Error('releaseNotes must be a plain-text string or localized object { en: "..." }.');
  }
  const out = {};
  for (const [lang, val] of Object.entries(notes)) {
    if (!LOCALE_KEY_RE.test(lang)) {
      throw new Error(`Invalid locale key "${lang}" in releaseNotes.`);
    }
    out[lang] = validatePlainNoteString(val, `releaseNotes.${lang}`);
  }
  if (!out.en) {
    throw new Error('releaseNotes must include an "en" entry.');
  }
  return out;
}

export function summarizeChangelogRelease(entry, maxLen = 480) {
  if (entry.intro && entry.intro.length <= maxLen) {
    return entry.intro.replace(/`([^`]+)`/g, '$1');
  }
  const headlines = entry.items.slice(0, 3).map((item) => {
    const plain = item.replace(/`([^`]+)`/g, '$1');
    const firstClause = plain.split(/:\s+|\.\s+/)[0].trim();
    return firstClause.endsWith('.') ? firstClause.slice(0, -1) : firstClause;
  });
  let joined = `${headlines.join('; ')}.`;
  if (joined.length > maxLen) {
    joined = `${joined.slice(0, maxLen - 1).replace(/\s+\S*$/, '')}…`;
  }
  return joined;
}

export function formatMarkdownReleaseNotes(productName, version, changelogEntry, artifacts, sbom = null) {
  const lines = [`## ${productName} ${version}`, ''];
  if (changelogEntry) {
    if (changelogEntry.intro) {
      lines.push(changelogEntry.intro, '');
    }
    for (const item of changelogEntry.items) {
      lines.push(`- ${item}`);
    }
    lines.push('');
  }
  const rows = Array.isArray(artifacts) ? [...artifacts] : [];
  if (sbom && typeof sbom === 'object' && sbom.name && sbom.sha256) {
    rows.push(sbom);
    rows.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
  }
  if (rows.length > 0) {
    lines.push('### Checksums (SHA-256)', '', '| File | Size (bytes) | SHA-256 |', '|---|---:|---|');
    for (const a of rows) {
      lines.push(`| \`${a.name}\` | ${a.size} | \`${a.sha256}\` |`);
    }
    lines.push('');
  }
  return `${lines.join('\n')}\n`;
}

export function computeFileSha256(filePath) {
  const hash = crypto.createHash('sha256');
  const fd = fs.openSync(filePath, 'r');
  try {
    const buf = Buffer.allocUnsafe(64 * 1024);
    let bytesRead = 0;
    while ((bytesRead = fs.readSync(fd, buf, 0, buf.length, null)) > 0) {
      hash.update(buf.subarray(0, bytesRead));
    }
  } finally {
    fs.closeSync(fd);
  }
  return hash.digest('hex');
}

function hasAnyPackageSuffix(filename) {
  return PACKAGE_EXTENSIONS.some((ext) => filename.endsWith(ext));
}

function parseArtifactFilename(filename, productId, version) {
  if (
    filename.includes('/') ||
    filename.includes('\\') ||
    filename.includes('..') ||
    /[\x00-\x20\x7f]/.test(filename)
  ) {
    throw new Error(`Unsafe artifact filename "${filename}".`);
  }

  const prefix = `${productId}-${version}-`;
  if (!filename.startsWith(prefix)) {
    throw new Error(
      `Artifact "${filename}" does not match target release prefix "${prefix}" (remove stale or foreign files from dist/).`
    );
  }

  const rest = filename.slice(prefix.length);
  const m = /^(windows|macos|linux|android)-(x64|arm64|universal)(-setup\.exe|\.msi|\.zip|\.dmg|\.tar\.gz|\.AppImage|\.apk|\.aab)$/.exec(rest);
  if (!m) {
    throw new Error(`Unrecognized artifact naming convention: "${filename}".`);
  }

  const [, platform, arch, suffix] = m;
  const spec = VALID_PLATFORM_FORMATS[platform]?.get(suffix);
  if (!spec) {
    throw new Error(`Unsupported package suffix "${suffix}" for platform "${platform}" in "${filename}".`);
  }

  return {
    name: filename,
    platform,
    arch,
    format: spec.format,
    primary: spec.primary,
  };
}

/**
 * Generates `releases.json` and `SHA256SUMS.txt` from a release `dist/` directory.
 */
export function generateReleaseManifest(options = {}) {
  const repoRoot = path.resolve(options.repoRoot || DEFAULT_REPO_ROOT);
  const brand = readBrandDefaults(repoRoot);
  const version = normalizeAndValidateVersion(
    options.version || readDefaultCmakeVersion(repoRoot),
    'version'
  );
  const minSupportedVersion = normalizeAndValidateVersion(
    options.minSupportedVersion || '1.8.0',
    'minSupportedVersion'
  );
  if (compareSemVer(minSupportedVersion, version) > 0) {
    throw new Error(
      `minSupportedVersion (${minSupportedVersion}) cannot be greater than latestVersion (${version}).`
    );
  }

  const mandatory = Boolean(options.mandatory);
  const publishedAt = validatePublishedDate(
    options.publishedAt || new Date().toISOString().slice(0, 10)
  );

  const dlBaseRaw = (options.dlBaseUrl || brand.defaultDlBaseUrl).replace(/\/+$/, '');
  validateAllowedHttpsUrl(dlBaseRaw, brand.allowedHosts, 'dlBaseUrl');
  const allDownloadsUrl = validateAllowedHttpsUrl(
    options.allDownloadsUrl || `${dlBaseRaw}/releases/`,
    brand.allowedHosts,
    'allDownloadsUrl'
  );

  const requiredPlatforms = options.requiredPlatforms ?? ['windows', 'macos', 'linux'];
  for (const reqPlat of requiredPlatforms) {
    if (!Object.prototype.hasOwnProperty.call(PLATFORM_LABELS, reqPlat)) {
      throw new Error(`Unknown required platform "${reqPlat}".`);
    }
  }

  // Resolve and validate CHANGELOG / release notes
  const changelogPath = path.resolve(options.changelogPath || path.join(repoRoot, 'CHANGELOG'));
  let changelogEntry = null;
  if (fs.existsSync(changelogPath)) {
    const entries = parseChangelog(fs.readFileSync(changelogPath, 'utf8'));
    changelogEntry = entries.find((e) => e.version === version) || null;
    if (!changelogEntry && version.includes('-')) {
      const baseVersion = version.slice(0, version.indexOf('-'));
      changelogEntry = entries.find((e) => e.version === baseVersion) || null;
    }
  }
  if (!changelogEntry && !options.allowUnlistedChangelog) {
    throw new Error(
      `Version ${version} was not found in ${changelogPath}. Add the ${brand.productName} ${version} section to CHANGELOG before cutting a release.`
    );
  }

  let rawNotes = options.releaseNotes;
  if (!rawNotes) {
    const enSummary = options.notesEn
      ? options.notesEn
      : changelogEntry
        ? summarizeChangelogRelease(changelogEntry)
        : '';
    if (!enSummary) {
      throw new Error('Could not build releaseNotes.en (no CHANGELOG entry or --notes-en provided).');
    }
    rawNotes = { en: enSummary };
    if (options.notesTr) {
      rawNotes.tr = options.notesTr;
    }
  }
  const releaseNotes = validateReleaseNotes(rawNotes);

  // Safely scan distDir
  const distDir = path.resolve(options.distDir || path.join(repoRoot, 'dist'));
  if (!fs.existsSync(distDir)) {
    throw new Error(`dist directory does not exist: ${distDir}`);
  }
  const distLstat = fs.lstatSync(distDir);
  if (distLstat.isSymbolicLink() || !distLstat.isDirectory()) {
    throw new Error(`dist path must be a regular directory (not a symlink): ${distDir}`);
  }

  const dirEntries = fs.readdirSync(distDir).sort();
  const artifacts = [];
  const expectedSbomName = `${brand.productId}-${version}-sbom.spdx.json`;
  let sbomEntry = null;

  for (const entryName of dirEntries) {
    if (path.basename(entryName) !== entryName || entryName.includes('..')) {
      throw new Error(`Invalid directory entry in dist/: "${entryName}"`);
    }
    const fullPath = path.resolve(distDir, entryName);
    if (!fullPath.startsWith(`${distDir}${path.sep}`)) {
      throw new Error(`Path traversal refused in dist/: "${entryName}"`);
    }

    const st = fs.lstatSync(fullPath);
    if (st.isSymbolicLink()) {
      throw new Error(`Refusing symbolic link inside dist/: "${entryName}"`);
    }
    if (IGNORED_DIST_FILES.has(entryName)) {
      continue;
    }
    if (st.isDirectory()) {
      // Allow local staging directories matching <productId>-<version>-<plat> and dist/wasm/, reject foreign dirs.
      if (entryName.startsWith(`${brand.productId}-${version}-`) || entryName === 'wasm') {
        continue;
      }
      throw new Error(`Unexpected directory inside dist/: "${entryName}"`);
    }
    if (!st.isFile()) {
      throw new Error(`Refusing non-regular file inside dist/: "${entryName}"`);
    }

    if (entryName.endsWith('.spdx.json')) {
      if (entryName !== expectedSbomName) {
        throw new Error(
          `SBOM "${entryName}" does not match target release "${expectedSbomName}" (remove stale files from dist/).`
        );
      }
      if (st.size <= 0) {
        throw new Error(`SBOM "${entryName}" is empty (0 bytes).`);
      }
      let parsedSbom;
      try {
        parsedSbom = JSON.parse(fs.readFileSync(fullPath, 'utf8'));
      } catch (err) {
        throw new Error(`SBOM "${entryName}" is not valid UTF-8 JSON: ${err.message}`);
      }
      if (!parsedSbom || parsedSbom.spdxVersion !== 'SPDX-2.3' || parsedSbom.SPDXID !== 'SPDXRef-DOCUMENT') {
        throw new Error(`SBOM "${entryName}" is not a valid SPDX 2.3 JSON document.`);
      }
      const sbomSha256 = computeFileSha256(fullPath);
      const sbomUrl = validateAllowedHttpsUrl(
        `${dlBaseRaw}/releases/${encodeURIComponent(version)}/${encodeURIComponent(entryName)}`,
        brand.allowedHosts,
        `SBOM URL (${entryName})`
      );
      sbomEntry = {
        name: entryName,
        format: 'spdx-json',
        spdxVersion: 'SPDX-2.3',
        url: sbomUrl,
        sha256: sbomSha256,
        size: st.size,
      };
      continue;
    }

    if (!entryName.startsWith(`${brand.productId}-`) && !hasAnyPackageSuffix(entryName)) {
      throw new Error(`Unexpected unrecognized file inside dist/: "${entryName}"`);
    }

    const meta = parseArtifactFilename(entryName, brand.productId, version);
    if (st.size <= 0) {
      throw new Error(`Artifact "${entryName}" is empty (0 bytes).`);
    }

    const sha256 = computeFileSha256(fullPath);
    const artifactUrl = validateAllowedHttpsUrl(
      `${dlBaseRaw}/releases/${encodeURIComponent(version)}/${encodeURIComponent(entryName)}`,
      brand.allowedHosts,
      `artifact URL (${entryName})`
    );

    artifacts.push({
      name: meta.name,
      platform: meta.platform,
      arch: meta.arch,
      format: meta.format,
      primary: meta.primary,
      url: artifactUrl,
      sha256,
      size: st.size,
    });
  }

  if (artifacts.length === 0) {
    throw new Error(`No release artifacts for ${brand.productId} ${version} found in ${distDir}.`);
  }

  // Build platform primary downloads
  const downloads = {};
  for (const plat of ['windows', 'macos', 'linux']) {
    const primaryCandidates = artifacts.filter((a) => a.platform === plat && a.primary);
    if (primaryCandidates.length > 1) {
      // Prefer canonical architecture when multiple architectures exist.
      const preferredArch = plat === 'macos' ? 'arm64' : 'x64';
      const chosen = primaryCandidates.find((a) => a.arch === preferredArch) || primaryCandidates[0];
      downloads[plat] = {
        url: chosen.url,
        label: PLATFORM_LABELS[plat],
        sha256: chosen.sha256,
        size: chosen.size,
      };
    } else if (primaryCandidates.length === 1) {
      const chosen = primaryCandidates[0];
      downloads[plat] = {
        url: chosen.url,
        label: PLATFORM_LABELS[plat],
        sha256: chosen.sha256,
        size: chosen.size,
      };
    }
  }

  if (options.androidStoreUrl) {
    const storeUrl = validateAllowedHttpsUrl(options.androidStoreUrl, brand.allowedHosts, 'androidStoreUrl');
    downloads.android = {
      url: storeUrl,
      label: new URL(storeUrl).hostname === 'play.google.com' ? 'Google Play' : 'Android',
    };
  } else {
    const androidPrimary = artifacts.find((a) => a.platform === 'android' && a.primary);
    if (androidPrimary) {
      downloads.android = {
        url: androidPrimary.url,
        label: 'Android',
        sha256: androidPrimary.sha256,
        size: androidPrimary.size,
      };
    }
  }

  if (options.iosStoreUrl) {
    const storeUrl = validateAllowedHttpsUrl(options.iosStoreUrl, brand.allowedHosts, 'iosStoreUrl');
    downloads.ios = {
      url: storeUrl,
      label: 'App Store',
    };
  }

  for (const reqPlat of requiredPlatforms) {
    if (!downloads[reqPlat]) {
      throw new Error(
        `Missing required primary release download for platform "${reqPlat}" (expected Windows -setup.exe, macOS .dmg, Linux .AppImage).`
      );
    }
  }

  const manifest = {
    schemaVersion: 1,
    latestVersion: version,
    minSupportedVersion,
    mandatory,
    publishedAt,
    allDownloadsUrl,
    releaseNotes,
    downloads,
    artifacts,
  };
  if (sbomEntry) {
    manifest.sbom = sbomEntry;
  }

  validateManifestObject(manifest, brand.allowedHosts);

  const checksumEntries = sbomEntry
    ? [...artifacts, sbomEntry].sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0))
    : artifacts;
  const checksumsText = `${checksumEntries.map((a) => `${a.sha256}  ${a.name}`).join('\n')}\n`;
  const manifestJson = `${JSON.stringify(manifest, null, 2)}\n`;
  const markdownNotes = formatMarkdownReleaseNotes(brand.productName, version, changelogEntry, artifacts, sbomEntry);

function writeRegularFileAtomic(targetPath, content) {
  const resolved = path.resolve(targetPath);
  const parentDir = path.dirname(resolved);
  const parentStat = fs.lstatSync(parentDir);
  if (parentStat.isSymbolicLink() || !parentStat.isDirectory()) {
    throw new Error(`Refusing to write into symlink or non-directory: ${parentDir}`);
  }
  try {
    const existing = fs.lstatSync(resolved);
    if (existing.isSymbolicLink() || !existing.isFile()) {
      throw new Error(`Refusing to overwrite symlink or non-regular file: ${resolved}`);
    }
  } catch (err) {
    if (!err || err.code !== 'ENOENT') {
      throw err;
    }
  }
  const tmpPath = `${resolved}.tmp.${process.pid}.${crypto.randomBytes(4).toString('hex')}`;
  fs.writeFileSync(tmpPath, content, { encoding: 'utf8', mode: 0o644, flag: 'wx' });
  fs.renameSync(tmpPath, resolved);
}

  if (options.writeFiles !== false) {
    const outManifest = path.resolve(options.outputPath || path.join(distDir, 'releases.json'));
    const outChecksums = path.resolve(options.checksumsPath || path.join(distDir, 'SHA256SUMS.txt'));
    writeRegularFileAtomic(outManifest, manifestJson);
    writeRegularFileAtomic(outChecksums, checksumsText);
    if (options.markdownNotesPath) {
      writeRegularFileAtomic(path.resolve(options.markdownNotesPath), markdownNotes);
    }
  }

  return {
    brand,
    manifest,
    manifestJson,
    checksumsText,
    markdownNotes,
  };
}

/**
 * Strictly validates a parsed `releases.json` object against schema & security invariants.
 */
export function validateManifestObject(manifest, allowedHosts) {
  if (!manifest || typeof manifest !== 'object' || Array.isArray(manifest)) {
    throw new Error('Manifest must be a JSON object.');
  }
  const rawJsonSize = Buffer.byteLength(JSON.stringify(manifest), 'utf8');
  if (rawJsonSize > 65536) {
    throw new Error(`Manifest JSON size (${rawJsonSize} bytes) exceeds 65536-byte client limit.`);
  }
  if (manifest.schemaVersion !== undefined && manifest.schemaVersion !== 1) {
    throw new Error(`Unsupported manifest schemaVersion "${manifest.schemaVersion}" (expected 1).`);
  }

  const latestVersion = normalizeAndValidateVersion(manifest.latestVersion, 'latestVersion');
  if (manifest.minSupportedVersion) {
    const minVer = normalizeAndValidateVersion(manifest.minSupportedVersion, 'minSupportedVersion');
    if (compareSemVer(minVer, latestVersion) > 0) {
      throw new Error(`minSupportedVersion (${minVer}) > latestVersion (${latestVersion}).`);
    }
  }
  if (typeof manifest.mandatory !== 'boolean') {
    throw new Error('Manifest "mandatory" field must be a boolean.');
  }
  validatePublishedDate(manifest.publishedAt);
  validateAllowedHttpsUrl(manifest.allDownloadsUrl, allowedHosts, 'allDownloadsUrl');
  validateReleaseNotes(manifest.releaseNotes);

  if (!manifest.downloads || typeof manifest.downloads !== 'object' || Array.isArray(manifest.downloads)) {
    throw new Error('Manifest "downloads" must be an object.');
  }
  for (const [plat, entry] of Object.entries(manifest.downloads)) {
    if (!Object.prototype.hasOwnProperty.call(PLATFORM_LABELS, plat)) {
      throw new Error(`Unknown platform "${plat}" in manifest.downloads.`);
    }
    if (!entry || typeof entry !== 'object' || Array.isArray(entry)) {
      throw new Error(`manifest.downloads.${plat} must be an object.`);
    }
    validateAllowedHttpsUrl(entry.url, allowedHosts, `downloads.${plat}.url`);
    if (typeof entry.label !== 'string' || entry.label.trim().length === 0 || entry.label.trim().length > 40) {
      throw new Error(`manifest.downloads.${plat}.label must be 1..40 characters.`);
    }
    if (entry.sha256 !== undefined && !SHA256_HEX_RE.test(entry.sha256)) {
      throw new Error(`Invalid sha256 in manifest.downloads.${plat}.`);
    }
    if (entry.size !== undefined && (!Number.isSafeInteger(entry.size) || entry.size <= 0)) {
      throw new Error(`Invalid size in manifest.downloads.${plat}.`);
    }
  }

  if (!Array.isArray(manifest.artifacts) || manifest.artifacts.length === 0) {
    throw new Error('Manifest "artifacts" must be a non-empty array.');
  }
  const seenNames = new Set();
  for (const art of manifest.artifacts) {
    if (!art || typeof art !== 'object' || Array.isArray(art)) {
      throw new Error('Each manifest artifact entry must be an object.');
    }
    if (typeof art.name !== 'string' || art.name.includes('/') || art.name.includes('\\') || art.name.includes('..')) {
      throw new Error(`Invalid artifact name "${art.name}".`);
    }
    if (seenNames.has(art.name)) {
      throw new Error(`Duplicate artifact "${art.name}" in manifest.artifacts.`);
    }
    seenNames.add(art.name);
    validateAllowedHttpsUrl(art.url, allowedHosts, `artifacts[${art.name}].url`);
    if (!SHA256_HEX_RE.test(art.sha256)) {
      throw new Error(`Invalid sha256 for artifact "${art.name}".`);
    }
    if (!Number.isSafeInteger(art.size) || art.size <= 0) {
      throw new Error(`Invalid size for artifact "${art.name}".`);
    }
  }

  if (manifest.sbom !== undefined) {
    const sbom = manifest.sbom;
    if (!sbom || typeof sbom !== 'object' || Array.isArray(sbom)) {
      throw new Error('Manifest "sbom" must be an object when present.');
    }
    if (
      typeof sbom.name !== 'string' ||
      !sbom.name.endsWith('-sbom.spdx.json') ||
      sbom.name.includes('/') ||
      sbom.name.includes('\\') ||
      sbom.name.includes('..') ||
      seenNames.has(sbom.name)
    ) {
      throw new Error(`Invalid or duplicate sbom.name "${sbom.name}".`);
    }
    if (sbom.format !== 'spdx-json' || sbom.spdxVersion !== 'SPDX-2.3') {
      throw new Error(`Unsupported sbom format/version (${sbom.format} / ${sbom.spdxVersion}).`);
    }
    validateAllowedHttpsUrl(sbom.url, allowedHosts, `sbom[${sbom.name}].url`);
    if (!SHA256_HEX_RE.test(sbom.sha256)) {
      throw new Error(`Invalid sha256 for sbom "${sbom.name}".`);
    }
    if (!Number.isSafeInteger(sbom.size) || sbom.size <= 0) {
      throw new Error(`Invalid size for sbom "${sbom.name}".`);
    }
  }

  return true;
}

/**
 * Verifies an existing `dist/releases.json` and `dist/SHA256SUMS.txt` against actual disk artifacts.
 */
export function verifyReleaseDirectory(options = {}) {
  const repoRoot = path.resolve(options.repoRoot || DEFAULT_REPO_ROOT);
  const brand = readBrandDefaults(repoRoot);
  const distDir = path.resolve(options.distDir || path.join(repoRoot, 'dist'));
  const manifestPath = path.resolve(options.outputPath || path.join(distDir, 'releases.json'));
  const checksumsPath = path.resolve(options.checksumsPath || path.join(distDir, 'SHA256SUMS.txt'));

  for (const p of [manifestPath, checksumsPath]) {
    const st = fs.lstatSync(p);
    if (!st.isFile() || st.isSymbolicLink() || st.size <= 0) {
      throw new Error(`Missing, empty, or symlink metadata file: ${p}`);
    }
  }

  const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
  validateManifestObject(manifest, brand.allowedHosts);

  if (options.expectedVersion) {
    const expected = normalizeAndValidateVersion(options.expectedVersion, 'expectedVersion');
    if (manifest.latestVersion !== expected) {
      throw new Error(
        `releases.json latestVersion "${manifest.latestVersion}" does not match expected "${expected}".`
      );
    }
  }

  const expectedSbomName = `${brand.productId}-${manifest.latestVersion}-sbom.spdx.json`;
  const onDiskSbomPath = path.join(distDir, expectedSbomName);
  if (!manifest.sbom && fs.existsSync(onDiskSbomPath)) {
    throw new Error(
      `Unlisted SBOM file "${expectedSbomName}" exists in dist/ without a corresponding manifest.sbom entry.`
    );
  }

  const checksumLines = fs
    .readFileSync(checksumsPath, 'utf8')
    .replace(/\r\n/g, '\n')
    .split('\n')
    .filter((l) => l.length > 0);

  const expectedChecksumCount = manifest.artifacts.length + (manifest.sbom ? 1 : 0);
  if (checksumLines.length !== expectedChecksumCount) {
    throw new Error(
      `SHA256SUMS.txt entry count (${checksumLines.length}) does not match expected artifact + SBOM count (${expectedChecksumCount}).`
    );
  }

  const checksumMap = new Map();
  for (const line of checksumLines) {
    const m = /^([0-9a-f]{64})  ([A-Za-z0-9][A-Za-z0-9._+-]{0,127})$/.exec(line);
    if (!m) {
      throw new Error(`Malformed line in SHA256SUMS.txt: "${line}"`);
    }
    const [, hash, filename] = m;
    if (checksumMap.has(filename)) {
      throw new Error(`Duplicate filename "${filename}" in SHA256SUMS.txt.`);
    }
    checksumMap.set(filename, hash);
  }

  for (const art of manifest.artifacts) {
    parseArtifactFilename(art.name, brand.productId, manifest.latestVersion);
    const sumHash = checksumMap.get(art.name);
    if (!sumHash || sumHash !== art.sha256) {
      throw new Error(`SHA-256 mismatch between releases.json and SHA256SUMS.txt for "${art.name}".`);
    }

    const filePath = path.resolve(distDir, art.name);
    if (!filePath.startsWith(`${distDir}${path.sep}`)) {
      throw new Error(`Path traversal detected for artifact "${art.name}".`);
    }
    const st = fs.lstatSync(filePath);
    if (!st.isFile() || st.isSymbolicLink()) {
      throw new Error(`Artifact "${art.name}" is missing or is a symbolic link.`);
    }
    if (st.size !== art.size) {
      throw new Error(`Artifact "${art.name}" byte size mismatch: disk=${st.size}, manifest=${art.size}.`);
    }
    const actualSha = computeFileSha256(filePath);
    if (actualSha !== art.sha256) {
      throw new Error(`Artifact "${art.name}" SHA-256 digest mismatch: disk=${actualSha}, manifest=${art.sha256}.`);
    }
  }

  if (manifest.sbom) {
    if (manifest.sbom.name !== expectedSbomName) {
      throw new Error(
        `Manifest sbom.name "${manifest.sbom.name}" does not match expected "${expectedSbomName}".`
      );
    }
    const sumHash = checksumMap.get(manifest.sbom.name);
    if (!sumHash || sumHash !== manifest.sbom.sha256) {
      throw new Error(
        `SHA-256 mismatch between releases.json and SHA256SUMS.txt for SBOM "${manifest.sbom.name}".`
      );
    }
    const sbomPath = path.resolve(distDir, manifest.sbom.name);
    if (!sbomPath.startsWith(`${distDir}${path.sep}`)) {
      throw new Error(`Path traversal detected for SBOM "${manifest.sbom.name}".`);
    }
    const st = fs.lstatSync(sbomPath);
    if (!st.isFile() || st.isSymbolicLink()) {
      throw new Error(`SBOM "${manifest.sbom.name}" is missing or is a symbolic link.`);
    }
    if (st.size !== manifest.sbom.size) {
      throw new Error(`SBOM "${manifest.sbom.name}" byte size mismatch: disk=${st.size}, manifest=${manifest.sbom.size}.`);
    }
    const actualSha = computeFileSha256(sbomPath);
    if (actualSha !== manifest.sbom.sha256) {
      throw new Error(`SBOM "${manifest.sbom.name}" SHA-256 digest mismatch: disk=${actualSha}, manifest=${manifest.sbom.sha256}.`);
    }
    const parsedSbom = JSON.parse(fs.readFileSync(sbomPath, 'utf8'));
    if (!parsedSbom || parsedSbom.spdxVersion !== 'SPDX-2.3' || parsedSbom.SPDXID !== 'SPDXRef-DOCUMENT') {
      throw new Error(`SBOM "${manifest.sbom.name}" on disk is not a valid SPDX 2.3 JSON document.`);
    }
  }

  // Also ensure primary download sha256/size match their corresponding artifact when hosted on dlHost.
  for (const [plat, dl] of Object.entries(manifest.downloads)) {
    const parsed = new URL(dl.url);
    if (parsed.hostname === brand.dlHost) {
      const base = path.posix.basename(parsed.pathname);
      const matchingArt = manifest.artifacts.find((a) => a.name === base);
      if (!matchingArt) {
        throw new Error(`Primary download for "${plat}" points to unlisted artifact "${base}".`);
      }
      if (dl.sha256 !== matchingArt.sha256 || dl.size !== matchingArt.size) {
        throw new Error(`Primary download sha256/size for "${plat}" diverges from artifact "${base}".`);
      }
    }
  }

  return { brand, manifest };
}

function parseArgs(argv) {
  const opts = {};
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    const next = () => {
      if (i + 1 >= argv.length) {
        throw new Error(`Option ${arg} requires a value.`);
      }
      i += 1;
      return argv[i];
    };
    switch (arg) {
      case '--version':
        opts.version = next();
        break;
      case '--dist':
        opts.distDir = next();
        break;
      case '--output':
        opts.outputPath = next();
        break;
      case '--checksums-output':
        opts.checksumsPath = next();
        break;
      case '--notes-markdown-out':
        opts.markdownNotesPath = next();
        break;
      case '--min-supported-version':
        opts.minSupportedVersion = next();
        break;
      case '--published-at':
        opts.publishedAt = next();
        break;
      case '--dl-base-url':
        opts.dlBaseUrl = next();
        break;
      case '--all-downloads-url':
        opts.allDownloadsUrl = next();
        break;
      case '--android-store-url':
        opts.androidStoreUrl = next();
        break;
      case '--ios-store-url':
        opts.iosStoreUrl = next();
        break;
      case '--changelog':
        opts.changelogPath = next();
        break;
      case '--notes-en':
        opts.notesEn = next();
        break;
      case '--notes-tr':
        opts.notesTr = next();
        break;
      case '--notes-json':
        opts.releaseNotes = JSON.parse(next());
        break;
      case '--require-platforms': {
        const val = next().trim();
        opts.requiredPlatforms = val ? val.split(',').map((s) => s.trim()).filter(Boolean) : [];
        break;
      }
      case '--mandatory':
        opts.mandatory = true;
        break;
      case '--allow-unlisted-changelog':
        opts.allowUnlistedChangelog = true;
        break;
      case '--verify-only':
        opts.verifyOnly = true;
        break;
      case '--help':
      case '-h':
        opts.help = true;
        break;
      default:
        throw new Error(`Unknown argument "${arg}". Run with --help for usage.`);
    }
  }
  return opts;
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  try {
    const cli = parseArgs(process.argv.slice(2));
    if (cli.help) {
      process.stdout.write(
        [
          'Usage: node ci/generate-releases-json.mjs [options]',
          '',
          'Options:',
          '  --version <ver>                Release version or v-tag (defaults to CMakeLists.txt)',
          '  --dist <dir>                   Artifact directory (default: ./dist)',
          '  --output <file>                Manifest output path (default: <dist>/releases.json)',
          '  --checksums-output <file>      Checksums output path (default: <dist>/SHA256SUMS.txt)',
          '  --notes-markdown-out <file>    Optional Markdown release notes output file',
          '  --min-supported-version <ver>  Minimum non-mandatory client version (default: 1.8.0)',
          '  --mandatory                    Mark this release as mandatory',
          '  --published-at <YYYY-MM-DD>    Release date in UTC (default: today UTC)',
          '  --dl-base-url <https://...>    Download CDN origin (default: https://dl.<RAGO_DOMAIN>)',
          '  --android-store-url <url>      Optional Google Play listing URL',
          '  --ios-store-url <url>          Optional Apple App Store listing URL',
          '  --notes-en <text>              Override English release summary',
          '  --notes-tr <text>              Optional Turkish release summary',
          '  --require-platforms <csv>      Required primary platforms (default: windows,macos,linux)',
          '  --verify-only                  Verify existing releases.json & SHA256SUMS.txt against dist/',
          '',
        ].join('\n')
      );
      process.exit(0);
    }

    if (cli.verifyOnly) {
      const res = verifyReleaseDirectory({
        distDir: cli.distDir,
        outputPath: cli.outputPath,
        checksumsPath: cli.checksumsPath,
        expectedVersion: cli.version,
      });
      process.stdout.write(
        `==> verified releases.json & SHA256SUMS.txt (${res.brand.productName} ${res.manifest.latestVersion}, ${res.manifest.artifacts.length} artifact(s))\n`
      );
    } else {
      const res = generateReleaseManifest({
        ...cli,
        androidStoreUrl: cli.androidStoreUrl || process.env.RAGO_ANDROID_STORE_URL || '',
        iosStoreUrl: cli.iosStoreUrl || process.env.RAGO_IOS_STORE_URL || '',
        notesTr: cli.notesTr || process.env.RAGO_RELEASE_NOTES_TR || '',
      });
      process.stdout.write(
        `==> generated releases.json & SHA256SUMS.txt (${res.brand.productName} ${res.manifest.latestVersion}, ${res.manifest.artifacts.length} artifact(s))\n`
      );
    }
  } catch (err) {
    process.stderr.write(`error: ${err instanceof Error ? err.message : String(err)}\n`);
    process.exit(1);
  }
}
