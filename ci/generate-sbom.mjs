#!/usr/bin/env node
/**
 * Zero-dependency deterministic SPDX 2.3 JSON SBOM generator & license bundle
 * verifier for RagoShell (Roadmap Phase 3.7 — A5 & E6, docs/ReleasePipeline.md).
 *
 * Security & single-source guarantees:
 * - Reads product identity, domain, and URLs strictly from `cmake/RagoShellDefaults.cmake`
 *   and dependency manifests from `vcpkg.json` + `CMakeLists.txt`.
 * - NEVER reads, sources, or prints `ci/.env`.
 * - Enforces path boundary checks (`path.resolve`), regular-file checks (`fs.lstatSync`),
 *   symlink refusal, non-empty file checks, and atomic writes (`wx` temp file + rename).
 * - Produces deterministic SPDX 2.3 JSON (`dist/<product>-<version>-sbom.spdx.json`) with
 *   Package URLs (`purl`), CPE 2.3 (`cpe23Type`) identifiers, SPDX license expressions,
 *   and SHA-256 digests of built `dist/` release artifacts when present.
 */

import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

import {
  computeFileSha256,
  normalizeAndValidateVersion,
  readBrandDefaults,
  readDefaultCmakeVersion,
  validateAllowedHttpsUrl,
} from './generate-releases-json.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const DEFAULT_REPO_ROOT = path.resolve(__dirname, '..');

const SPDX_ID_RE = /^SPDXRef-[A-Za-z0-9.-]+$/;
const ISO_UTC_SECONDS_RE = /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$/;
const SHA256_HEX_RE = /^[0-9a-f]{64}$/;
const SHA512_HEX_RE = /^[0-9a-f]{128}$/;
const PURL_RE = /^pkg:[a-z0-9+.-]+\/.+/i;
const CPE23_RE = /^cpe:2\.3:[aho]:[^:\s]+:[^:\s]+:[^:\s]+:/;

// RFC 4122 URL namespace UUID (6ba7b811-9dad-11d1-80b4-00c04fd430c8) for deterministic UUIDv5.
const UUID_URL_NAMESPACE_BYTES = Buffer.from('6ba7b8119dad11d180b400c04fd430c8', 'hex');

const RELEASE_PACKAGE_SUFFIX_RE = /^(windows|macos|linux|android)-(x64|arm64|universal)(-setup\.exe|\.msi|\.zip|\.dmg|\.tar\.gz|\.AppImage|\.apk|\.aab)$/;

const IGNORED_DIST_FOR_SBOM = new Set([
  '.DS_Store',
  'releases.json',
  'releases.json.minisig',
  'SHA256SUMS.txt',
  'SHA256SUMS.txt.minisig',
  'RELEASE_NOTES.md',
]);

export const ROOT_LICENSE_FILES = Object.freeze([
  'LICENSE',
  'LICENSE.LGPLv3',
  'COPYRIGHT',
  'THIRD-PARTY-NOTICES.txt',
]);

export const EMBEDDED_THIRD_PARTY_LICENSE_MAP = Object.freeze([
  { relSource: 'src/third-party/qscintilla-2.8.4/sources/LICENSE.GPL3', installedName: 'qscintilla-LICENSE.GPL3' },
  { relSource: 'src/third-party/qscintilla-2.8.4/sources/OPENSOURCE-NOTICE.TXT', installedName: 'qscintilla-OPENSOURCE-NOTICE.txt' },
  { relSource: 'src/third-party/qjson-0.8.1/sources/README.license', installedName: 'qjson-LICENSE.txt' },
  { relSource: 'src/third-party/googletest-1.8.1/LICENSE', installedName: 'googletest-LICENSE.txt' },
  { relSource: 'assets/fonts/DEJAVU-LICENSE', installedName: 'DejaVuSans-LICENSE.txt' },
]);

/**
 * Curated upstream metadata for dependencies resolved by our pinned `vcpkg.json`
 * (`builtin-baseline` 4f973280dd95e3902b3410933fb2ce108bb29d1d + `overrides`).
 * When `vcpkg_installed/<triplet>/share/<port>/vcpkg.spdx.json` is present, exact built port versions,
 * upstream download locations, and archive/tree SHA-512 digests overlay these defaults.
 */
const VCPKG_PORT_CATALOG = Object.freeze({
  'mongo-cxx-driver': {
    defaultVersion: '4.0.0',
    license: 'Apache-2.0',
    supplier: 'Organization: MongoDB, Inc.',
    homepage: 'https://github.com/mongodb/mongo-cxx-driver',
    downloadTemplate: (v) => `git+https://github.com/mongodb/mongo-cxx-driver@r${v}`,
    githubPurl: (v) => `pkg:github/mongodb/mongo-cxx-driver@r${v}`,
    cpeVendor: 'mongodb',
    cpeProduct: 'mongo-cxx-driver',
    copyrightText: 'Copyright 2009-present MongoDB, Inc.',
    dependsOn: ['mongo-c-driver', 'libbson'],
  },
  'mongo-c-driver': {
    defaultVersion: '1.30.7',
    license: 'Apache-2.0',
    supplier: 'Organization: MongoDB, Inc.',
    homepage: 'https://github.com/mongodb/mongo-c-driver',
    downloadTemplate: (v) => `git+https://github.com/mongodb/mongo-c-driver@${v}`,
    githubPurl: (v) => `pkg:github/mongodb/mongo-c-driver@${v}`,
    cpeVendor: 'mongodb',
    cpeProduct: 'c_driver',
    copyrightText: 'Copyright 2009-present MongoDB, Inc.',
    dependsOn: ['libbson', 'openssl', 'zlib', 'utf8proc'],
  },
  libbson: {
    defaultVersion: '1.30.7',
    license: 'Apache-2.0',
    supplier: 'Organization: MongoDB, Inc.',
    homepage: 'https://github.com/mongodb/mongo-c-driver/tree/master/src/libbson',
    downloadTemplate: (v) => `git+https://github.com/mongodb/mongo-c-driver@${v}`,
    githubPurl: (v) => `pkg:github/mongodb/mongo-c-driver@${v}#src/libbson`,
    cpeVendor: 'mongodb',
    cpeProduct: 'libbson',
    copyrightText: 'Copyright 2009-present MongoDB, Inc.',
    dependsOn: [],
  },
  openssl: {
    defaultVersion: '3.6.5',
    license: 'Apache-2.0',
    supplier: 'Organization: The OpenSSL Project',
    homepage: 'https://www.openssl.org/',
    downloadTemplate: (v) => `git+https://github.com/openssl/openssl@openssl-${v}`,
    githubPurl: (v) => `pkg:github/openssl/openssl@openssl-${v}`,
    cpeVendor: 'openssl',
    cpeProduct: 'openssl',
    copyrightText: 'Copyright (c) 1998-2026 The OpenSSL Project Authors',
    dependsOn: [],
  },
  libssh2: {
    defaultVersion: '1.11.1',
    license: 'BSD-3-Clause',
    supplier: 'Organization: The libssh2 Project',
    homepage: 'https://www.libssh2.org/',
    downloadTemplate: (v) => `git+https://github.com/libssh2/libssh2@libssh2-${v}`,
    githubPurl: (v) => `pkg:github/libssh2/libssh2@libssh2-${v}`,
    cpeVendor: 'libssh2',
    cpeProduct: 'libssh2',
    copyrightText: 'Copyright (c) 2004-2024 The libssh2 Project and its contributors',
    dependsOn: ['openssl', 'zlib'],
  },
  zlib: {
    defaultVersion: '1.3.2',
    license: 'Zlib',
    supplier: 'Person: Jean-loup Gailly and Mark Adler',
    homepage: 'https://zlib.net/',
    downloadTemplate: (v) => `git+https://github.com/madler/zlib@v${v}`,
    githubPurl: (v) => `pkg:github/madler/zlib@v${v}`,
    cpeVendor: 'zlib',
    cpeProduct: 'zlib',
    copyrightText: 'Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler',
    dependsOn: [],
  },
  utf8proc: {
    defaultVersion: '2.12.0',
    license: 'MIT',
    supplier: 'Organization: JuliaStrings',
    homepage: 'https://github.com/JuliaLang/utf8proc',
    downloadTemplate: (v) => `git+https://github.com/JuliaLang/utf8proc@v${v}`,
    githubPurl: (v) => `pkg:github/JuliaLang/utf8proc@v${v}`,
    cpeVendor: 'julialang',
    cpeProduct: 'utf8proc',
    copyrightText: 'Copyright (c) 2014-2024 by Steven G. Johnson, Jiahao Chen, Tony Kelman, Jonas Fonseca, and other contributors',
    dependsOn: [],
  },
});

const SCINTILLA_EXTRACTED_LICENSE = Object.freeze({
  licenseId: 'LicenseRef-Scintilla',
  name: 'Scintilla Historical Permission Notice',
  extractedText:
    'Copyright 1998-2014 by Neil Hodgson <neilh@scintilla.org>. All Rights Reserved. ' +
    'Permission to use, copy, modify, and distribute this software and its documentation for any purpose ' +
    'and without fee is hereby granted, provided that the above copyright notice appear in all copies and ' +
    'that both that copyright notice and this permission notice appear in supporting documentation.',
});

function sanitizeSpdxToken(raw) {
  return String(raw)
    .replace(/[^A-Za-z0-9.-]+/g, '-')
    .replace(/^-+|-+$/g, '');
}

function makeUuidV5FromName(name) {
  const hash = crypto
    .createHash('sha1')
    .update(UUID_URL_NAMESPACE_BYTES)
    .update(Buffer.from(String(name), 'utf8'))
    .digest();
  const bytes = Buffer.from(hash.subarray(0, 16));
  bytes[6] = (bytes[6] & 0x0f) | 0x50; // version 5
  bytes[8] = (bytes[8] & 0x3f) | 0x80; // RFC 4122 variant
  const hex = bytes.toString('hex');
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20, 32)}`;
}

function escapeCpeValue(val) {
  return String(val).replace(/[^A-Za-z0-9._-]/g, '_').toLowerCase();
}

function buildCpe23(vendor, product, version) {
  return `cpe:2.3:a:${escapeCpeValue(vendor)}:${escapeCpeValue(product)}:${escapeCpeValue(version)}:*:*:*:*:*:*:*`;
}

function resolveCreatedTimestamp(explicitCreated) {
  if (explicitCreated !== undefined && explicitCreated !== '') {
    const trimmed = String(explicitCreated).trim();
    if (!ISO_UTC_SECONDS_RE.test(trimmed) || Number.isNaN(Date.parse(trimmed))) {
      throw new Error(`Invalid --created timestamp "${explicitCreated}": expected YYYY-MM-DDTHH:MM:SSZ.`);
    }
    return trimmed;
  }
  if (process.env.SOURCE_DATE_EPOCH !== undefined && process.env.SOURCE_DATE_EPOCH !== '') {
    const rawEpoch = process.env.SOURCE_DATE_EPOCH.trim();
    if (!/^\d{1,12}$/.test(rawEpoch)) {
      throw new Error(`Invalid SOURCE_DATE_EPOCH "${rawEpoch}": expected non-negative integer seconds.`);
    }
    const d = new Date(Number.parseInt(rawEpoch, 10) * 1000);
    if (Number.isNaN(d.getTime())) {
      throw new Error(`Out-of-range SOURCE_DATE_EPOCH "${rawEpoch}".`);
    }
    return d.toISOString().replace(/\.\d{3}Z$/, 'Z');
  }
  return new Date().toISOString().replace(/\.\d{3}Z$/, 'Z');
}

function assertNonEmptyRegularFile(filePath, label) {
  const resolved = path.resolve(filePath);
  let st;
  try {
    st = fs.lstatSync(resolved);
  } catch {
    throw new Error(`Missing required file (${label}): ${resolved}`);
  }
  if (st.isSymbolicLink() || !st.isFile()) {
    throw new Error(`Refusing symbolic link or non-regular file (${label}): ${resolved}`);
  }
  if (st.size <= 0) {
    throw new Error(`Required file is empty (0 bytes) (${label}): ${resolved}`);
  }
  return st;
}

function writeRegularFileAtomic(targetPath, content) {
  const resolved = path.resolve(targetPath);
  const parentDir = path.dirname(resolved);
  fs.mkdirSync(parentDir, { recursive: true });
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

/**
 * Verifies source-tree license & GPLv3 §6 / LGPLv3 §4 notice files before build/packaging.
 */
export function verifySourceLicenseNotices(repoRoot = DEFAULT_REPO_ROOT) {
  const root = path.resolve(repoRoot);
  const brand = readBrandDefaults(root);

  for (const rel of ROOT_LICENSE_FILES) {
    assertNonEmptyRegularFile(path.join(root, rel), `root license ${rel}`);
  }
  for (const item of EMBEDDED_THIRD_PARTY_LICENSE_MAP) {
    assertNonEmptyRegularFile(path.join(root, item.relSource), `embedded notice ${item.relSource}`);
  }

  const noticesText = fs.readFileSync(path.join(root, 'THIRD-PARTY-NOTICES.txt'), 'utf8');
  if (!noticesText.includes('Complete Corresponding Source Code')) {
    throw new Error('THIRD-PARTY-NOTICES.txt is missing the GPLv3 §6 Complete Corresponding Source Code section.');
  }
  if (!noticesText.includes('Linking & Relinking Information')) {
    throw new Error('THIRD-PARTY-NOTICES.txt is missing the LGPLv3 §4 Linking & Relinking Information section.');
  }
  if (!noticesText.includes(brand.productUrl)) {
    throw new Error(`THIRD-PARTY-NOTICES.txt does not reference canonical product URL ${brand.productUrl}.`);
  }
  const expectedRepoUrl = `https://github.com/${brand.githubOwner || brand.productId}/${brand.productId}`;
  if (!noticesText.includes(expectedRepoUrl)) {
    throw new Error(`THIRD-PARTY-NOTICES.txt does not reference canonical source repository ${expectedRepoUrl}.`);
  }
  return true;
}

/**
 * Fail-fast verifier for a staged/installed `licenses/` directory (macOS, Windows, Linux).
 * Ensures all root notices, embedded `third-party/` component notices, and (when requested)
 * `third-party/<port>-COPYRIGHT` files exist, are regular files (never symlinks), and are non-empty.
 */
export function verifyInstalledLicensesDir(licensesDir, options = {}) {
  const resolvedDir = path.resolve(licensesDir);
  let dirStat;
  try {
    dirStat = fs.lstatSync(resolvedDir);
  } catch {
    throw new Error(`Installed licenses directory does not exist: ${resolvedDir}`);
  }
  if (dirStat.isSymbolicLink() || !dirStat.isDirectory()) {
    throw new Error(`Installed licenses path must be a regular directory (not a symlink): ${resolvedDir}`);
  }

  for (const rootFile of ROOT_LICENSE_FILES) {
    assertNonEmptyRegularFile(path.join(resolvedDir, rootFile), `licenses/${rootFile}`);
  }

  const thirdPartyDir = path.join(resolvedDir, 'third-party');
  let tpStat;
  try {
    tpStat = fs.lstatSync(thirdPartyDir);
  } catch {
    throw new Error(`Missing licenses/third-party directory: ${thirdPartyDir}`);
  }
  if (tpStat.isSymbolicLink() || !tpStat.isDirectory()) {
    throw new Error(`licenses/third-party must be a regular directory (not a symlink): ${thirdPartyDir}`);
  }

  for (const item of EMBEDDED_THIRD_PARTY_LICENSE_MAP) {
    assertNonEmptyRegularFile(
      path.join(thirdPartyDir, item.installedName),
      `licenses/third-party/${item.installedName}`
    );
  }

  // Recursively ensure NO entry anywhere under `licenses/` is empty or a symbolic link.
  const copyrightFiles = [];
  const walk = (dir) => {
    for (const entry of fs.readdirSync(dir)) {
      const full = path.resolve(dir, entry);
      if (!full.startsWith(`${resolvedDir}${path.sep}`)) {
        throw new Error(`Path traversal detected inside licenses/: ${full}`);
      }
      const st = fs.lstatSync(full);
      if (st.isSymbolicLink()) {
        throw new Error(`Refusing symbolic link inside licenses/: ${full}`);
      }
      if (st.isDirectory()) {
        walk(full);
      } else if (st.isFile()) {
        if (st.size <= 0) {
          throw new Error(`Empty (0-byte) license file inside licenses/: ${full}`);
        }
        if (dir === thirdPartyDir && entry.endsWith('-COPYRIGHT')) {
          copyrightFiles.push(entry);
        }
      } else {
        throw new Error(`Refusing non-regular file inside licenses/: ${full}`);
      }
    }
  };
  walk(resolvedDir);

  if (options.requireVcpkgCopyrights) {
    const requiredPorts = options.requiredVcpkgPorts || ['mongo-cxx-driver', 'openssl', 'libssh2'];
    for (const port of requiredPorts) {
      const expected = `${port}-COPYRIGHT`;
      if (!copyrightFiles.includes(expected)) {
        throw new Error(
          `Missing vcpkg copyright file "licenses/third-party/${expected}" in ${thirdPartyDir} (found: [${copyrightFiles.sort().join(', ')}]).`
        );
      }
    }
  }

  return {
    licensesDir: resolvedDir,
    rootFiles: [...ROOT_LICENSE_FILES],
    embeddedFiles: EMBEDDED_THIRD_PARTY_LICENSE_MAP.map((x) => x.installedName),
    vcpkgCopyrightFiles: copyrightFiles.sort(),
  };
}

function readCmakeMetadata(repoRoot) {
  const cmakePath = path.join(repoRoot, 'CMakeLists.txt');
  const text = fs.readFileSync(cmakePath, 'utf8');
  const projectVersion = readDefaultCmakeVersion(repoRoot);

  const readUnquotedOrQuoted = (varName, fallback) => {
    const re = new RegExp(`^set\\(${varName}\\s+"?([^"\\s)]+)"?\\)`, 'm');
    const m = re.exec(text);
    return m ? m[1] : fallback;
  };

  const qjsonVersion = readUnquotedOrQuoted('QJSON_VERSION', '0.8.1');
  const qscintillaVersion = readUnquotedOrQuoted('QSCINTILLA_VERSION', '2.8.4');
  const googletestVersion = readUnquotedOrQuoted('GOOGLE_TEST_VERSION', '1.8.1');

  const qtModules = [];
  const qtRe = /find_package\(Qt6\s+REQUIRED\s+COMPONENTS\s+([^)]+)\)/g;
  let m;
  while ((m = qtRe.exec(text)) !== null) {
    for (const tok of m[1].trim().split(/\s+/)) {
      if (tok && !qtModules.includes(tok)) {
        qtModules.push(tok);
      }
    }
  }

  return {
    projectVersion,
    qjsonVersion,
    qscintillaVersion,
    googletestVersion,
    qtModules,
  };
}

function readVcpkgManifest(repoRoot, brand, expectedCmakeVersion) {
  const vcpkgPath = path.join(repoRoot, 'vcpkg.json');
  assertNonEmptyRegularFile(vcpkgPath, 'vcpkg.json');
  const manifest = JSON.parse(fs.readFileSync(vcpkgPath, 'utf8'));

  if (manifest.name !== brand.productId) {
    throw new Error(`vcpkg.json "name" ("${manifest.name}") does not match RAGO_PRODUCT_ID ("${brand.productId}").`);
  }
  const manifestVer = normalizeAndValidateVersion(manifest['version-string'], 'vcpkg.json version-string');
  if (manifestVer !== expectedCmakeVersion) {
    throw new Error(
      `vcpkg.json "version-string" ("${manifestVer}") is out of sync with CMakeLists.txt PROJECT_VERSION ("${expectedCmakeVersion}").`
    );
  }
  const baseline = typeof manifest['builtin-baseline'] === 'string' ? manifest['builtin-baseline'].trim() : '';
  if (!/^[0-9a-f]{40}$/i.test(baseline)) {
    throw new Error('vcpkg.json must define a 40-hex commit SHA in "builtin-baseline".');
  }

  const directDeps = [];
  for (const dep of manifest.dependencies || []) {
    if (typeof dep === 'string') {
      directDeps.push(dep);
    } else if (dep && typeof dep === 'object' && typeof dep.name === 'string') {
      directDeps.push(dep.name);
    }
  }

  const overrides = new Map();
  for (const ov of manifest.overrides || []) {
    if (ov && typeof ov === 'object' && typeof ov.name === 'string' && typeof ov.version === 'string') {
      overrides.set(ov.name, ov.version.trim());
    }
  }

  return {
    baseline: baseline.toLowerCase(),
    directDeps,
    overrides,
  };
}

function discoverVcpkgSpdxFiles(repoRoot, explicitVcpkgDir, autoDiscover) {
  const candidateDirs = [];
  if (explicitVcpkgDir) {
    const resolved = path.resolve(explicitVcpkgDir);
    const st = fs.lstatSync(resolved);
    if (st.isSymbolicLink() || !st.isDirectory()) {
      throw new Error(`--vcpkg-installed must be a regular directory (not a symlink): ${resolved}`);
    }
    candidateDirs.push(resolved);
  } else if (autoDiscover) {
    const buildRoot = path.join(repoRoot, 'build');
    if (fs.existsSync(buildRoot)) {
      const bStat = fs.lstatSync(buildRoot);
      if (bStat.isDirectory() && !bStat.isSymbolicLink()) {
        for (const presetName of fs.readdirSync(buildRoot).sort()) {
          const vcpkgDir = path.join(buildRoot, presetName, 'vcpkg_installed');
          if (fs.existsSync(vcpkgDir)) {
            const st = fs.lstatSync(vcpkgDir);
            if (st.isDirectory() && !st.isSymbolicLink()) {
              candidateDirs.push(vcpkgDir);
            }
          }
        }
      }
    }
  }

  const byPort = new Map();
  for (const vcpkgBase of candidateDirs) {
    for (const triplet of fs.readdirSync(vcpkgBase).sort()) {
      if (triplet === 'vcpkg') continue;
      const shareDir = path.join(vcpkgBase, triplet, 'share');
      if (!fs.existsSync(shareDir)) continue;
      const shareStat = fs.lstatSync(shareDir);
      if (!shareStat.isDirectory() || shareStat.isSymbolicLink()) continue;

      for (const portName of fs.readdirSync(shareDir).sort()) {
        if (byPort.has(portName)) continue;
        const spdxPath = path.join(shareDir, portName, 'vcpkg.spdx.json');
        if (!fs.existsSync(spdxPath)) continue;
        const fStat = fs.lstatSync(spdxPath);
        if (!fStat.isFile() || fStat.isSymbolicLink() || fStat.size <= 0) continue;

        const parsed = JSON.parse(fs.readFileSync(spdxPath, 'utf8'));
        const portPkg = Array.isArray(parsed.packages)
          ? parsed.packages.find((p) => p && p.SPDXID === 'SPDXRef-port')
          : null;
        const resPkg = Array.isArray(parsed.packages)
          ? parsed.packages.find((p) => p && p.SPDXID === 'SPDXRef-resource-0')
          : null;
        if (!portPkg) continue;

        const rawVersion = typeof portPkg.versionInfo === 'string' ? portPkg.versionInfo.trim() : '';
        const [upstreamVersion, portRevision = ''] = rawVersion.split('#', 2);
        const portPurl = Array.isArray(portPkg.externalRefs)
          ? portPkg.externalRefs.find((r) => r && r.referenceType === 'purl')?.referenceLocator || ''
          : '';
        const resSha512 = Array.isArray(resPkg?.checksums)
          ? resPkg.checksums.find((c) => c && c.algorithm === 'SHA512')?.checksumValue?.toLowerCase() || ''
          : '';

        byPort.set(portName, {
          portName,
          triplet,
          upstreamVersion: upstreamVersion || rawVersion,
          portRevision,
          rawLicense: typeof portPkg.licenseConcluded === 'string' ? portPkg.licenseConcluded : '',
          homepage: typeof portPkg.homepage === 'string' ? portPkg.homepage : '',
          portDownloadLocation: typeof portPkg.downloadLocation === 'string' ? portPkg.downloadLocation : '',
          resourceDownloadLocation:
            typeof resPkg?.downloadLocation === 'string' && resPkg.downloadLocation !== 'NONE'
              ? resPkg.downloadLocation
              : '',
          portPurl,
          resourceSha512: SHA512_HEX_RE.test(resSha512) ? resSha512 : '',
        });
      }
    }
  }
  return byPort;
}

function detectQtVersion(repoRoot, explicitQtVersion, autoDiscover) {
  const candidates = [explicitQtVersion, process.env.RAGO_QT_VERSION, process.env.QT_VERSION];
  for (const c of candidates) {
    if (typeof c === 'string' && c.trim()) {
      return normalizeAndValidateVersion(c.trim(), 'Qt version');
    }
  }
  if (autoDiscover) {
    const buildRoot = path.join(repoRoot, 'build');
    if (fs.existsSync(buildRoot) && fs.lstatSync(buildRoot).isDirectory()) {
      for (const sub of fs.readdirSync(buildRoot).sort()) {
        const cachePath = path.join(buildRoot, sub, 'CMakeCache.txt');
        if (fs.existsSync(cachePath) && fs.lstatSync(cachePath).isFile()) {
          const cacheText = fs.readFileSync(cachePath, 'utf8');
          const m = /Qt[/\\](\d+\.\d+\.\d+)[/\\]/.exec(cacheText);
          if (m) {
            return normalizeAndValidateVersion(m[1], 'CMakeCache Qt version');
          }
        }
      }
    }
  }
  if (typeof process.env.QT_DEFAULT_VERSION === 'string' && process.env.QT_DEFAULT_VERSION.trim()) {
    return normalizeAndValidateVersion(process.env.QT_DEFAULT_VERSION.trim(), 'QT_DEFAULT_VERSION');
  }
  return '6.8.2';
}

function normalizeSpdxLicense(portName, rawLicense, fallbackLicense) {
  if (fallbackLicense) {
    if (!rawLicense || rawLicense === 'NOASSERTION' || rawLicense.startsWith('LicenseRef-vcpkg-')) {
      return fallbackLicense;
    }
  }
  if (!rawLicense || rawLicense.startsWith('LicenseRef-vcpkg-')) {
    return fallbackLicense || 'NOASSERTION';
  }
  return rawLicense;
}

function scanDistReleasePackages(distDir, brand, version) {
  if (!distDir || !fs.existsSync(distDir)) {
    return [];
  }
  const resolvedDist = path.resolve(distDir);
  const distStat = fs.lstatSync(resolvedDist);
  if (distStat.isSymbolicLink() || !distStat.isDirectory()) {
    throw new Error(`dist path must be a regular directory (not a symlink): ${resolvedDist}`);
  }

  const prefix = `${brand.productId}-${version}-`;
  const results = [];

  for (const entryName of fs.readdirSync(resolvedDist).sort()) {
    if (path.basename(entryName) !== entryName || entryName.includes('..')) {
      throw new Error(`Invalid directory entry in dist/: "${entryName}"`);
    }
    if (IGNORED_DIST_FOR_SBOM.has(entryName) || entryName.endsWith('.spdx.json') || entryName.endsWith('.spdx.json.minisig')) {
      continue;
    }
    const fullPath = path.resolve(resolvedDist, entryName);
    if (!fullPath.startsWith(`${resolvedDist}${path.sep}`)) {
      throw new Error(`Path traversal refused in dist/: "${entryName}"`);
    }
    const st = fs.lstatSync(fullPath);
    if (st.isSymbolicLink()) {
      throw new Error(`Refusing symbolic link inside dist/: "${entryName}"`);
    }
    if (st.isDirectory()) {
      if (entryName.startsWith(prefix)) continue;
      throw new Error(`Unexpected directory inside dist/: "${entryName}"`);
    }
    if (!st.isFile()) {
      throw new Error(`Refusing non-regular file inside dist/: "${entryName}"`);
    }
    if (!entryName.startsWith(prefix)) {
      throw new Error(
        `Artifact "${entryName}" in dist/ does not match release prefix "${prefix}" (clean stale files first).`
      );
    }
    const suffixPart = entryName.slice(prefix.length);
    if (!RELEASE_PACKAGE_SUFFIX_RE.test(suffixPart)) {
      throw new Error(`Unrecognized release artifact in dist/: "${entryName}"`);
    }
    if (st.size <= 0) {
      throw new Error(`Release artifact "${entryName}" is empty (0 bytes).`);
    }

    const sha256 = computeFileSha256(fullPath);
    const downloadUrl = validateAllowedHttpsUrl(
      `${brand.defaultDlBaseUrl}/releases/${encodeURIComponent(version)}/${encodeURIComponent(entryName)}`,
      brand.allowedHosts,
      `SBOM artifact URL (${entryName})`
    );
    results.push({
      name: entryName,
      size: st.size,
      sha256,
      downloadUrl,
    });
  }

  return results;
}

/**
 * Generates a deterministic SPDX 2.3 JSON SBOM document and optionally writes it to disk.
 */
export function generateSbom(options = {}) {
  const repoRoot = path.resolve(options.repoRoot || DEFAULT_REPO_ROOT);
  verifySourceLicenseNotices(repoRoot);

  const brand = readBrandDefaults(repoRoot);
  const cmakeMeta = readCmakeMetadata(repoRoot);
  const version = normalizeAndValidateVersion(
    options.version || cmakeMeta.projectVersion,
    'version'
  );
  const vcpkgManifest = readVcpkgManifest(repoRoot, brand, cmakeMeta.projectVersion);

  const autoDiscover = options.autoDiscoverVcpkg !== false;
  const discoveredVcpkg = discoverVcpkgSpdxFiles(repoRoot, options.vcpkgInstalledDir || '', autoDiscover);
  const qtVersion = detectQtVersion(repoRoot, options.qtVersion || '', autoDiscover);
  const created = resolveCreatedTimestamp(options.created);

  const distDir = options.distDir !== undefined
    ? (options.distDir ? path.resolve(options.distDir) : '')
    : path.join(repoRoot, 'dist');
  const distPackages = options.skipDist ? [] : scanDistReleasePackages(distDir, brand, version);

  const packages = [];
  const relationships = [];
  const rootPkgId = `SPDXRef-Package-${sanitizeSpdxToken(brand.productId)}`;

  const repoOwner = brand.githubOwner || brand.productId;
  const primaryVcsUrl = `git+https://github.com/${repoOwner}/${brand.productId}@v${version}`;

  // 1. Root RagoShell application package
  packages.push({
    name: brand.productId,
    SPDXID: rootPkgId,
    versionInfo: version,
    supplier: `Organization: ${brand.productName} contributors`,
    downloadLocation: primaryVcsUrl,
    filesAnalyzed: false,
    homepage: brand.productUrl,
    licenseConcluded: 'GPL-3.0-only',
    licenseDeclared: 'GPL-3.0-only',
    copyrightText: `Copyright (C) 2014-2021 Robomongo original authors and contributors; Copyright (C) 2026 ${brand.productName} contributors`,
    primaryPackagePurpose: 'APPLICATION',
    externalRefs: [
      {
        referenceCategory: 'PACKAGE-MANAGER',
        referenceType: 'purl',
        referenceLocator: `pkg:generic/${brand.productId}@${version}?vcs_url=${encodeURIComponent(primaryVcsUrl)}`,
      },
      {
        referenceCategory: 'SECURITY',
        referenceType: 'cpe23Type',
        referenceLocator: buildCpe23(brand.productId, brand.productId, version),
      },
    ],
  });

  relationships.push({
    spdxElementId: 'SPDXRef-DOCUMENT',
    relationshipType: 'DESCRIBES',
    relatedSpdxElement: rootPkgId,
  });

  // 2. vcpkg C/C++ dependencies (manifest + overrides + transitive + discovered vcpkg.spdx.json)
  const allPortNames = new Set([
    ...Object.keys(VCPKG_PORT_CATALOG),
    ...vcpkgManifest.directDeps,
    ...vcpkgManifest.overrides.keys(),
    ...discoveredVcpkg.keys(),
  ]);

  const vcpkgPkgIds = new Map();
  for (const portName of [...allPortNames].sort()) {
    const catalog = VCPKG_PORT_CATALOG[portName] || null;
    const spdxInfo = discoveredVcpkg.get(portName) || null;
    const isBuildHelper = portName.startsWith('vcpkg-');

    const resolvedUpstreamVersion =
      vcpkgManifest.overrides.get(portName) ||
      spdxInfo?.upstreamVersion ||
      catalog?.defaultVersion ||
      '0.0.0';

    const pkgId = `SPDXRef-Package-vcpkg-${sanitizeSpdxToken(portName)}`;
    vcpkgPkgIds.set(portName, pkgId);

    const license = normalizeSpdxLicense(portName, spdxInfo?.rawLicense || '', catalog?.license || 'MIT');
    const downloadLocation =
      spdxInfo?.resourceDownloadLocation ||
      (catalog ? catalog.downloadTemplate(resolvedUpstreamVersion) : spdxInfo?.portDownloadLocation) ||
      `git+https://github.com/microsoft/vcpkg@${vcpkgManifest.baseline}#ports/${portName}`;

    const portQualifiers = [`baseline=${vcpkgManifest.baseline}`];
    if (spdxInfo?.portRevision) {
      portQualifiers.push(`port_version=${encodeURIComponent(spdxInfo.portRevision)}`);
    }
    const vcpkgPurl = `pkg:vcpkg/${portName}@${encodeURIComponent(resolvedUpstreamVersion)}?${portQualifiers.join('&')}`;

    const externalRefs = [
      {
        referenceCategory: 'PACKAGE-MANAGER',
        referenceType: 'purl',
        referenceLocator: vcpkgPurl,
      },
    ];
    if (catalog?.githubPurl) {
      externalRefs.push({
        referenceCategory: 'PACKAGE-MANAGER',
        referenceType: 'purl',
        referenceLocator: catalog.githubPurl(resolvedUpstreamVersion),
      });
    }
    if (catalog?.cpeVendor && catalog?.cpeProduct) {
      externalRefs.push({
        referenceCategory: 'SECURITY',
        referenceType: 'cpe23Type',
        referenceLocator: buildCpe23(catalog.cpeVendor, catalog.cpeProduct, resolvedUpstreamVersion),
      });
    }

    const pkgObj = {
      name: portName,
      SPDXID: pkgId,
      versionInfo: resolvedUpstreamVersion,
      supplier: catalog?.supplier || 'Organization: Microsoft vcpkg contributors',
      downloadLocation,
      filesAnalyzed: false,
      homepage: spdxInfo?.homepage || catalog?.homepage || 'https://github.com/microsoft/vcpkg',
      licenseConcluded: license,
      licenseDeclared: license,
      copyrightText: catalog?.copyrightText || 'NOASSERTION',
      primaryPackagePurpose: isBuildHelper ? 'BUILD' : 'LIBRARY',
      externalRefs,
    };
    if (spdxInfo?.resourceSha512) {
      pkgObj.checksums = [{ algorithm: 'SHA512', checksumValue: spdxInfo.resourceSha512 }];
    }
    packages.push(pkgObj);

    if (isBuildHelper) {
      relationships.push({
        spdxElementId: pkgId,
        relationshipType: 'BUILD_DEPENDENCY_OF',
        relatedSpdxElement: rootPkgId,
      });
    } else {
      relationships.push({
        spdxElementId: rootPkgId,
        relationshipType: 'DEPENDS_ON',
        relatedSpdxElement: pkgId,
      });
    }
  }

  for (const [portName, catalog] of Object.entries(VCPKG_PORT_CATALOG)) {
    const parentId = vcpkgPkgIds.get(portName);
    if (!parentId) continue;
    for (const depPort of catalog.dependsOn) {
      const childId = vcpkgPkgIds.get(depPort);
      if (childId) {
        relationships.push({
          spdxElementId: parentId,
          relationshipType: 'DEPENDS_ON',
          relatedSpdxElement: childId,
        });
      }
    }
  }

  // 3. Qt 6 modules declared in CMakeLists.txt
  for (const mod of [...cmakeMeta.qtModules].sort()) {
    const modLower = mod.toLowerCase();
    const pkgId = `SPDXRef-Package-qt6-${sanitizeSpdxToken(modLower)}`;
    const isBuildTool = mod === 'LinguistTools';
    const isBsdStartup = mod === 'EntryPointPrivate';
    const qtLicense = isBsdStartup
      ? 'BSD-3-Clause'
      : isBuildTool
        ? 'GPL-3.0-only WITH Qt-GPL-exception-1.0'
        : 'LGPL-3.0-only OR GPL-3.0-only';

    packages.push({
      name: `qt6-${modLower}`,
      SPDXID: pkgId,
      versionInfo: qtVersion,
      supplier: 'Organization: The Qt Company Ltd.',
      downloadLocation: `https://download.qt.io/archive/qt/${qtVersion.split('.').slice(0, 2).join('.')}/${qtVersion}/`,
      filesAnalyzed: false,
      homepage: 'https://www.qt.io/',
      licenseConcluded: qtLicense,
      licenseDeclared: qtLicense,
      copyrightText: 'Copyright (C) The Qt Company Ltd. and other contributors',
      primaryPackagePurpose: isBuildTool ? 'BUILD' : 'FRAMEWORK',
      externalRefs: [
        {
          referenceCategory: 'PACKAGE-MANAGER',
          referenceType: 'purl',
          referenceLocator: `pkg:generic/qt@${qtVersion}?module=${encodeURIComponent(mod)}`,
        },
        {
          referenceCategory: 'SECURITY',
          referenceType: 'cpe23Type',
          referenceLocator: buildCpe23('qt', 'qt', qtVersion),
        },
      ],
    });

    if (isBuildTool) {
      relationships.push({
        spdxElementId: pkgId,
        relationshipType: 'BUILD_DEPENDENCY_OF',
        relatedSpdxElement: rootPkgId,
      });
    } else {
      relationships.push({
        spdxElementId: rootPkgId,
        relationshipType: 'DEPENDS_ON',
        relatedSpdxElement: pkgId,
      });
    }
  }

  // 4. Embedded `src/third-party/` and bundled font components
  const embeddedSpecs = [
    {
      name: 'qscintilla',
      version: cmakeMeta.qscintillaVersion,
      supplier: 'Organization: Riverbank Computing Limited',
      downloadLocation: 'https://www.riverbankcomputing.com/software/qscintilla/download',
      homepage: 'https://www.riverbankcomputing.com/software/qscintilla/',
      license: 'GPL-3.0-only AND LicenseRef-Scintilla',
      copyrightText: 'Copyright (c) Riverbank Computing Limited; Copyright 1998-2014 by Neil Hodgson',
      checksumFile: path.join(repoRoot, `src/third-party/qscintilla-${cmakeMeta.qscintillaVersion}/sources/LICENSE.GPL3`),
      purl: `pkg:generic/qscintilla@${cmakeMeta.qscintillaVersion}`,
      cpe: buildCpe23('riverbankcomputing', 'qscintilla', cmakeMeta.qscintillaVersion),
      relationship: 'DEPENDS_ON',
    },
    {
      name: 'qjson',
      version: cmakeMeta.qjsonVersion,
      supplier: 'Person: Flavio Castelli',
      downloadLocation: `git+https://github.com/flavio/qjson@${cmakeMeta.qjsonVersion}`,
      homepage: 'https://github.com/flavio/qjson',
      license: 'LGPL-2.1-only',
      copyrightText: 'Copyright (C) 2008 Flavio Castelli <flavio.castelli@gmail.com>',
      checksumFile: path.join(repoRoot, `src/third-party/qjson-${cmakeMeta.qjsonVersion}/sources/README.license`),
      purl: `pkg:github/flavio/qjson@${cmakeMeta.qjsonVersion}`,
      cpe: buildCpe23('qjson_project', 'qjson', cmakeMeta.qjsonVersion),
      relationship: 'DEPENDS_ON',
    },
    {
      name: 'dejavu-sans-ttf',
      version: '2.37',
      supplier: 'Organization: DejaVu Fonts Project',
      downloadLocation: 'https://github.com/dejavu-fonts/dejavu-fonts/releases/tag/version_2_37',
      homepage: 'https://dejavu-fonts.github.io/',
      license: 'Bitstream-Vera',
      copyrightText: 'Copyright (c) 2003 by Bitstream, Inc.; DejaVu changes are in public domain',
      checksumFile: path.join(repoRoot, 'assets/fonts/DejaVuSans.ttf'),
      purl: 'pkg:generic/dejavu-sans-ttf@2.37',
      cpe: null,
      relationship: 'DEPENDS_ON',
    },
    {
      name: 'googletest',
      version: cmakeMeta.googletestVersion,
      supplier: 'Organization: Google Inc.',
      downloadLocation: `git+https://github.com/google/googletest@release-${cmakeMeta.googletestVersion}`,
      homepage: 'https://github.com/google/googletest',
      license: 'BSD-3-Clause',
      copyrightText: 'Copyright 2008, Google Inc.',
      checksumFile: path.join(repoRoot, `src/third-party/googletest-${cmakeMeta.googletestVersion}/LICENSE`),
      purl: `pkg:github/google/googletest@release-${cmakeMeta.googletestVersion}`,
      cpe: buildCpe23('google', 'googletest', cmakeMeta.googletestVersion),
      relationship: 'TEST_DEPENDENCY_OF',
    },
  ];

  for (const emb of embeddedSpecs) {
    assertNonEmptyRegularFile(emb.checksumFile, `embedded component ${emb.name}`);
    const sha256 = computeFileSha256(emb.checksumFile);
    const pkgId = `SPDXRef-Package-embedded-${sanitizeSpdxToken(emb.name)}`;
    const externalRefs = [
      {
        referenceCategory: 'PACKAGE-MANAGER',
        referenceType: 'purl',
        referenceLocator: emb.purl,
      },
    ];
    if (emb.cpe) {
      externalRefs.push({
        referenceCategory: 'SECURITY',
        referenceType: 'cpe23Type',
        referenceLocator: emb.cpe,
      });
    }

    packages.push({
      name: emb.name,
      SPDXID: pkgId,
      versionInfo: emb.version,
      supplier: emb.supplier,
      downloadLocation: emb.downloadLocation,
      filesAnalyzed: false,
      homepage: emb.homepage,
      checksums: [{ algorithm: 'SHA256', checksumValue: sha256 }],
      licenseConcluded: emb.license,
      licenseDeclared: emb.license,
      copyrightText: emb.copyrightText,
      primaryPackagePurpose: 'LIBRARY',
      externalRefs,
    });

    if (emb.relationship === 'TEST_DEPENDENCY_OF') {
      relationships.push({
        spdxElementId: pkgId,
        relationshipType: 'TEST_DEPENDENCY_OF',
        relatedSpdxElement: rootPkgId,
      });
    } else {
      relationships.push({
        spdxElementId: rootPkgId,
        relationshipType: 'DEPENDS_ON',
        relatedSpdxElement: pkgId,
      });
    }
  }

  // 5. Built release artifacts in `dist/` (when present)
  for (const art of distPackages) {
    const artId = `SPDXRef-Artifact-${sanitizeSpdxToken(art.name)}`;
    const isArchive = art.name.endsWith('.zip') || art.name.endsWith('.tar.gz');
    packages.push({
      name: art.name,
      SPDXID: artId,
      versionInfo: version,
      packageFileName: art.name,
      supplier: `Organization: ${brand.productName} contributors`,
      downloadLocation: art.downloadUrl,
      filesAnalyzed: false,
      checksums: [{ algorithm: 'SHA256', checksumValue: art.sha256 }],
      licenseConcluded: 'GPL-3.0-only',
      licenseDeclared: 'GPL-3.0-only',
      copyrightText: `Copyright (C) 2014-2021 Robomongo original authors and contributors; Copyright (C) 2026 ${brand.productName} contributors`,
      primaryPackagePurpose: isArchive ? 'ARCHIVE' : 'INSTALL',
      externalRefs: [
        {
          referenceCategory: 'PACKAGE-MANAGER',
          referenceType: 'purl',
          referenceLocator: `pkg:generic/${brand.productId}@${version}?artifact=${encodeURIComponent(art.name)}`,
        },
      ],
    });
    relationships.push({
      spdxElementId: artId,
      relationshipType: 'GENERATED_FROM',
      relatedSpdxElement: rootPkgId,
    });
  }

  const docName = `${brand.productId}-${version}`;
  const canonicalPayload = JSON.stringify({
    name: docName,
    packages,
    relationships,
  });
  const docUuid = makeUuidV5FromName(`https://dl.${brand.domain}/spdx/${docName}:${canonicalPayload}`);
  const documentNamespace = `https://dl.${brand.domain}/spdx/${docName}-${docUuid}`;

  const document = {
    $schema: 'https://raw.githubusercontent.com/spdx/spdx-spec/v2.3/schemas/spdx-schema.json',
    spdxVersion: 'SPDX-2.3',
    dataLicense: 'CC0-1.0',
    SPDXID: 'SPDXRef-DOCUMENT',
    name: docName,
    documentNamespace,
    creationInfo: {
      licenseListVersion: '3.25',
      creators: [
        `Organization: ${brand.productName} contributors`,
        `Tool: ${brand.productId}-generate-sbom-1.0.0`,
      ],
      created,
    },
    documentDescribes: [rootPkgId],
    hasExtractedLicensingInfo: [SCINTILLA_EXTRACTED_LICENSE],
    packages,
    relationships,
  };

  validateSpdxDocument(document, { expectedDomain: brand.domain });

  const sbomFilename = `${brand.productId}-${version}-sbom.spdx.json`;
  const defaultOutDir = distDir || path.join(repoRoot, 'dist');
  const outputPath = path.resolve(options.outputPath || path.join(defaultOutDir, sbomFilename));
  const sbomJson = `${JSON.stringify(document, null, 2)}\n`;

  if (options.writeFile !== false) {
    writeRegularFileAtomic(outputPath, sbomJson);
  }

  return {
    brand,
    version,
    sbomFilename,
    outputPath,
    document,
    sbomJson,
    distArtifactCount: distPackages.length,
    vcpkgDiscoveredCount: discoveredVcpkg.size,
  };
}

/**
 * Strictly validates an SPDX 2.3 JSON document object against schema, PURL/CPE,
 * license reference, checksum, and relationship graph invariants.
 */
export function validateSpdxDocument(doc, options = {}) {
  if (!doc || typeof doc !== 'object' || Array.isArray(doc)) {
    throw new Error('SPDX SBOM must be a JSON object.');
  }
  if (doc.spdxVersion !== 'SPDX-2.3') {
    throw new Error(`Unsupported spdxVersion "${doc.spdxVersion}" (expected "SPDX-2.3").`);
  }
  if (doc.dataLicense !== 'CC0-1.0') {
    throw new Error(`Invalid SPDX dataLicense "${doc.dataLicense}" (expected "CC0-1.0").`);
  }
  if (doc.SPDXID !== 'SPDXRef-DOCUMENT') {
    throw new Error(`Invalid root SPDXID "${doc.SPDXID}" (expected "SPDXRef-DOCUMENT").`);
  }
  if (typeof doc.name !== 'string' || doc.name.trim().length === 0 || doc.name.length > 128) {
    throw new Error('SPDX document "name" must be a non-empty string (<= 128 chars).');
  }
  if (typeof doc.documentNamespace !== 'string' || doc.documentNamespace.includes('#')) {
    throw new Error('SPDX "documentNamespace" must be an https:// URI without "#" fragments.');
  }
  let nsUrl;
  try {
    nsUrl = new URL(doc.documentNamespace);
  } catch {
    throw new Error(`Malformed SPDX documentNamespace URI: "${doc.documentNamespace}"`);
  }
  if (nsUrl.protocol !== 'https:') {
    throw new Error(`SPDX documentNamespace must use https://: "${doc.documentNamespace}"`);
  }
  if (options.expectedDomain && nsUrl.hostname !== `dl.${options.expectedDomain}`) {
    throw new Error(
      `SPDX documentNamespace host "${nsUrl.hostname}" does not match expected "dl.${options.expectedDomain}".`
    );
  }

  if (!doc.creationInfo || typeof doc.creationInfo !== 'object') {
    throw new Error('SPDX "creationInfo" object is required.');
  }
  if (!ISO_UTC_SECONDS_RE.test(doc.creationInfo.created || '')) {
    throw new Error(`Invalid creationInfo.created "${doc.creationInfo.created}" (expected YYYY-MM-DDTHH:MM:SSZ).`);
  }
  if (!Array.isArray(doc.creationInfo.creators) || doc.creationInfo.creators.length === 0) {
    throw new Error('SPDX creationInfo.creators must be a non-empty array.');
  }

  const extractedLicenseIds = new Set();
  if (doc.hasExtractedLicensingInfo !== undefined) {
    if (!Array.isArray(doc.hasExtractedLicensingInfo)) {
      throw new Error('SPDX hasExtractedLicensingInfo must be an array when present.');
    }
    for (const info of doc.hasExtractedLicensingInfo) {
      if (
        !info ||
        typeof info.licenseId !== 'string' ||
        !info.licenseId.startsWith('LicenseRef-') ||
        typeof info.extractedText !== 'string' ||
        info.extractedText.trim().length === 0
      ) {
        throw new Error('Invalid entry in hasExtractedLicensingInfo.');
      }
      extractedLicenseIds.add(info.licenseId);
    }
  }

  if (!Array.isArray(doc.packages) || doc.packages.length === 0) {
    throw new Error('SPDX "packages" must be a non-empty array.');
  }

  const elementIds = new Set(['SPDXRef-DOCUMENT']);
  for (const pkg of doc.packages) {
    if (!pkg || typeof pkg !== 'object' || Array.isArray(pkg)) {
      throw new Error('Each SPDX package entry must be an object.');
    }
    if (typeof pkg.SPDXID !== 'string' || !SPDX_ID_RE.test(pkg.SPDXID)) {
      throw new Error(`Invalid package SPDXID "${pkg.SPDXID}".`);
    }
    if (elementIds.has(pkg.SPDXID)) {
      throw new Error(`Duplicate SPDXID "${pkg.SPDXID}" in packages.`);
    }
    elementIds.add(pkg.SPDXID);

    if (typeof pkg.name !== 'string' || pkg.name.trim().length === 0) {
      throw new Error(`Package ${pkg.SPDXID} is missing "name".`);
    }
    if (typeof pkg.versionInfo !== 'string' || pkg.versionInfo.trim().length === 0) {
      throw new Error(`Package ${pkg.SPDXID} is missing "versionInfo".`);
    }
    if (typeof pkg.downloadLocation !== 'string' || pkg.downloadLocation.trim().length === 0) {
      throw new Error(`Package ${pkg.SPDXID} is missing "downloadLocation".`);
    }
    if (typeof pkg.filesAnalyzed !== 'boolean') {
      throw new Error(`Package ${pkg.SPDXID} must specify boolean "filesAnalyzed".`);
    }
    for (const licField of ['licenseConcluded', 'licenseDeclared']) {
      const expr = pkg[licField];
      if (typeof expr !== 'string' || expr.trim().length === 0) {
        throw new Error(`Package ${pkg.SPDXID} is missing "${licField}".`);
      }
      const refMatches = expr.match(/LicenseRef-[A-Za-z0-9.-]+/g) || [];
      for (const refId of refMatches) {
        if (!extractedLicenseIds.has(refId)) {
          throw new Error(
            `Package ${pkg.SPDXID} references undefined custom license "${refId}" not in hasExtractedLicensingInfo.`
          );
        }
      }
    }

    if (pkg.checksums !== undefined) {
      if (!Array.isArray(pkg.checksums) || pkg.checksums.length === 0) {
        throw new Error(`Package ${pkg.SPDXID} checksums must be a non-empty array when present.`);
      }
      for (const c of pkg.checksums) {
        if (c.algorithm === 'SHA256' && !SHA256_HEX_RE.test(c.checksumValue || '')) {
          throw new Error(`Invalid SHA256 checksum on package ${pkg.SPDXID}.`);
        }
        if (c.algorithm === 'SHA512' && !SHA512_HEX_RE.test(c.checksumValue || '')) {
          throw new Error(`Invalid SHA512 checksum on package ${pkg.SPDXID}.`);
        }
      }
    }

    if (!Array.isArray(pkg.externalRefs) || pkg.externalRefs.length === 0) {
      throw new Error(`Package ${pkg.SPDXID} must contain at least one externalRef (purl).`);
    }
    let hasPurl = false;
    for (const ref of pkg.externalRefs) {
      if (ref.referenceType === 'purl') {
        if (ref.referenceCategory !== 'PACKAGE-MANAGER' || !PURL_RE.test(ref.referenceLocator || '')) {
          throw new Error(`Invalid purl externalRef on ${pkg.SPDXID}: "${ref.referenceLocator}"`);
        }
        hasPurl = true;
      } else if (ref.referenceType === 'cpe23Type') {
        if (ref.referenceCategory !== 'SECURITY' || !CPE23_RE.test(ref.referenceLocator || '')) {
          throw new Error(`Invalid cpe23Type externalRef on ${pkg.SPDXID}: "${ref.referenceLocator}"`);
        }
      }
    }
    if (!hasPurl) {
      throw new Error(`Package ${pkg.SPDXID} is missing a purl externalRef.`);
    }
  }

  if (!Array.isArray(doc.documentDescribes) || doc.documentDescribes.length === 0) {
    throw new Error('SPDX "documentDescribes" must be a non-empty array.');
  }
  for (const id of doc.documentDescribes) {
    if (!elementIds.has(id)) {
      throw new Error(`documentDescribes references unknown SPDXID "${id}".`);
    }
  }

  if (!Array.isArray(doc.relationships) || doc.relationships.length === 0) {
    throw new Error('SPDX "relationships" must be a non-empty array.');
  }
  for (const rel of doc.relationships) {
    if (!rel || typeof rel !== 'object') {
      throw new Error('Invalid relationship entry.');
    }
    if (!elementIds.has(rel.spdxElementId)) {
      throw new Error(`Relationship references unknown spdxElementId "${rel.spdxElementId}".`);
    }
    if (!elementIds.has(rel.relatedSpdxElement)) {
      throw new Error(`Relationship references unknown relatedSpdxElement "${rel.relatedSpdxElement}".`);
    }
    if (typeof rel.relationshipType !== 'string' || !/^[A-Z_]+$/.test(rel.relationshipType)) {
      throw new Error(`Invalid relationshipType "${rel.relationshipType}".`);
    }
  }

  return true;
}

/**
 * Verifies an existing `.spdx.json` file on disk and cross-checks any `SPDXRef-Artifact-*`
 * SHA-256 digests against actual files in `distDir` when `distDir` is provided.
 */
export function verifySbomFile(options = {}) {
  const repoRoot = path.resolve(options.repoRoot || DEFAULT_REPO_ROOT);
  const brand = readBrandDefaults(repoRoot);
  const version = normalizeAndValidateVersion(
    options.version || readDefaultCmakeVersion(repoRoot),
    'version'
  );
  const distDir = options.distDir ? path.resolve(options.distDir) : '';
  const sbomFilename = `${brand.productId}-${version}-sbom.spdx.json`;
  const sbomPath = path.resolve(
    options.sbomPath || path.join(distDir || path.join(repoRoot, 'dist'), sbomFilename)
  );

  assertNonEmptyRegularFile(sbomPath, `SBOM ${sbomFilename}`);
  const doc = JSON.parse(fs.readFileSync(sbomPath, 'utf8'));
  validateSpdxDocument(doc, { expectedDomain: brand.domain });

  if (doc.name !== `${brand.productId}-${version}`) {
    throw new Error(`SBOM document name "${doc.name}" does not match expected "${brand.productId}-${version}".`);
  }

  if (distDir && fs.existsSync(distDir)) {
    const actualDist = scanDistReleasePackages(distDir, brand, version);
    const actualByName = new Map(actualDist.map((a) => [a.name, a]));
    const sbomArtifacts = doc.packages.filter((p) => p.SPDXID.startsWith('SPDXRef-Artifact-'));

    for (const artPkg of sbomArtifacts) {
      const diskArt = actualByName.get(artPkg.name);
      if (!diskArt) {
        throw new Error(`SBOM lists release artifact "${artPkg.name}", which is missing from ${distDir}.`);
      }
      const recordedSha = artPkg.checksums?.find((c) => c.algorithm === 'SHA256')?.checksumValue;
      if (recordedSha !== diskArt.sha256) {
        throw new Error(
          `SBOM SHA-256 mismatch for "${artPkg.name}": sbom=${recordedSha}, disk=${diskArt.sha256}.`
        );
      }
    }
    if (actualDist.length > 0 && sbomArtifacts.length !== actualDist.length) {
      throw new Error(
        `SBOM artifact package count (${sbomArtifacts.length}) does not match dist/ binary count (${actualDist.length}).`
      );
    }
  }

  return { brand, version, sbomPath, document: doc };
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
      case '--vcpkg-installed':
        opts.vcpkgInstalledDir = next();
        break;
      case '--no-auto-vcpkg':
        opts.autoDiscoverVcpkg = false;
        break;
      case '--skip-dist':
        opts.skipDist = true;
        break;
      case '--qt-version':
        opts.qtVersion = next();
        break;
      case '--created':
        opts.created = next();
        break;
      case '--verify-only':
        opts.verifyOnly = true;
        break;
      case '--verify-source-licenses':
        opts.verifySourceLicenses = true;
        break;
      case '--verify-licenses':
        opts.verifyLicensesDir = next();
        break;
      case '--require-vcpkg-copyrights':
        opts.requireVcpkgCopyrights = true;
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
          'Usage: node ci/generate-sbom.mjs [options]',
          '',
          'Options:',
          '  --version <ver>                Release version or v-tag (defaults to CMakeLists.txt)',
          '  --dist <dir>                   Release artifact directory (default: ./dist)',
          '  --output <file>                SBOM output path (default: <dist>/<product>-<version>-sbom.spdx.json)',
          '  --vcpkg-installed <dir>        Explicit vcpkg_installed root to merge vcpkg.spdx.json metadata',
          '  --no-auto-vcpkg                Disable auto-discovery of build/*/vcpkg_installed',
          '  --skip-dist                    Generate source/dependency SBOM only (ignore dist/ packages)',
          '  --qt-version <ver>             Explicit Qt 6 version (e.g. 6.12.0)',
          '  --created <YYYY-MM-DDTHH:MM:SSZ> Explicit deterministic UTC creation timestamp',
          '  --verify-only                  Verify an existing SPDX 2.3 SBOM file against schema & dist/',
          '  --verify-source-licenses       Verify root & embedded source license notices (GPLv3 §6 / LGPLv3 §4)',
          '  --verify-licenses <dir>        Fail-fast verify an installed licenses/ directory',
          '  --require-vcpkg-copyrights     With --verify-licenses, require third-party/<port>-COPYRIGHT files',
          '',
        ].join('\n')
      );
      process.exit(0);
    }

    if (cli.verifySourceLicenses) {
      verifySourceLicenseNotices(DEFAULT_REPO_ROOT);
      process.stdout.write('==> verified source license notices & GPLv3 §6 / LGPLv3 §4 declarations\n');
    } else if (cli.verifyLicensesDir) {
      const res = verifyInstalledLicensesDir(cli.verifyLicensesDir, {
        requireVcpkgCopyrights: Boolean(cli.requireVcpkgCopyrights),
      });
      process.stdout.write(
        `==> verified installed licenses bundle: ${res.licensesDir} (${res.rootFiles.length} root, ${res.embeddedFiles.length} embedded, ${res.vcpkgCopyrightFiles.length} vcpkg copyright file(s))\n`
      );
    } else if (cli.verifyOnly) {
      const res = verifySbomFile({
        version: cli.version,
        distDir: cli.distDir,
        sbomPath: cli.outputPath,
      });
      process.stdout.write(
        `==> verified SPDX 2.3 SBOM: ${res.sbomPath} (${res.document.packages.length} package(s), ${res.document.relationships.length} relationship(s))\n`
      );
    } else {
      const res = generateSbom(cli);
      process.stdout.write(
        `==> generated SPDX 2.3 SBOM: ${res.outputPath} (${res.document.packages.length} package(s), ${res.distArtifactCount} dist artifact(s))\n`
      );
    }
  } catch (err) {
    process.stderr.write(`error: ${err instanceof Error ? err.message : String(err)}\n`);
    process.exit(1);
  }
}
