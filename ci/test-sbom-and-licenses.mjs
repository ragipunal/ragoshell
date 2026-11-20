#!/usr/bin/env node
/**
 * Automated unit, integration & negative security test suite for Roadmap Phase 3.7:
 * - A5: CMake `licenses/` bundle (`cmake/RagoShellLicenses.cmake`), GPLv3 §6 / LGPLv3 §4
 *   declarations in `THIRD-PARTY-NOTICES.txt`, brand cleanliness, and fail-fast license
 *   directory verification (`--verify-licenses`, `--verify-source-licenses`).
 * - E6: Deterministic SPDX 2.3 JSON SBOM generation (`ci/generate-sbom.mjs`), `purl` and
 *   `cpe23Type` external identifiers, `vcpkg.spdx.json` merging, `dist/` SHA-256 digests,
 *   negative/tamper guards, and release pipeline (`releases.json`, `SHA256SUMS.txt`, R2,
 *   GitHub Releases, Gitea Releases, `ragoshell-site`, `release-pulse-api`) integration.
 */

import assert from 'node:assert/strict';
import { execFileSync, spawnSync } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

import {
  generateReleaseManifest,
  readBrandDefaults,
  readDefaultCmakeVersion,
  verifyReleaseDirectory,
} from './generate-releases-json.mjs';
import {
  EMBEDDED_THIRD_PARTY_LICENSE_MAP,
  ROOT_LICENSE_FILES,
  generateSbom,
  validateSpdxDocument,
  verifyInstalledLicensesDir,
  verifySbomFile,
  verifySourceLicenseNotices,
} from './generate-sbom.mjs';
import { generateEphemeralTestKeyPair, signBufferWithTestKey } from './lib/minisign.mjs';
import { createReleasePulseServer } from '../services/release-pulse-api/server.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(__dirname, '..');

// Pattern for pre-rename product name without spelling it literally in source.
const FORBIDDEN_OLD_BRAND_RE = /rago[\s_-]?mongo/i;

function makeTempDir(prefix) {
  return fs.mkdtempSync(path.join(os.tmpdir(), prefix));
}

function resolveCmakeBinary() {
  const probe = spawnSync('cmake', ['--version'], { encoding: 'utf8' });
  if (!probe.error && probe.status === 0) {
    return 'cmake';
  }
  const fallbackCandidates = [
    path.join(os.homedir(), 'Library/Android/sdk/cmake/3.31.6/bin/cmake'),
    '/opt/homebrew/bin/cmake',
    '/usr/local/bin/cmake',
    '/usr/bin/cmake',
  ];
  for (const candidate of fallbackCandidates) {
    if (fs.existsSync(candidate)) {
      const check = spawnSync(candidate, ['--version'], { encoding: 'utf8' });
      if (!check.error && check.status === 0) {
        return candidate;
      }
    }
  }
  return '';
}

function populateFakeDist(distDir, productId, version) {
  fs.mkdirSync(distDir, { recursive: true });
  const specs = [
    [`${productId}-${version}-windows-x64-setup.exe`, `MZ-fake-nsis-installer-${version}\n`],
    [`${productId}-${version}-windows-x64.msi`, `D0CF11E0-fake-wix-msi-${version}\n`],
    [`${productId}-${version}-windows-x64.zip`, `PK-fake-windows-portable-${version}\n`],
    [`${productId}-${version}-macos-arm64.dmg`, `koly-fake-macos-dmg-${version}\n`],
    [`${productId}-${version}-macos-arm64.tar.gz`, `1f8b-fake-macos-tar-${version}\n`],
    [`${productId}-${version}-linux-x64.AppImage`, `ELF-AI-fake-linux-appimage-${version}\n`],
    [`${productId}-${version}-linux-x64.tar.gz`, `1f8b-fake-linux-tar-${version}\n`],
    [`${productId}-${version}-android-arm64.apk`, `PK-fake-android-apk-${version}\n`],
    [`${productId}-${version}-android-arm64.aab`, `PK-fake-android-aab-${version}\n`],
  ];
  const expected = new Map();
  for (const [name, content] of specs) {
    const buf = Buffer.from(content, 'utf8');
    fs.writeFileSync(path.join(distDir, name), buf);
    expected.set(name, {
      size: buf.length,
      sha256: crypto.createHash('sha256').update(buf).digest('hex'),
    });
  }
  return expected;
}

function writeTestSignature(
  filePath,
  keyPair,
  version,
  timestamp = new Date().toISOString().replace(/\.\d{3}Z$/, 'Z')
) {
  const bytes = fs.readFileSync(filePath);
  const baseName = path.basename(filePath);
  const sigText = signBufferWithTestKey(bytes, keyPair, {
    untrustedComment: `signature for ${baseName}`,
    trustedComment: `release:${version}\ttimestamp:${timestamp}\tfile:${baseName}`,
  });
  fs.writeFileSync(`${filePath}.minisig`, sigText, { mode: 0o644 });
}

function testSourceNoticesAndBrandCleanliness(brand) {
  assert.equal(verifySourceLicenseNotices(repoRoot), true);

  // CLI --verify-source-licenses check
  execFileSync(process.execPath, [path.join(repoRoot, 'ci', 'generate-sbom.mjs'), '--verify-source-licenses'], {
    cwd: repoRoot,
    stdio: 'pipe',
  });

  const noticesText = fs.readFileSync(path.join(repoRoot, 'THIRD-PARTY-NOTICES.txt'), 'utf8');
  assert.match(noticesText, /Complete Corresponding Source Code \(GNU GPLv3 Section 6\)/);
  assert.ok(noticesText.includes(`https://github.com/${brand.githubOwner || brand.productId}/${brand.productId}`));
  assert.ok(noticesText.includes(brand.productUrl));
  assert.match(noticesText, /Linking & Relinking Information \(GNU LGPLv3 Section 4 \/ GNU GPLv3 Section 6\)/);
  assert.ok(noticesText.includes('Robomongo'), 'Robomongo attribution must be preserved');

  // Ensure zero legacy brand leaks across touched files and notices
  const checkedFiles = [
    'LICENSE',
    'LICENSE.LGPLv3',
    'COPYRIGHT',
    'THIRD-PARTY-NOTICES.txt',
    'vcpkg.json',
    'cmake/RagoShellLicenses.cmake',
    'ci/generate-sbom.mjs',
    'ci/build-android.sh',
    'ci/build-android.ps1',
    'docs/AndroidCodeSigning.md',
    'ci/test-sbom-and-licenses.mjs',
  ];
  for (const rel of checkedFiles) {
    const content = fs.readFileSync(path.join(repoRoot, rel), 'utf8');
    if (rel !== 'ci/test-sbom-and-licenses.mjs') {
      assert.equal(
        FORBIDDEN_OLD_BRAND_RE.test(content),
        false,
        `Forbidden pre-rename product name found in ${rel}`
      );
    }
  }

  // Ensure ci/generate-sbom.mjs never reads or sources ci/.env or any .env file
  const sbomScriptSource = fs.readFileSync(path.join(repoRoot, 'ci', 'generate-sbom.mjs'), 'utf8');
  const codeLines = sbomScriptSource
    .split(/\r?\n/)
    .filter((l) => !l.trim().startsWith('*') && !l.trim().startsWith('//'))
    .join('\n')
    .replace(/\bprocess\.env\b/g, 'PROCESS_ENV');
  assert.equal(/\.env\b/.test(codeLines), false, 'ci/generate-sbom.mjs must never access .env files');

  for (const androidScript of ['build-android.sh', 'build-android.ps1']) {
    const src = fs.readFileSync(path.join(repoRoot, 'ci', androidScript), 'utf8');
    const stripped = src
      .split(/\r?\n/)
      .filter((l) => !l.trim().startsWith('#'))
      .join('\n')
      .replace(/\bprocess\.env\b/g, 'PROCESS_ENV')
      .replace(/\$env:/gi, 'PS_ENV_')
      .replace(/Env:/g, 'PS_ENV_PROVIDER_')
      .replace(/:env\b/g, '_COLON_ENV')
      .replace(/\benv:/g, 'ARG_ENV_');
    assert.equal(/\.env\b/.test(stripped), false, `ci/${androidScript} must never read or source .env files`);
  }
}

function testCmakeLicensesInstallAndNegativeGuards(brand, tmpRoot) {
  const cmakeBin = resolveCmakeBinary();
  if (cmakeBin) {
    for (const isMac of [false, true]) {
      const label = isMac ? 'macos' : 'flat';
      const harnessSrc = path.join(tmpRoot, `cmake-lic-src-${label}`);
      const harnessBuild = path.join(tmpRoot, `cmake-lic-build-${label}`);
      const stageDir = path.join(tmpRoot, `cmake-lic-stage-${label}`);
      fs.mkdirSync(harnessSrc, { recursive: true });
      fs.mkdirSync(harnessBuild, { recursive: true });

      // Synthetic vcpkg_installed with valid ports + 0-byte port + symlink port (must skip invalid ones)
      const shareBase = path.join(harnessBuild, 'vcpkg_installed', 'x64-test', 'share');
      for (const port of ['openssl', 'mongo-cxx-driver', 'libssh2', 'zlib']) {
        const portDir = path.join(shareBase, port);
        fs.mkdirSync(portDir, { recursive: true });
        fs.writeFileSync(path.join(portDir, 'copyright'), `Copyright notice for ${port}\n`);
      }
      const emptyPortDir = path.join(shareBase, 'empty-port');
      fs.mkdirSync(emptyPortDir, { recursive: true });
      fs.writeFileSync(path.join(emptyPortDir, 'copyright'), '');

      const symlinkPortDir = path.join(shareBase, 'symlink-port');
      fs.mkdirSync(symlinkPortDir, { recursive: true });
      const outsideSecret = path.join(tmpRoot, `outside-${label}.txt`);
      fs.writeFileSync(outsideSecret, 'do-not-install');
      fs.symlinkSync(outsideSecret, path.join(symlinkPortDir, 'copyright'));

      const repoRootCMake = repoRoot.split(path.sep).join('/');
      const wrapperCmake = [
        'cmake_minimum_required(VERSION 3.20)',
        'project(LicenseBundleHarness LANGUAGES NONE)',
        `set(CMAKE_SOURCE_DIR "${repoRootCMake}")`,
        `set(SYSTEM_MACOSX ${isMac ? 'ON' : 'OFF'})`,
        'set(QJSON_VERSION 0.8.1)',
        'set(QJSON_DIR src/third-party/qjson-${QJSON_VERSION})',
        'set(QSCINTILLA_VERSION 2.8.4)',
        'set(QSCINTILLA_DIR src/third-party/qscintilla-${QSCINTILLA_VERSION})',
        'set(GOOGLE_TEST_VERSION 1.8.1)',
        'set(GOOGLE_TEST_DIR src/third-party/googletest-${GOOGLE_TEST_VERSION})',
        'set(VCPKG_TARGET_TRIPLET "x64-test")',
        `include("${repoRootCMake}/cmake/RagoShellDefaults.cmake")`,
        `include("${repoRootCMake}/cmake/RagoShellLicenses.cmake")`,
        '',
      ].join('\n');
      fs.writeFileSync(path.join(harnessSrc, 'CMakeLists.txt'), wrapperCmake);

      execFileSync(cmakeBin, ['-S', harnessSrc, '-B', harnessBuild], { stdio: 'pipe' });
      execFileSync(cmakeBin, ['--install', harnessBuild, '--component', 'licenses', '--prefix', stageDir], {
        stdio: 'pipe',
      });

      const expectedLicensesDir = isMac
        ? path.join(stageDir, `${brand.productName}.app`, 'Contents', 'Resources', 'licenses')
        : path.join(stageDir, 'licenses');

      const verified = verifyInstalledLicensesDir(expectedLicensesDir, {
        requireVcpkgCopyrights: true,
        requiredVcpkgPorts: ['openssl', 'mongo-cxx-driver', 'libssh2', 'zlib'],
      });
      assert.equal(verified.rootFiles.length, ROOT_LICENSE_FILES.length);
      assert.equal(verified.embeddedFiles.length, EMBEDDED_THIRD_PARTY_LICENSE_MAP.length);
      assert.deepEqual(verified.vcpkgCopyrightFiles, [
        'libssh2-COPYRIGHT',
        'mongo-cxx-driver-COPYRIGHT',
        'openssl-COPYRIGHT',
        'zlib-COPYRIGHT',
      ]);
      assert.equal(
        fs.existsSync(path.join(expectedLicensesDir, 'third-party', 'empty-port-COPYRIGHT')),
        false,
        '0-byte vcpkg copyright file must not be installed'
      );
      assert.equal(
        fs.existsSync(path.join(expectedLicensesDir, 'third-party', 'symlink-port-COPYRIGHT')),
        false,
        'Symlinked vcpkg copyright file must not be installed'
      );

      // CLI --verify-licenses check
      execFileSync(
        process.execPath,
        [
          path.join(repoRoot, 'ci', 'generate-sbom.mjs'),
          '--verify-licenses',
          expectedLicensesDir,
          '--require-vcpkg-copyrights',
        ],
        { cwd: repoRoot, stdio: 'pipe' }
      );
    }
  }

  // Negative verification checks on synthetic licenses/ tree
  const negDir = path.join(tmpRoot, 'licenses-negative');
  fs.mkdirSync(path.join(negDir, 'third-party'), { recursive: true });
  for (const f of ROOT_LICENSE_FILES) {
    fs.writeFileSync(path.join(negDir, f), fs.readFileSync(path.join(repoRoot, f)));
  }
  for (const item of EMBEDDED_THIRD_PARTY_LICENSE_MAP) {
    fs.writeFileSync(
      path.join(negDir, 'third-party', item.installedName),
      fs.readFileSync(path.join(repoRoot, item.relSource))
    );
  }

  // Passes when vcpkg copyrights are optional
  verifyInstalledLicensesDir(negDir, { requireVcpkgCopyrights: false });

  // Fails when vcpkg copyrights are required but missing
  assert.throws(
    () => verifyInstalledLicensesDir(negDir, { requireVcpkgCopyrights: true }),
    /Missing vcpkg copyright file/
  );

  // Fails when a 0-byte file is introduced inside licenses/third-party/
  const zeroFile = path.join(negDir, 'third-party', 'corrupt-COPYRIGHT');
  fs.writeFileSync(zeroFile, '');
  assert.throws(
    () => verifyInstalledLicensesDir(negDir, { requireVcpkgCopyrights: false }),
    /Empty \(0-byte\) license file/
  );
  fs.unlinkSync(zeroFile);

  // Fails when a symlink is placed inside licenses/third-party/
  const targetTxt = path.join(tmpRoot, 'ext-target.txt');
  fs.writeFileSync(targetTxt, 'external');
  const symPath = path.join(negDir, 'third-party', 'evil-link-COPYRIGHT');
  fs.symlinkSync(targetTxt, symPath);
  assert.throws(
    () => verifyInstalledLicensesDir(negDir, { requireVcpkgCopyrights: false }),
    /Refusing symbolic link inside licenses/
  );
  fs.unlinkSync(symPath);

  // Fails when a required root file is missing
  fs.unlinkSync(path.join(negDir, 'THIRD-PARTY-NOTICES.txt'));
  assert.throws(
    () => verifyInstalledLicensesDir(negDir, { requireVcpkgCopyrights: false }),
    /Missing required file/
  );
}

function testSpdxSbomGenerationDeterminismAndSchema(brand, tmpRoot) {
  const version = readDefaultCmakeVersion(repoRoot);
  const vcpkgSim = path.join(tmpRoot, 'vcpkg-sim');
  const distDir = path.join(tmpRoot, 'dist-sbom');
  const expectedDist = populateFakeDist(distDir, brand.productId, version);

  // Create synthetic vcpkg.spdx.json files (including libbson LicenseRef-vcpkg-null and openssl #2 port revision)
  const shareDir = path.join(vcpkgSim, 'x64-linux', 'share');
  fs.mkdirSync(path.join(shareDir, 'libbson'), { recursive: true });
  fs.writeFileSync(
    path.join(shareDir, 'libbson', 'vcpkg.spdx.json'),
    JSON.stringify({
      spdxVersion: 'SPDX-2.3',
      packages: [
        {
          name: 'libbson',
          SPDXID: 'SPDXRef-port',
          versionInfo: '1.30.7',
          licenseConcluded: 'LicenseRef-vcpkg-null',
          homepage: 'https://github.com/mongodb/mongo-c-driver/tree/master/src/libbson',
        },
        {
          SPDXID: 'SPDXRef-resource-0',
          downloadLocation: 'git+https://github.com/mongodb/mongo-c-driver@1.30.7',
          checksums: [{ algorithm: 'SHA512', checksumValue: 'a'.repeat(128) }],
        },
      ],
    })
  );

  fs.mkdirSync(path.join(shareDir, 'openssl'), { recursive: true });
  fs.writeFileSync(
    path.join(shareDir, 'openssl', 'vcpkg.spdx.json'),
    JSON.stringify({
      spdxVersion: 'SPDX-2.3',
      packages: [
        {
          name: 'openssl',
          SPDXID: 'SPDXRef-port',
          versionInfo: '3.6.5#2',
          licenseConcluded: 'Apache-2.0',
          homepage: 'https://www.openssl.org/',
        },
        {
          SPDXID: 'SPDXRef-resource-0',
          downloadLocation: 'git+https://github.com/openssl/openssl@openssl-3.6.5',
          checksums: [{ algorithm: 'SHA512', checksumValue: 'b'.repeat(128) }],
        },
      ],
    })
  );

  const out1 = path.join(tmpRoot, 'sbom-1.spdx.json');
  const out2 = path.join(tmpRoot, 'sbom-2.spdx.json');

  const res1 = generateSbom({
    repoRoot,
    version,
    distDir,
    vcpkgInstalledDir: vcpkgSim,
    autoDiscoverVcpkg: false,
    qtVersion: '6.12.0',
    created: '2026-11-02T00:00:00Z',
    outputPath: out1,
  });
  const res2 = generateSbom({
    repoRoot,
    version,
    distDir,
    vcpkgInstalledDir: vcpkgSim,
    autoDiscoverVcpkg: false,
    qtVersion: '6.12.0',
    created: '2026-11-02T00:00:00Z',
    outputPath: out2,
  });

  // 100% byte-for-byte determinism check
  assert.equal(res1.sbomJson, res2.sbomJson, 'SBOM generation must be byte-for-byte deterministic');
  assert.equal(res1.document.documentNamespace, res2.document.documentNamespace);

  const doc = res1.document;
  assert.equal(doc.spdxVersion, 'SPDX-2.3');
  assert.equal(doc.dataLicense, 'CC0-1.0');
  assert.equal(doc.SPDXID, 'SPDXRef-DOCUMENT');
  assert.equal(doc.name, `${brand.productId}-${version}`);
  assert.ok(doc.documentNamespace.startsWith(`https://dl.${brand.domain}/spdx/${brand.productId}-${version}-`));

  // Verify libbson LicenseRef-vcpkg-null was normalized to Apache-2.0
  const libbsonPkg = doc.packages.find((p) => p.name === 'libbson');
  assert.ok(libbsonPkg);
  assert.equal(libbsonPkg.versionInfo, '1.30.7');
  assert.equal(libbsonPkg.licenseConcluded, 'Apache-2.0');
  assert.equal(libbsonPkg.checksums?.[0]?.checksumValue, 'a'.repeat(128));

  // Verify openssl #2 port revision stripped from upstream versionInfo & CPE, kept in vcpkg purl
  const opensslPkg = doc.packages.find((p) => p.name === 'openssl');
  assert.ok(opensslPkg);
  assert.equal(opensslPkg.versionInfo, '3.6.5');
  assert.ok(
    opensslPkg.externalRefs.some(
      (r) => r.referenceType === 'purl' && r.referenceLocator.includes('pkg:vcpkg/openssl@3.6.5') && r.referenceLocator.includes('port_version=2')
    )
  );
  assert.ok(
    opensslPkg.externalRefs.some(
      (r) => r.referenceType === 'cpe23Type' && r.referenceLocator === 'cpe:2.3:a:openssl:openssl:3.6.5:*:*:*:*:*:*:*'
    )
  );

  // Verify mongo-cxx-driver override 4.0.0, embedded components, Qt 6 modules, and dist/ SHA-256 digests
  assert.equal(doc.packages.find((p) => p.name === 'mongo-cxx-driver')?.versionInfo, '4.0.0');
  assert.equal(doc.packages.find((p) => p.name === 'qscintilla')?.versionInfo, '2.8.4');
  assert.equal(doc.packages.find((p) => p.name === 'qjson')?.versionInfo, '0.8.1');
  assert.equal(doc.packages.find((p) => p.name === 'googletest')?.versionInfo, '1.8.1');
  assert.equal(doc.packages.find((p) => p.name === 'qt6-core')?.versionInfo, '6.12.0');

  for (const [artName, meta] of expectedDist.entries()) {
    const artPkg = doc.packages.find((p) => p.name === artName);
    assert.ok(artPkg, `Missing dist artifact ${artName} in SBOM packages`);
    assert.equal(artPkg.checksums?.[0]?.algorithm, 'SHA256');
    assert.equal(artPkg.checksums?.[0]?.checksumValue, meta.sha256);
    assert.ok(
      doc.relationships.some(
        (rel) =>
          rel.spdxElementId === artPkg.SPDXID &&
          rel.relationshipType === 'GENERATED_FROM' &&
          rel.relatedSpdxElement === `SPDXRef-Package-${brand.productId}`
      )
    );
  }

  // Verify on-disk SBOM with verifySbomFile
  const canonicalOut = path.join(distDir, `${brand.productId}-${version}-sbom.spdx.json`);
  fs.copyFileSync(out1, canonicalOut);
  const verified = verifySbomFile({ repoRoot, version, distDir });
  assert.equal(verified.document.packages.length, doc.packages.length);
}

function testNegativeSbomAndTamperGuards(brand, tmpRoot) {
  const version = readDefaultCmakeVersion(repoRoot);

  // 1. Bad SemVer rejected
  assert.throws(
    () => generateSbom({ repoRoot, version: '../evil', skipDist: true, writeFile: false }),
    /Invalid version/
  );

  // 2. Symlink in dist/ rejected
  const distSym = path.join(tmpRoot, 'dist-sbom-sym');
  populateFakeDist(distSym, brand.productId, version);
  const ext = path.join(tmpRoot, 'outside-sbom.bin');
  fs.writeFileSync(ext, 'secret');
  fs.symlinkSync(ext, path.join(distSym, `${brand.productId}-${version}-extra.zip`));
  assert.throws(
    () => generateSbom({ repoRoot, version, distDir: distSym, writeFile: false }),
    /Refusing symbolic link/
  );

  // 3. 0-byte artifact in dist/ rejected
  const distZero = path.join(tmpRoot, 'dist-sbom-zero');
  populateFakeDist(distZero, brand.productId, version);
  fs.writeFileSync(path.join(distZero, `${brand.productId}-${version}-macos-arm64.dmg`), '');
  assert.throws(
    () => generateSbom({ repoRoot, version, distDir: distZero, writeFile: false }),
    /empty \(0 bytes\)/
  );

  // 4. Tampered artifact after SBOM generation caught by verifySbomFile
  const distTamper = path.join(tmpRoot, 'dist-sbom-tamper');
  populateFakeDist(distTamper, brand.productId, version);
  generateSbom({ repoRoot, version, distDir: distTamper, created: '2026-11-02T00:00:00Z' });
  const dmgPath = path.join(distTamper, `${brand.productId}-${version}-macos-arm64.dmg`);
  const origDmg = fs.readFileSync(dmgPath);
  const flippedDmg = Buffer.from(origDmg);
  flippedDmg[0] ^= 0xff;
  fs.writeFileSync(dmgPath, flippedDmg);
  assert.throws(
    () => verifySbomFile({ repoRoot, version, distDir: distTamper }),
    /SBOM SHA-256 mismatch/
  );
  fs.writeFileSync(dmgPath, origDmg);
  verifySbomFile({ repoRoot, version, distDir: distTamper });

  // 5. Malformed SPDX schema mutations caught by validateSpdxDocument
  const { document: baseDoc } = generateSbom({
    repoRoot,
    version,
    skipDist: true,
    autoDiscoverVcpkg: false,
    created: '2026-11-02T00:00:00Z',
    writeFile: false,
  });

  const clone = () => JSON.parse(JSON.stringify(baseDoc));

  const badVerDoc = clone();
  badVerDoc.spdxVersion = 'SPDX-2.2';
  assert.throws(() => validateSpdxDocument(badVerDoc), /Unsupported spdxVersion/);

  const badNsDoc = clone();
  badNsDoc.documentNamespace = 'http://insecure.example/spdx/doc';
  assert.throws(() => validateSpdxDocument(badNsDoc), /must use https/);

  const badLicRefDoc = clone();
  badLicRefDoc.packages[0].licenseConcluded = 'LicenseRef-UndefinedCustom';
  assert.throws(() => validateSpdxDocument(badLicRefDoc), /undefined custom license/);

  const noPurlDoc = clone();
  noPurlDoc.packages[0].externalRefs = [];
  assert.throws(() => validateSpdxDocument(noPurlDoc), /at least one externalRef/);

  const danglingRelDoc = clone();
  danglingRelDoc.relationships.push({
    spdxElementId: 'SPDXRef-DOCUMENT',
    relationshipType: 'DESCRIBES',
    relatedSpdxElement: 'SPDXRef-DoesNotExist',
  });
  assert.throws(() => validateSpdxDocument(danglingRelDoc), /unknown relatedSpdxElement/);
}

async function testReleasePipelineSbomIntegration(brand, tmpRoot) {
  const version = '2.0.0'; // Present in CHANGELOG
  const distDir = path.join(tmpRoot, 'dist-e2e-sbom');
  populateFakeDist(distDir, brand.productId, version);

  // Step 1: Generate SBOM into dist/
  const sbomRes = generateSbom({
    repoRoot,
    version,
    distDir,
    autoDiscoverVcpkg: false,
    qtVersion: '6.12.0',
    created: '2026-11-02T00:00:00Z',
  });
  const sbomName = `${brand.productId}-${version}-sbom.spdx.json`;
  assert.equal(sbomRes.sbomFilename, sbomName);

  // Step 2: Generate releases.json + SHA256SUMS.txt
  const relRes = generateReleaseManifest({
    repoRoot,
    distDir,
    version,
    publishedAt: '2026-11-02',
  });
  assert.ok(relRes.manifest.sbom, 'manifest.sbom must be populated when .spdx.json is in dist/');
  assert.equal(relRes.manifest.sbom.name, sbomName);
  assert.equal(relRes.manifest.sbom.format, 'spdx-json');
  assert.equal(relRes.manifest.sbom.spdxVersion, 'SPDX-2.3');
  assert.equal(
    relRes.manifest.sbom.url,
    `https://dl.${brand.domain}/releases/${version}/${sbomName}`
  );
  assert.ok(relRes.checksumsText.includes(`  ${sbomName}\n`), 'SHA256SUMS.txt must include SBOM digest');
  assert.ok(relRes.markdownNotes.includes(`\`${sbomName}\``), 'Markdown notes must list SBOM checksum');

  // Verify tampering with SBOM on disk after manifest generation is caught by verifyReleaseDirectory
  const sbomPath = path.join(distDir, sbomName);
  const origSbomBytes = fs.readFileSync(sbomPath);
  fs.writeFileSync(sbomPath, `${origSbomBytes.toString('utf8')}\n`);
  assert.throws(
    () => verifyReleaseDirectory({ repoRoot, distDir, expectedVersion: version }),
    /SBOM .* byte size mismatch|SHA-256 digest mismatch/
  );
  fs.writeFileSync(sbomPath, origSbomBytes);
  verifyReleaseDirectory({ repoRoot, distDir, expectedVersion: version });

  // Step 3: Sign with ephemeral key and verify publish-release.sh --dry-run uploads SBOM to R2, GitHub, and Gitea
  const key = generateEphemeralTestKeyPair('sbom test key');
  writeTestSignature(path.join(distDir, 'releases.json'), key, version);
  writeTestSignature(path.join(distDir, 'SHA256SUMS.txt'), key, version);

  const dryRunOut = execFileSync(
    path.join(repoRoot, 'ci', 'publish-release.sh'),
    [
      '--dry-run',
      '--allow-test-pubkey',
      '--verify-node-only',
      '--skip-generate',
      '--dist',
      distDir,
      '--version',
      version,
      '--targets',
      'r2,github,gitea',
    ],
    {
      cwd: repoRoot,
      encoding: 'utf8',
      env: {
        ...process.env,
        RAGO_MINISIGN_PUBKEY: key.base64,
        R2_ACCOUNT_ID: '0123456789abcdef0123456789abcdef',
        R2_BUCKET: `${brand.productId}-releases`,
      },
    }
  );

  assert.ok(
    dryRunOut.includes(
      `s3://${brand.productId}-releases/releases/${version}/${sbomName} <- ${sbomName} [Cache-Control: public, max-age=31536000, immutable] [Content-Type: application/spdx+json; charset=utf-8]`
    ),
    'publish-release.sh must upload .spdx.json to R2 with immutable cache-control and application/spdx+json MIME type'
  );
  const aabName = `${brand.productId}-${version}-android-arm64.aab`;
  assert.ok(
    dryRunOut.includes(
      `s3://${brand.productId}-releases/releases/${version}/${aabName} <- ${aabName} [Cache-Control: public, max-age=31536000, immutable] [Content-Type: application/x-authorware-bin]`
    ),
    'publish-release.sh must upload Android .aab bundle to R2 with immutable cache-control'
  );
  const ghSection = dryRunOut.split('==> [dry-run] GitHub Releases target:')[1]?.split('==> [dry-run] Gitea Releases target:')[0] || '';
  const giteaSection = dryRunOut.split('==> [dry-run] Gitea Releases target:')[1] || '';
  assert.ok(ghSection.includes(`upload ${sbomName}`), 'GitHub Releases dry-run must attach SBOM');
  assert.ok(ghSection.includes(`upload ${aabName}`), 'GitHub Releases dry-run must attach Android .aab');
  assert.ok(giteaSection.includes(`upload ${sbomName}`), 'Gitea Releases dry-run must attach SBOM');
  assert.ok(giteaSection.includes(`upload ${aabName}`), 'Gitea Releases dry-run must attach Android .aab');

  // Step 4: Verify ragoshell-site primary download buttons and release-pulse-api remain 100% compatible
  const siblingSiteDir = path.resolve(repoRoot, '..', `${brand.productId}-site`);
  const siteReleasesTs = path.join(siblingSiteDir, 'src', 'lib', 'releases.ts');
  const siteConfigMjs = path.join(siblingSiteDir, 'site.config.mjs');
  if (fs.existsSync(siteReleasesTs) && fs.existsSync(siteConfigMjs)) {
    const { parseManifest } = await import(pathToFileURL(siteReleasesTs).href);
    const siteMod = await import(pathToFileURL(siteConfigMjs).href);
    const site = siteMod.default ?? siteMod.site;
    const parsed = parseManifest(relRes.manifest, site.download.allowedHosts);
    assert.ok(parsed !== null);
    assert.equal(parsed.downloads.windows?.url, relRes.manifest.downloads.windows.url);
    assert.equal(parsed.downloads.macos?.url, relRes.manifest.downloads.macos.url);
    assert.equal(parsed.downloads.linux?.url, relRes.manifest.downloads.linux.url);
  }

  const adminToken = 'sbom-pulse-admin-token-0123456789';
  const api = createReleasePulseServer({
    dbPath: ':memory:',
    releasesPath: path.join(distDir, 'releases.json'),
    persistReleases: false,
    adminToken,
  });
  await new Promise((resolve) => api.server.listen(0, '127.0.0.1', resolve));
  const baseUrl = `http://127.0.0.1:${api.server.address().port}`;
  try {
    const res = await fetch(`${baseUrl}/v1/admin/releases`, {
      method: 'POST',
      headers: {
        Authorization: `Bearer ${adminToken}`,
        'Content-Type': 'application/json',
      },
      body: JSON.stringify(relRes.manifest),
    });
    assert.equal(res.status, 200);
    const body = await res.json();
    assert.equal(body.manifest.sbom.name, sbomName);
    assert.equal(body.manifest.sbom.spdxVersion, 'SPDX-2.3');
  } finally {
    await api.close();
  }
}

async function main() {
  const brand = readBrandDefaults(repoRoot);
  const tmpRoot = makeTempDir(`${brand.productId}-sbom-lic-test-`);
  try {
    testSourceNoticesAndBrandCleanliness(brand);
    testCmakeLicensesInstallAndNegativeGuards(brand, tmpRoot);
    testSpdxSbomGenerationDeterminismAndSchema(brand, tmpRoot);
    testNegativeSbomAndTamperGuards(brand, tmpRoot);
    await testReleasePipelineSbomIntegration(brand, tmpRoot);
    process.stdout.write(
      `==> SBOM & license bundle test suite passed (${brand.productName}, CMake licenses + GPLv3 §6 + SPDX 2.3 SBOM + negative/tamper guards + release pipeline).\n`
    );
  } finally {
    fs.rmSync(tmpRoot, { recursive: true, force: true });
  }
}

await main();
