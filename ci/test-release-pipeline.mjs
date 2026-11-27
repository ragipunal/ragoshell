#!/usr/bin/env node
/**
 * Automated end-to-end & negative security test suite for Roadmap Phase 3.6:
 * - `ci/generate-releases-json.mjs` (manifest + SHA256SUMS.txt generation & verification)
 * - Cross-repo compatibility with `ragoshell-site` (`parseManifest`) and `services/release-pulse-api`
 * - Negative security tests (bad SemVer, HTTP/non-allowlisted/credential URLs, symlinks, 0-byte files,
 *   stale version artifacts, missing primary platforms, raw HTML notes, post-generation binary tampering)
 * - Minisign Ed25519 + BLAKE2b-512 signature & signed `trusted comment` positive + negative tests
 * - `ci/publish-release.sh --dry-run` atomic R2 upload order, Cache-Control headers, and live-mode guards
 * - Full `minisign` CLI end-to-end keygen + sign + `minisign -Vm` + `--dry-run` publish when `minisign` is installed
 */

import assert from 'node:assert/strict';
import { execFileSync, spawnSync } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import zlib from 'node:zlib';

import {
  compareSemVer,
  generateReleaseManifest,
  normalizeAndValidateVersion,
  readBrandDefaults,
  readDefaultCmakeVersion,
  validateAllowedHttpsUrl,
  verifyReleaseDirectory,
} from './generate-releases-json.mjs';
import { verifyReleaseTag } from './check-release-tag.mjs';
import {
  buildReleaseSyncUrls,
  normalizeStableReleaseTag,
  verifySignedManifestFiles,
} from './verify-remote-manifest.mjs';
import {
  generateEphemeralTestKeyPair,
  parseMinisignPublicKey,
  signBufferWithTestKey,
  verifyMinisigBuffer,
  verifyMinisigFile,
} from './lib/minisign.mjs';
import { createReleasePulseServer } from '../services/release-pulse-api/server.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(__dirname, '..');

function makeTempDir(prefix) {
  return fs.mkdtempSync(path.join(os.tmpdir(), prefix));
}

function populateFakeDist(distDir, productId, version, includeAndroid = true) {
  fs.mkdirSync(distDir, { recursive: true });
  const specs = [
    [`${productId}-${version}-windows-x64-setup.exe`, `MZ-fake-nsis-installer-${version}\n`],
    [`${productId}-${version}-windows-x64.msi`, `D0CF11E0-fake-wix-msi-${version}\n`],
    [`${productId}-${version}-windows-x64.zip`, `PK-fake-windows-portable-${version}\n`],
    [`${productId}-${version}-macos-arm64.dmg`, `koly-fake-macos-dmg-${version}\n`],
    [`${productId}-${version}-macos-arm64.tar.gz`, `1f8b-fake-macos-tar-${version}\n`],
    [`${productId}-${version}-linux-x64.AppImage`, `ELF-AI-fake-linux-appimage-${version}\n`],
    [`${productId}-${version}-linux-x64.tar.gz`, `1f8b-fake-linux-tar-${version}\n`],
  ];
  if (includeAndroid) {
    specs.push([`${productId}-${version}-android-arm64.apk`, `PK-fake-android-apk-${version}\n`]);
    specs.push([`${productId}-${version}-android-arm64.aab`, `PK-fake-android-aab-${version}\n`]);
  }
  const expected = new Map();
  for (const [name, content] of specs) {
    const buf = Buffer.from(content, 'utf8');
    const filePath = path.join(distDir, name);
    fs.writeFileSync(filePath, buf);
    expected.set(name, {
      size: buf.length,
      sha256: crypto.createHash('sha256').update(buf).digest('hex'),
    });
  }
  return expected;
}

function writeTestSignature(filePath, keyPair, version, timestamp = '2026-11-01T12:00:00Z') {
  const bytes = fs.readFileSync(filePath);
  const baseName = path.basename(filePath);
  const sigText = signBufferWithTestKey(bytes, keyPair, {
    untrustedComment: `signature for ${baseName}`,
    trustedComment: `release:${version}\ttimestamp:${timestamp}\tfile:${baseName}`,
  });
  fs.writeFileSync(`${filePath}.minisig`, sigText, { mode: 0o644 });
}

async function testManifestGenerationAndSchemaCompatibility(brand, tmpRoot) {
  const version = '2.0.0'; // Listed in repository CHANGELOG
  const distDir = path.join(tmpRoot, 'dist-valid');
  const expectedFiles = populateFakeDist(distDir, brand.productId, version, true);
  const notesMdPath = path.join(tmpRoot, 'RELEASE_NOTES.md');

  const playUrl = `https://play.google.com/store/apps/details?id=${brand.bundleId}`;
  const appStoreUrl = `https://apps.apple.com/app/${brand.productId}/id0000000000`;

  const result = generateReleaseManifest({
    repoRoot,
    distDir,
    version: `v${version}`,
    minSupportedVersion: '1.8.0',
    publishedAt: '2026-11-01',
    iosStoreUrl: appStoreUrl,
    notesTr: 'Linux Wayland/AppImage, macOS imzalama ve yayın hattı iyileştirmeleri.',
    markdownNotesPath: notesMdPath,
  });

  const { manifest } = result;
  assert.equal(manifest.schemaVersion, 1);
  assert.equal(manifest.latestVersion, version);
  assert.equal(manifest.minSupportedVersion, '1.8.0');
  assert.equal(manifest.mandatory, false);
  assert.equal(manifest.publishedAt, '2026-11-01');
  assert.equal(manifest.allDownloadsUrl, `https://dl.${brand.domain}/releases/`);
  assert.ok(manifest.releaseNotes.en.length > 10);
  assert.ok(manifest.releaseNotes.tr.includes('Linux'));

  // Primary desktop downloads MUST select -setup.exe, .dmg, and .AppImage
  assert.equal(
    manifest.downloads.windows.url,
    `https://dl.${brand.domain}/releases/${version}/${brand.productId}-${version}-windows-x64-setup.exe`
  );
  assert.equal(
    manifest.downloads.macos.url,
    `https://dl.${brand.domain}/releases/${version}/${brand.productId}-${version}-macos-arm64.dmg`
  );
  assert.equal(
    manifest.downloads.linux.url,
    `https://dl.${brand.domain}/releases/${version}/${brand.productId}-${version}-linux-x64.AppImage`
  );
  // Direct APK download when androidStoreUrl is omitted (.aab stays non-primary)
  assert.equal(
    manifest.downloads.android.url,
    `https://dl.${brand.domain}/releases/${version}/${brand.productId}-${version}-android-arm64.apk`
  );
  assert.equal(manifest.downloads.ios.url, appStoreUrl);

  const apkArtifact = manifest.artifacts.find((a) => a.name === `${brand.productId}-${version}-android-arm64.apk`);
  const aabArtifact = manifest.artifacts.find((a) => a.name === `${brand.productId}-${version}-android-arm64.aab`);
  assert.ok(apkArtifact && apkArtifact.format === 'apk' && apkArtifact.primary === true);
  assert.ok(aabArtifact && aabArtifact.format === 'aab' && aabArtifact.primary === false);

  // All 9 artifacts present with exact SHA-256 and byte sizes
  assert.equal(manifest.artifacts.length, expectedFiles.size);
  for (const art of manifest.artifacts) {
    const exp = expectedFiles.get(art.name);
    assert.ok(exp, `Unexpected artifact ${art.name}`);
    assert.equal(art.size, exp.size);
    assert.equal(art.sha256, exp.sha256);
  }

  // Verify SHA256SUMS.txt format and CLI --verify-only mode
  const sumsText = fs.readFileSync(path.join(distDir, 'SHA256SUMS.txt'), 'utf8');
  const sumLines = sumsText.trim().split('\n');
  assert.equal(sumLines.length, expectedFiles.size);
  for (const line of sumLines) {
    assert.match(line, /^[0-9a-f]{64}  [A-Za-z0-9._+-]+$/);
  }
  const verified = verifyReleaseDirectory({ repoRoot, distDir, expectedVersion: version });
  assert.equal(verified.manifest.latestVersion, version);
  assert.ok(fs.existsSync(notesMdPath));

  // Also test with Google Play store override for Android
  const distStore = path.join(tmpRoot, 'dist-store');
  populateFakeDist(distStore, brand.productId, version, true);
  const storeRes = generateReleaseManifest({
    repoRoot,
    distDir: distStore,
    version,
    publishedAt: '2026-11-01',
    androidStoreUrl: playUrl,
    iosStoreUrl: appStoreUrl,
  });
  assert.equal(storeRes.manifest.downloads.android.url, playUrl);
  assert.equal(storeRes.manifest.downloads.android.label, 'Google Play');

  // Cross-repo compatibility 1: ragoshell-site src/lib/releases.ts + site.config.mjs
  const siblingSiteDir = path.resolve(repoRoot, '..', `${brand.productId}-site`);
  const siteReleasesTs = path.join(siblingSiteDir, 'src', 'lib', 'releases.ts');
  const siteConfigMjs = path.join(siblingSiteDir, 'site.config.mjs');
  if (fs.existsSync(siteReleasesTs) && fs.existsSync(siteConfigMjs)) {
    const { parseManifest } = await import(pathToFileURL(siteReleasesTs).href);
    const siteMod = await import(pathToFileURL(siteConfigMjs).href);
    const site = siteMod.default ?? siteMod.site;

    for (const candidate of [manifest, storeRes.manifest]) {
      const parsedSite = parseManifest(candidate, site.download.allowedHosts);
      assert.ok(parsedSite !== null, 'ragoshell-site parseManifest rejected generated manifest');
      assert.equal(parsedSite.version, version);
      assert.equal(parsedSite.date, '2026-11-01');
      assert.equal(parsedSite.downloads.windows?.url, candidate.downloads.windows.url);
      assert.equal(parsedSite.downloads.macos?.url, candidate.downloads.macos.url);
      assert.equal(parsedSite.downloads.linux?.url, candidate.downloads.linux.url);
      assert.equal(parsedSite.downloads.android?.url, candidate.downloads.android.url);
      assert.equal(parsedSite.downloads.ios?.url, candidate.downloads.ios.url);
    }
  }

  // Cross-repo compatibility 2: services/release-pulse-api/server.mjs live HTTP round-trip
  const adminToken = 'test-release-pipeline-admin-token-0123456789';
  const api = createReleasePulseServer({
    dbPath: ':memory:',
    releasesPath: path.join(distDir, 'releases.json'),
    persistReleases: false,
    adminToken,
  });

  await new Promise((resolve) => api.server.listen(0, '127.0.0.1', resolve));
  const addr = api.server.address();
  const baseUrl = `http://127.0.0.1:${addr.port}`;

  try {
    const checkRes = await fetch(`${baseUrl}/v1/release-check`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        appVersion: '1.9.0',
        platform: 'macos',
        arch: 'arm64',
      }),
    });
    assert.equal(checkRes.status, 200);
    const checkBody = await checkRes.json();
    assert.equal(checkBody.schemaVersion, 1);
    assert.equal(checkBody.latestVersion, version);
    assert.equal(checkBody.minSupportedVersion, '1.8.0');
    assert.equal(checkBody.mandatory, false);
    assert.equal(checkBody.downloads.macos.sha256, manifest.downloads.macos.sha256);
    assert.equal(checkBody.downloads.macos.size, manifest.downloads.macos.size);
    assert.equal(checkBody.artifacts.length, manifest.artifacts.length);

    // Hot-reload generated manifest via POST /v1/admin/releases
    const adminRes = await fetch(`${baseUrl}/v1/admin/releases`, {
      method: 'POST',
      headers: {
        Authorization: `Bearer ${adminToken}`,
        'Content-Type': 'application/json',
      },
      body: JSON.stringify(storeRes.manifest),
    });
    assert.equal(adminRes.status, 200);
    const adminBody = await adminRes.json();
    assert.equal(adminBody.status, 'updated');
    assert.equal(adminBody.manifest.latestVersion, version);
  } finally {
    await api.close();
  }
}

function testNegativeManifestAndFilesystemGuards(brand, tmpRoot) {
  // 1. Invalid SemVer / traversal / leading zeros / script injection
  for (const badVer of ['', '../2.0.0', '2.0', '02.0.0', '2.0.0/evil', '2.0.0<script>', 'v']) {
    assert.throws(
      () => normalizeAndValidateVersion(badVer),
      /Invalid version/,
      `Expected bad version "${badVer}" to be rejected`
    );
  }

  // 1b. SemVer 2.0.0 numeric pre-release ordering (rc.10 > rc.2, stable > rc.10)
  assert.equal(compareSemVer('2.1.0-rc.10', '2.1.0-rc.2'), 1);
  assert.equal(compareSemVer('2.1.0-rc.2', '2.1.0-rc.10'), -1);
  assert.equal(compareSemVer('2.1.0', '2.1.0-rc.10'), 1);

  // 1c. Hyphenated pre-release identifiers keep every part (core = before the first '-', pre-release = the rest)
  assert.equal(compareSemVer('2.1.0-rc-2', '2.1.0-rc-1'), 1);
  assert.equal(compareSemVer('2.1.0-rc-1', '2.1.0-rc-2'), -1);
  assert.equal(compareSemVer('2.1.0-rc-1', '2.1.0-rc-1'), 0);
  assert.equal(compareSemVer('2.1.0-x-y-z.2', '2.1.0-x-y-z.10'), -1);
  assert.equal(normalizeAndValidateVersion('v2.1.0-rc-2'), '2.1.0-rc-2');

  // 1d. SemVer 2.0.0 §11 precedence: numeric < alphanumeric, shorter identifier set lower on an equal prefix,
  //     alphanumerics in ASCII order (the spec's example chain)
  const precedenceChain = [
    '1.0.0-alpha',
    '1.0.0-alpha.1',
    '1.0.0-alpha.beta',
    '1.0.0-beta',
    '1.0.0-beta.2',
    '1.0.0-beta.11',
    '1.0.0-rc.1',
    '1.0.0',
  ];
  for (let i = 0; i + 1 < precedenceChain.length; i += 1) {
    assert.equal(compareSemVer(precedenceChain[i], precedenceChain[i + 1]), -1, `${precedenceChain[i]} < ${precedenceChain[i + 1]}`);
    assert.equal(compareSemVer(precedenceChain[i + 1], precedenceChain[i]), 1, `${precedenceChain[i + 1]} > ${precedenceChain[i]}`);
  }
  assert.equal(compareSemVer('2.1.0-1', '2.1.0-a'), -1);

  // 1e. SemVer 2.0.0 §9: numeric pre-release identifiers must not have leading zeros; alphanumerics may start with 0
  for (const badPre of ['2.1.0-rc.01', '2.1.0-01', '2.1.0-rc.1.00', '2.1.0-', '2.1.0-rc..1', '2.1.0-rc.']) {
    assert.throws(
      () => normalizeAndValidateVersion(badPre),
      /Invalid version/,
      `Expected bad pre-release "${badPre}" to be rejected`
    );
  }
  for (const okPre of ['2.1.0-0', '2.1.0-rc.0', '2.1.0-0a', '2.1.0-rc.10', '2.1.0--', '2.1.0-0-1']) {
    assert.equal(normalizeAndValidateVersion(okPre), okPre, `Expected pre-release "${okPre}" to be accepted`);
  }

  // 2. Disallowed URL schemes, hosts, credentials, and path traversal
  for (const badUrl of [
    `http://dl.${brand.domain}/releases/2.0.0/a.dmg`,
    'https://evil.example.com/releases/2.0.0/a.dmg',
    `https://user:secret@dl.${brand.domain}/releases/2.0.0/a.dmg`,
    `https://dl.${brand.domain}/releases/../2.0.0/a.dmg`,
    'javascript:alert(1)',
  ]) {
    assert.throws(
      () => validateAllowedHttpsUrl(badUrl, brand.allowedHosts, 'testUrl'),
      /scheme must be https|allowed hosts|credentials|traversal|Invalid/,
      `Expected unsafe URL "${badUrl}" to be rejected`
    );
  }

  // 3. Unlisted version in CHANGELOG refused unless explicitly allowed
  const distUnlisted = path.join(tmpRoot, 'dist-unlisted');
  populateFakeDist(distUnlisted, brand.productId, '9.9.9');
  assert.throws(
    () => generateReleaseManifest({ repoRoot, distDir: distUnlisted, version: '9.9.9' }),
    /not found in .*CHANGELOG/
  );
  const distUnlistedRc = path.join(tmpRoot, 'dist-unlisted-rc');
  populateFakeDist(distUnlistedRc, brand.productId, '9.9.9-rc.1');
  assert.throws(
    () => generateReleaseManifest({ repoRoot, distDir: distUnlistedRc, version: '9.9.9-rc.1' }),
    /not found in .*CHANGELOG/
  );

  // 3b. Pre-release tag reuses matching base X.Y.Z section in CHANGELOG without allowUnlistedChangelog
  const distBaseRc = path.join(tmpRoot, 'dist-base-rc');
  populateFakeDist(distBaseRc, brand.productId, '2.1.0-rc.1');
  const rcManifest = generateReleaseManifest({ repoRoot, distDir: distBaseRc, version: '2.1.0-rc.1' });
  assert.equal(rcManifest.manifest.latestVersion, '2.1.0-rc.1');

  // 3c. Strict release tag verifier matches SemVer 2.0.0, CMakeLists.txt PROJECT_VERSION, and CHANGELOG
  const currentCmakeVer = readDefaultCmakeVersion(repoRoot).replace(/-.*$/, '');
  const stableTagInfo = verifyReleaseTag({ repoRoot, tag: `v${currentCmakeVer}` });
  assert.equal(stableTagInfo.isPrerelease, false);
  const rcTagInfo = verifyReleaseTag({ repoRoot, tag: `v${currentCmakeVer}-rc.1` });
  assert.equal(rcTagInfo.isPrerelease, true);
  assert.equal(rcTagInfo.changelogVersion, currentCmakeVer);
  for (const badTag of [
    '',
    currentCmakeVer,
    'v2.1',
    'v99.0.0',
    `v${currentCmakeVer}-rc.01`,
    `v${currentCmakeVer}-foo.1`,
    'widgets-final',
  ]) {
    assert.throws(
      () => verifyReleaseTag({ repoRoot, tag: badTag }),
      /required|Invalid release tag|does not match/,
      `Expected release tag "${badTag}" to be rejected`
    );
  }

  // 4. minSupportedVersion > latestVersion refused
  const distMinGt = path.join(tmpRoot, 'dist-mingt');
  populateFakeDist(distMinGt, brand.productId, '2.0.0');
  assert.throws(
    () =>
      generateReleaseManifest({
        repoRoot,
        distDir: distMinGt,
        version: '2.0.0',
        minSupportedVersion: '2.1.0',
      }),
    /cannot be greater than latestVersion/
  );

  // 5. Raw HTML tags in releaseNotes refused
  assert.throws(
    () =>
      generateReleaseManifest({
        repoRoot,
        distDir: distMinGt,
        version: '2.0.0',
        notesEn: 'Fixes <script>alert(1)</script> injection',
      }),
    /raw HTML markup is forbidden/
  );

  // 6. Empty (0-byte) artifact refused
  const distZero = path.join(tmpRoot, 'dist-zero');
  populateFakeDist(distZero, brand.productId, '2.0.0');
  fs.writeFileSync(path.join(distZero, `${brand.productId}-2.0.0-linux-x64.AppImage`), '');
  assert.throws(
    () => generateReleaseManifest({ repoRoot, distDir: distZero, version: '2.0.0' }),
    /empty \(0 bytes\)/
  );

  // 7. Symbolic link in dist/ refused (both artifact names and metadata names like releases.json)
  const distSymlink = path.join(tmpRoot, 'dist-symlink');
  populateFakeDist(distSymlink, brand.productId, '2.0.0');
  const outsideFile = path.join(tmpRoot, 'outside.txt');
  fs.writeFileSync(outsideFile, 'secret');
  fs.symlinkSync(outsideFile, path.join(distSymlink, `${brand.productId}-2.0.0-extra.zip`));
  assert.throws(
    () => generateReleaseManifest({ repoRoot, distDir: distSymlink, version: '2.0.0' }),
    /Refusing symbolic link/
  );
  const distMetaSymlink = path.join(tmpRoot, 'dist-meta-symlink');
  populateFakeDist(distMetaSymlink, brand.productId, '2.0.0');
  fs.symlinkSync(outsideFile, path.join(distMetaSymlink, 'releases.json'));
  assert.throws(
    () => generateReleaseManifest({ repoRoot, distDir: distMetaSymlink, version: '2.0.0' }),
    /Refusing symbolic link/
  );

  // 8. Stale artifact from another version mixed in dist/ refused
  const distStale = path.join(tmpRoot, 'dist-stale');
  populateFakeDist(distStale, brand.productId, '2.0.0');
  fs.writeFileSync(path.join(distStale, `${brand.productId}-1.9.0-linux-x64.AppImage`), 'stale');
  assert.throws(
    () => generateReleaseManifest({ repoRoot, distDir: distStale, version: '2.0.0' }),
    /does not match target release prefix/
  );

  // 9. Missing mandatory primary desktop platform (.dmg deleted) refused
  const distMissingMac = path.join(tmpRoot, 'dist-missing-mac');
  populateFakeDist(distMissingMac, brand.productId, '2.0.0');
  fs.unlinkSync(path.join(distMissingMac, `${brand.productId}-2.0.0-macos-arm64.dmg`));
  assert.throws(
    () => generateReleaseManifest({ repoRoot, distDir: distMissingMac, version: '2.0.0' }),
    /Missing required primary release download for platform "macos"/
  );

  // 10. Post-generation binary tamper & SHA256SUMS tamper caught by verifyReleaseDirectory
  const distTamper = path.join(tmpRoot, 'dist-tamper');
  populateFakeDist(distTamper, brand.productId, '2.0.0');
  generateReleaseManifest({ repoRoot, distDir: distTamper, version: '2.0.0' });

  const appImagePath = path.join(distTamper, `${brand.productId}-2.0.0-linux-x64.AppImage`);
  const origBytes = fs.readFileSync(appImagePath);
  const flipped = Buffer.from(origBytes);
  flipped[0] ^= 0xff; // Same byte length, different SHA-256
  fs.writeFileSync(appImagePath, flipped);
  assert.throws(
    () => verifyReleaseDirectory({ repoRoot, distDir: distTamper, expectedVersion: '2.0.0' }),
    /SHA-256 digest mismatch/
  );
  fs.writeFileSync(appImagePath, origBytes);
  verifyReleaseDirectory({ repoRoot, distDir: distTamper, expectedVersion: '2.0.0' });
}

function testMinisignCryptographicAndCommentGuards(brand, tmpRoot) {
  const version = '2.0.0';
  const distDir = path.join(tmpRoot, 'dist-minisign');
  populateFakeDist(distDir, brand.productId, version);
  generateReleaseManifest({ repoRoot, distDir, version, publishedAt: '2026-11-01' });

  const keyA = generateEphemeralTestKeyPair('test key A');
  const keyB = generateEphemeralTestKeyPair('test key B');
  const parsedPubA = parseMinisignPublicKey(keyA.pubFileText);
  assert.equal(parsedPubA.base64, keyA.base64);

  const manifestPath = path.join(distDir, 'releases.json');
  const sumsPath = path.join(distDir, 'SHA256SUMS.txt');
  writeTestSignature(manifestPath, keyA, version, '2026-11-01T12:00:00Z');
  writeTestSignature(sumsPath, keyA, version, '2026-11-01T12:00:00Z');

  // Positive verification (both direct library call and sign-release.sh --verify-only --verify-node-only)
  const okRes = verifyMinisigFile(manifestPath, `${manifestPath}.minisig`, keyA.base64, {
    expectedVersion: version,
    expectedFile: 'releases.json',
    nowMs: Date.parse('2026-11-01T12:05:00Z'),
  });
  assert.equal(okRes.ok, true);
  assert.equal(okRes.tokens.release, version);

  execFileSync(
    path.join(repoRoot, 'ci', 'sign-release.sh'),
    ['--verify-only', '--allow-test-pubkey', '--verify-node-only', '--dist', distDir, '--version', version],
    {
      cwd: repoRoot,
      env: { ...process.env, RAGO_MINISIGN_PUBKEY: keyA.base64 },
      stdio: 'pipe',
    }
  );

  const mismatchRes = spawnSync(
    path.join(repoRoot, 'ci', 'sign-release.sh'),
    ['--verify-only', '--verify-node-only', '--dist', distDir, '--version', version],
    {
      cwd: repoRoot,
      encoding: 'utf8',
      env: { ...process.env, RAGO_MINISIGN_PUBKEY: keyA.base64 },
    }
  );
  if (!mismatchRes.error) {
    assert.notEqual(mismatchRes.status, 0);
    assert.match(mismatchRes.stderr, /does not match RAGO_MINISIGN_PUBKEY/);
  }

  const manifestBytes = fs.readFileSync(manifestPath);
  const validSigText = fs.readFileSync(`${manifestPath}.minisig`, 'utf8');

  // Negative 1: 1-byte modification in releases.json
  const tamperedBytes = Buffer.from(manifestBytes);
  tamperedBytes[tamperedBytes.length - 2] ^= 0x01;
  assert.throws(
    () =>
      verifyMinisigBuffer(tamperedBytes, validSigText, keyA.base64, {
        expectedVersion: version,
        expectedFile: 'releases.json',
        checkClockSkew: false,
      }),
    /Ed25519 file signature verification failed/
  );

  // Negative 2: Forged trusted comment without re-signing global_sig
  const forgedCommentSig = validSigText.replace('release:2.0.0', 'release:2.1.0');
  assert.throws(
    () =>
      verifyMinisigBuffer(manifestBytes, forgedCommentSig, keyA.base64, {
        expectedVersion: '2.1.0',
        expectedFile: 'releases.json',
        checkClockSkew: false,
      }),
    /global signature over trusted comment failed/
  );

  // Negative 3: Validly signed older release replayed for newer expectedVersion
  const replaySig = signBufferWithTestKey(manifestBytes, keyA, {
    trustedComment: 'release:1.9.0\ttimestamp:2026-10-01T00:00:00Z\tfile:releases.json',
  });
  assert.throws(
    () =>
      verifyMinisigBuffer(manifestBytes, replaySig, keyA.base64, {
        expectedVersion: '2.0.0',
        expectedFile: 'releases.json',
        checkClockSkew: false,
      }),
    /Trusted comment release mismatch/
  );

  // Negative 4: Cross-file signature swap (SHA256SUMS.txt.minisig used for releases.json, or wrong file claim)
  const wrongFileClaimSig = signBufferWithTestKey(manifestBytes, keyA, {
    trustedComment: 'release:2.0.0\ttimestamp:2026-11-01T12:00:00Z\tfile:SHA256SUMS.txt',
  });
  assert.throws(
    () =>
      verifyMinisigBuffer(manifestBytes, wrongFileClaimSig, keyA.base64, {
        expectedVersion: '2.0.0',
        expectedFile: 'releases.json',
        checkClockSkew: false,
      }),
    /Trusted comment file mismatch/
  );

  // Negative 5: Missing timestamp claim
  const noTsSig = signBufferWithTestKey(manifestBytes, keyA, {
    trustedComment: 'release:2.0.0\tfile:releases.json',
  });
  assert.throws(
    () =>
      verifyMinisigBuffer(manifestBytes, noTsSig, keyA.base64, {
        expectedVersion: '2.0.0',
        expectedFile: 'releases.json',
        checkClockSkew: false,
      }),
    /missing valid UTC ISO-8601 "timestamp:/
  );

  // Negative 6: Future timestamp beyond allowed clock skew
  assert.throws(
    () =>
      verifyMinisigBuffer(manifestBytes, validSigText, keyA.base64, {
        expectedVersion: '2.0.0',
        expectedFile: 'releases.json',
        nowMs: Date.parse('2026-11-01T10:00:00Z'),
        maxFutureSkewSec: 300,
      }),
    /is in the future/
  );

  // Negative 7: Wrong public key
  assert.throws(
    () =>
      verifyMinisigBuffer(manifestBytes, validSigText, keyB.base64, {
        expectedVersion: '2.0.0',
        expectedFile: 'releases.json',
        checkClockSkew: false,
      }),
    /key ID mismatch/
  );

  // Negative 8: Duplicate key in trusted comment (ambiguous key:value injection) refused
  const dupKeySig = signBufferWithTestKey(manifestBytes, keyA, {
    trustedComment: 'release:1.0.0\trelease:2.0.0\ttimestamp:2026-11-01T12:00:00Z\tfile:releases.json',
  });
  assert.throws(
    () =>
      verifyMinisigBuffer(manifestBytes, dupKeySig, keyA.base64, {
        expectedVersion: '2.0.0',
        expectedFile: 'releases.json',
        checkClockSkew: false,
      }),
    /Duplicate claim key "release"/
  );

  // Negative 9: Legacy unprehashed "Ed" header mutation refused (prehashed "ED" required)
  const sigLines = validSigText.trim().split('\n');
  const rawSigBin = Buffer.from(sigLines[1], 'base64');
  rawSigBin.write('Ed', 0, 2, 'ascii');
  const legacyAlgoSig = [sigLines[0], rawSigBin.toString('base64'), sigLines[2], sigLines[3]].join('\n') + '\n';
  assert.throws(
    () =>
      verifyMinisigBuffer(manifestBytes, legacyAlgoSig, keyA.base64, {
        expectedVersion: '2.0.0',
        expectedFile: 'releases.json',
        checkClockSkew: false,
      }),
    /only BLAKE2b-512 prehashed "ED" is accepted/
  );

  return { distDir, version, keyA };
}

function testPublishDryRunAndLiveGuards(brand, signedFixture) {
  const { distDir, version, keyA } = signedFixture;
  const publishScript = path.join(repoRoot, 'ci', 'publish-release.sh');

  // Guard 1: --skip-sign without --dry-run MUST fail immediately
  const badSkipSign = spawnSync(publishScript, ['--skip-sign', '--dist', distDir, '--version', version], {
    cwd: repoRoot,
    encoding: 'utf8',
  });
  assert.notEqual(badSkipSign.status, 0);
  assert.match(badSkipSign.stderr, /--skip-sign is strictly forbidden outside --dry-run/);

  // Guard 2: --verify-node-only without --dry-run MUST fail immediately
  const badNodeOnly = spawnSync(
    publishScript,
    ['--verify-node-only', '--dist', distDir, '--version', version],
    {
      cwd: repoRoot,
      encoding: 'utf8',
    }
  );
  assert.notEqual(badNodeOnly.status, 0);
  assert.match(badNodeOnly.stderr, /--verify-node-only is strictly forbidden outside --dry-run/);

  // Guard 3: Leading-dash SSH target option injection MUST fail immediately
  const badSshOpt = spawnSync(
    publishScript,
    ['--dry-run', '--allow-test-pubkey', '--verify-node-only', '--skip-generate', '--dist', distDir, '--version', version, '--targets', 'site'],
    {
      cwd: repoRoot,
      encoding: 'utf8',
      env: {
        ...process.env,
        RAGO_MINISIGN_PUBKEY: keyA.base64,
        RAGO_SITE_RELEASES_SSH: '-oProxyCommand=evil@host',
      },
    }
  );
  assert.notEqual(badSshOpt.status, 0);
  assert.match(badSshOpt.stderr, /invalid RAGO_SITE_RELEASES_SSH/);

  // Guard 4: Non-Cloudflare R2_ENDPOINT_URL MUST fail without --allow-custom-endpoints
  const badR2Endpoint = spawnSync(
    publishScript,
    ['--dry-run', '--allow-test-pubkey', '--verify-node-only', '--skip-generate', '--dist', distDir, '--version', version, '--targets', 'r2'],
    {
      cwd: repoRoot,
      encoding: 'utf8',
      env: {
        ...process.env,
        RAGO_MINISIGN_PUBKEY: keyA.base64,
        R2_ENDPOINT_URL: 'https://attacker.example',
      },
    }
  );
  assert.notEqual(badR2Endpoint.status, 0);
  assert.match(badR2Endpoint.stderr, /must be an official Cloudflare R2 S3 endpoint/);

  // Dry-run across all 5 targets (r2, github, gitea, pulse, site)
  const dryRunOut = execFileSync(
    publishScript,
    [
      '--dry-run',
      '--allow-test-pubkey',
      '--verify-node-only',
      '--skip-generate',
      '--dist',
      distDir,
      '--version',
      `v${version}`,
      '--targets',
      'r2,github,gitea,pulse,site',
    ],
    {
      cwd: repoRoot,
      encoding: 'utf8',
      env: {
        ...process.env,
        RAGO_MINISIGN_PUBKEY: keyA.base64,
        R2_ACCOUNT_ID: '0123456789abcdef0123456789abcdef',
        R2_BUCKET: `${brand.productId}-releases`,
        PULSE_API_ADMIN_URL: `https://api.${brand.domain}/v1/admin/releases`,
        RAGO_SITE_RELEASES_SSH: 'deploy@infra.example.internal',
      },
    }
  );

  // Verify R2 atomic upload ordering & Cache-Control headers:
  // 1) Versioned artifacts + SHA256SUMS + archived manifest use max-age=31536000, immutable
  // 2) Root releases.json.minisig comes AFTER versioned objects and BEFORE root releases.json, with max-age=300
  const idxVersionedAppImage = dryRunOut.indexOf(
    `s3://${brand.productId}-releases/releases/${version}/${brand.productId}-${version}-linux-x64.AppImage <- ${brand.productId}-${version}-linux-x64.AppImage [Cache-Control: public, max-age=31536000, immutable]`
  );
  const idxVersionedSumsSig = dryRunOut.indexOf(
    `s3://${brand.productId}-releases/releases/${version}/SHA256SUMS.txt.minisig <- SHA256SUMS.txt.minisig [Cache-Control: public, max-age=31536000, immutable]`
  );
  const idxRootSig = dryRunOut.indexOf(
    `s3://${brand.productId}-releases/releases.json.minisig <- releases.json.minisig [Cache-Control: public, max-age=300]`
  );
  const idxRootManifest = dryRunOut.indexOf(
    `s3://${brand.productId}-releases/releases.json <- releases.json [Cache-Control: public, max-age=300]`
  );

  assert.ok(idxVersionedAppImage >= 0, 'Missing immutable versioned AppImage R2 upload in dry-run plan');
  assert.ok(idxVersionedSumsSig > idxVersionedAppImage, 'Missing immutable SHA256SUMS.txt.minisig R2 upload');
  assert.ok(idxRootSig > idxVersionedSumsSig, 'Root releases.json.minisig must be uploaded AFTER versioned R2 objects');
  assert.ok(idxRootManifest > idxRootSig, 'Root releases.json must be uploaded LAST (after releases.json.minisig)');

  // Verify GitHub & Gitea release steps appear BEFORE root R2 manifest switchover
  const idxGitHub = dryRunOut.indexOf('==> [dry-run] GitHub Releases target:');
  const idxGitea = dryRunOut.indexOf('==> [dry-run] Gitea Releases target:');
  assert.ok(idxGitHub > idxVersionedSumsSig && idxGitHub < idxRootSig, 'GitHub release should precede root R2 switchover');
  assert.ok(idxGitea > idxGitHub && idxGitea < idxRootSig, 'Gitea release should precede root R2 switchover');
  assert.match(dryRunOut, /\[dry-run\] release-pulse-api hook: POST https:\/\/api\..*\/v1\/admin\/releases/);
  assert.match(
    dryRunOut,
    /\[dry-run\] static site hook: stream releases\.json \+ releases\.json\.minisig -> ssh -- deploy@infra\.example\.internal releases/
  );
  assert.doesNotMatch(dryRunOut, /skipped/, 'A stable release must not skip the root manifest or the hooks');
}

// A pre-release (v2.1.0-rc.1) publishes only releases/<version>/ and the forge releases: the root releases.json and
// its signature (read by every stable client) and the pulse/site hooks are skipped, and the skip is printed.
function testPublishPrereleaseSkipsRootManifest(brand, tmpRoot, keyA) {
  const version = '2.1.0-rc.1';
  const distDir = path.join(tmpRoot, 'dist-prerelease');
  populateFakeDist(distDir, brand.productId, version);
  generateReleaseManifest({
    repoRoot,
    distDir,
    version,
    publishedAt: '2026-11-01',
    allowUnlistedChangelog: true,
    notesEn: 'Release candidate for the pre-release dry-run test.',
  });
  writeTestSignature(path.join(distDir, 'releases.json'), keyA, version, '2026-11-01T12:00:00Z');
  writeTestSignature(path.join(distDir, 'SHA256SUMS.txt'), keyA, version, '2026-11-01T12:00:00Z');

  const out = execFileSync(
    path.join(repoRoot, 'ci', 'publish-release.sh'),
    [
      '--dry-run',
      '--allow-test-pubkey',
      '--verify-node-only',
      '--skip-generate',
      '--dist',
      distDir,
      '--version',
      `v${version}`,
      '--targets',
      'r2,github,gitea,pulse,site',
    ],
    {
      cwd: repoRoot,
      encoding: 'utf8',
      env: {
        ...process.env,
        RAGO_MINISIGN_PUBKEY: keyA.base64,
        R2_ACCOUNT_ID: '0123456789abcdef0123456789abcdef',
        R2_BUCKET: `${brand.productId}-releases`,
        PULSE_API_ADMIN_URL: `https://api.${brand.domain}/v1/admin/releases`,
        RAGO_SITE_RELEASES_SSH: 'deploy@infra.example.internal',
      },
    }
  );

  const bucket = `s3://${brand.productId}-releases`;
  // versioned objects, including the archived manifest and its signature, are still published
  assert.ok(
    out.includes(
      `${bucket}/releases/${version}/${brand.productId}-${version}-linux-x64.AppImage <- ${brand.productId}-${version}-linux-x64.AppImage [Cache-Control: public, max-age=31536000, immutable]`
    ),
    'Pre-release must still upload its versioned artifacts'
  );
  assert.ok(
    out.includes(`${bucket}/releases/${version}/releases.json.minisig <- releases.json.minisig`),
    'Pre-release must still upload its versioned releases.json.minisig'
  );
  assert.match(out, /\[dry-run\] GitHub Releases target: .*prerelease=true/);
  assert.match(out, /\[dry-run\] Gitea Releases target: .*prerelease=true/);
  // the root manifest and signature are never written, the hooks never run, and each skip is printed
  assert.ok(!out.includes(`${bucket}/releases.json <-`), 'Pre-release must not overwrite the root releases.json');
  assert.ok(!out.includes(`${bucket}/releases.json.minisig <-`), 'Pre-release must not overwrite the root signature');
  assert.doesNotMatch(out, /release-pulse-api hook: POST/);
  assert.doesNotMatch(out, /static site hook: stream/);
  assert.match(out, /Step 4\/4: skipped for pre-release 2\.1\.0-rc\.1: root releases\.json\.minisig \+ releases\.json are NOT updated/);
  assert.match(out, /skipped release-pulse-api hook for pre-release 2\.1\.0-rc\.1/);
  assert.match(out, /skipped static site hook for pre-release 2\.1\.0-rc\.1/);

  // --stable cannot turn a pre-release version into a root manifest update
  const forcedStable = execFileSync(
    path.join(repoRoot, 'ci', 'publish-release.sh'),
    ['--dry-run', '--allow-test-pubkey', '--verify-node-only', '--skip-generate', '--stable', '--dist', distDir,
      '--version', version, '--targets', 'r2'],
    {
      cwd: repoRoot,
      encoding: 'utf8',
      env: {
        ...process.env,
        RAGO_MINISIGN_PUBKEY: keyA.base64,
        R2_ACCOUNT_ID: '0123456789abcdef0123456789abcdef',
        R2_BUCKET: `${brand.productId}-releases`,
      },
    }
  );
  assert.ok(!forcedStable.includes(`${bucket}/releases.json <-`), '--stable must not publish a pre-release root manifest');
  assert.match(forcedStable, /Step 4\/4: skipped for pre-release/);
}

function testRealMinisignCliEndToEndIfInstalled(brand, tmpRoot) {
  const probe = spawnSync('minisign', ['-v'], { encoding: 'utf8' });
  if (probe.error || probe.status !== 0) {
    process.stdout.write(
      '==> [info] `minisign` CLI binary not found on PATH; pure-Node Ed25519 + BLAKE2b-512 suite verified (install `minisign` to also exercise CLI keygen/signing).\n'
    );
    return;
  }

  const version = '2.0.0';
  const cliDir = path.join(tmpRoot, 'minisign-cli-e2e');
  const distDir = path.join(cliDir, 'dist');
  populateFakeDist(distDir, brand.productId, version);
  generateReleaseManifest({ repoRoot, distDir, version, publishedAt: '2026-11-01' });

  const pubPath = path.join(cliDir, 'test-release.pub');
  const secPath = path.join(cliDir, 'test-release.key');
  execFileSync('minisign', ['-G', '-W', '-p', pubPath, '-s', secPath], { stdio: 'pipe' });
  const pubParsed = parseMinisignPublicKey(fs.readFileSync(pubPath, 'utf8'));

  // End-to-end sign + verify using ci/sign-release.sh (invokes real `minisign -Sm` and `minisign -Vm`)
  execFileSync(
    path.join(repoRoot, 'ci', 'sign-release.sh'),
    ['--allow-test-pubkey', '--dist', distDir, '--version', version],
    {
      cwd: repoRoot,
      env: {
        ...process.env,
        RAGO_MINISIGN_SECRET_KEY_FILE: secPath,
        RAGO_MINISIGN_PUBKEY: pubParsed.base64,
      },
      stdio: 'pipe',
    }
  );

  assert.ok(fs.existsSync(path.join(distDir, 'releases.json.minisig')));
  assert.ok(fs.existsSync(path.join(distDir, 'SHA256SUMS.txt.minisig')));

  // Tamper test against real `minisign -Vm`
  const manifestPath = path.join(distDir, 'releases.json');
  const orig = fs.readFileSync(manifestPath);
  fs.writeFileSync(manifestPath, Buffer.concat([orig, Buffer.from('\n')]));
  const tamperCheck = spawnSync(
    path.join(repoRoot, 'ci', 'sign-release.sh'),
    ['--verify-only', '--allow-test-pubkey', '--dist', distDir, '--version', version],
    {
      cwd: repoRoot,
      env: { ...process.env, RAGO_MINISIGN_PUBKEY: pubParsed.base64 },
      encoding: 'utf8',
    }
  );
  assert.notEqual(tamperCheck.status, 0, 'minisign -Vm must reject tampered releases.json');
  fs.writeFileSync(manifestPath, orig);

  // Full publish-release.sh --dry-run WITHOUT --verify-node-only (uses real minisign CLI)
  execFileSync(
    path.join(repoRoot, 'ci', 'publish-release.sh'),
    ['--dry-run', '--allow-test-pubkey', '--dist', distDir, '--version', version, '--targets', 'r2,github,gitea'],
    {
      cwd: repoRoot,
      env: {
        ...process.env,
        RAGO_MINISIGN_SECRET_KEY: fs.readFileSync(secPath, 'utf8'),
        RAGO_MINISIGN_PUBKEY: pubParsed.base64,
        R2_ACCOUNT_ID: '0123456789abcdef0123456789abcdef',
      },
      stdio: 'pipe',
    }
  );

  // Also test scrypt password-encrypted (`Sc`) secret key signing via PTY when python3 is present
  const pyProbe = spawnSync('python3', ['--version'], { encoding: 'utf8' });
  if (!pyProbe.error && pyProbe.status === 0) {
    const encPubPath = path.join(cliDir, 'enc-release.pub');
    const encSecPath = path.join(cliDir, 'enc-release.key');
    const testPass = 'ephemeral-test-passphrase-3.6!';
    execFileSync(
      'python3',
      [
        '-c',
        `import errno, fcntl, os, pty, select, subprocess, sys, termios
pub, sec, pw = sys.argv[1], sys.argv[2], os.environ["TEST_PW"]
m, s = pty.openpty()
def setup():
    os.setsid()
    fcntl.ioctl(0, termios.TIOCSCTTY, 0)
p = subprocess.Popen(["minisign", "-G", "-p", pub, "-s", sec], stdin=s, stdout=s, stderr=s, preexec_fn=setup, close_fds=True)
os.close(s)
sent = 0
buf = b""
while True:
    r, _, _ = select.select([m], [], [], 30.0)
    if not r:
        p.kill()
        raise RuntimeError("Timed out in minisign -G")
    try:
        chunk = os.read(m, 4096)
    except OSError as exc:
        if exc.errno == errno.EIO:
            break
        raise
    if not chunk:
        break
    buf += chunk
    if sent == 0 and b"Password:" in buf:
        os.write(m, (pw + "\\n").encode("utf-8"))
        sent = 1
    if sent == 1 and b"one more time" in buf:
        os.write(m, (pw + "\\n").encode("utf-8"))
        sent = 2
os.close(m)
sys.exit(p.wait())`,
        encPubPath,
        encSecPath,
      ],
      { env: { ...process.env, TEST_PW: testPass }, stdio: 'pipe' }
    );
    const encPubParsed = parseMinisignPublicKey(fs.readFileSync(encPubPath, 'utf8'));
    execFileSync(
      path.join(repoRoot, 'ci', 'sign-release.sh'),
      ['--allow-test-pubkey', '--dist', distDir, '--version', version],
      {
        cwd: repoRoot,
        env: {
          ...process.env,
          RAGO_MINISIGN_SECRET_KEY_FILE: encSecPath,
          RAGO_MINISIGN_PASSWORD: testPass,
          RAGO_MINISIGN_PUBKEY: encPubParsed.base64,
        },
        stdio: 'pipe',
      }
    );
  }

  process.stdout.write(`==> [ok] real \`minisign\` CLI keygen + sign + \`minisign -Vm\` + dry-run publish passed.\n`);
}

function buildSyntheticArm64ElfSo(pAlignBytes) {
  // Minimal valid 64-bit little-endian EM_AARCH64 (183) ET_DYN ELF with 1 PT_LOAD segment (120 bytes total).
  const buf = Buffer.alloc(128, 0);
  buf[0] = 0x7f;
  buf.write('ELF', 1, 3, 'ascii');
  buf[4] = 2; // ELFCLASS64
  buf[5] = 1; // ELFDATA2LSB
  buf[6] = 1; // EV_CURRENT
  buf.writeUInt16LE(3, 16); // e_type = ET_DYN
  buf.writeUInt16LE(183, 18); // e_machine = EM_AARCH64
  buf.writeUInt32LE(1, 20); // e_version = EV_CURRENT
  buf.writeBigUInt64LE(64n, 32); // e_phoff = 64
  buf.writeUInt16LE(64, 52); // e_ehsize = 64
  buf.writeUInt16LE(56, 54); // e_phentsize = 56
  buf.writeUInt16LE(1, 56); // e_phnum = 1

  // Elf64_Phdr at offset 64
  buf.writeUInt32LE(1, 64); // p_type = PT_LOAD
  buf.writeUInt32LE(5, 68); // p_flags = PF_R | PF_X
  buf.writeBigUInt64LE(0n, 72); // p_offset
  buf.writeBigUInt64LE(0n, 80); // p_vaddr
  buf.writeBigUInt64LE(0n, 88); // p_paddr
  buf.writeBigUInt64LE(128n, 96); // p_filesz
  buf.writeBigUInt64LE(128n, 104); // p_memsz
  buf.writeBigUInt64LE(BigInt(pAlignBytes), 112); // p_align
  return buf;
}

function buildStoredZipBuffer(entries, { alignSo16k = false } = {}) {
  // Minimal uncompressed (store method 0) PKZIP builder — supports 16 KB extra-field padding and '../' for Zip-Slip negative tests.
  const localParts = [];
  const centralParts = [];
  let offset = 0;

  for (const { name, data, unixMode = 0o100644 } of entries) {
    const nameBuf = Buffer.from(name, 'utf8');
    const bodyBuf = Buffer.isBuffer(data) ? data : Buffer.from(data);
    const crc = zlib.crc32(bodyBuf) >>> 0;

    let extraLen = 0;
    if (alignSo16k && name.endsWith('.so')) {
      const rem = (offset + 30 + nameBuf.length) % 16384;
      extraLen = rem === 0 ? 0 : 16384 - rem;
    }
    const extraBuf = Buffer.alloc(extraLen, 0);

    const localHdr = Buffer.alloc(30);
    localHdr.writeUInt32LE(0x04034b50, 0);
    localHdr.writeUInt16LE(20, 4);
    localHdr.writeUInt16LE(0x0800, 6); // UTF-8 filename flag
    localHdr.writeUInt16LE(0, 8); // store
    localHdr.writeUInt32LE(crc, 14);
    localHdr.writeUInt32LE(bodyBuf.length, 18);
    localHdr.writeUInt32LE(bodyBuf.length, 22);
    localHdr.writeUInt16LE(nameBuf.length, 26);
    localHdr.writeUInt16LE(extraBuf.length, 28);

    localParts.push(localHdr, nameBuf, extraBuf, bodyBuf);

    const centralHdr = Buffer.alloc(46);
    centralHdr.writeUInt32LE(0x02014b50, 0);
    centralHdr.writeUInt16LE((3 << 8) | 20, 4); // version made by UNIX
    centralHdr.writeUInt16LE(20, 6);
    centralHdr.writeUInt16LE(0x0800, 8);
    centralHdr.writeUInt16LE(0, 10);
    centralHdr.writeUInt32LE(crc, 16);
    centralHdr.writeUInt32LE(bodyBuf.length, 20);
    centralHdr.writeUInt32LE(bodyBuf.length, 24);
    centralHdr.writeUInt16LE(nameBuf.length, 28);
    centralHdr.writeUInt32LE((unixMode << 16) >>> 0, 38);
    centralHdr.writeUInt32LE(offset, 42);
    centralParts.push(centralHdr, nameBuf);

    offset += localHdr.length + nameBuf.length + extraBuf.length + bodyBuf.length;
  }

  const centralBuf = Buffer.concat(centralParts);
  const eocd = Buffer.alloc(22);
  eocd.writeUInt32LE(0x06054b50, 0);
  eocd.writeUInt16LE(entries.length, 8);
  eocd.writeUInt16LE(entries.length, 10);
  eocd.writeUInt32LE(centralBuf.length, 12);
  eocd.writeUInt32LE(offset, 16);
  return Buffer.concat([...localParts, centralBuf, eocd]);
}

function cleanAndroidTestEnv(overrides = {}) {
  const env = { ...process.env };
  for (const k of Object.keys(env)) {
    if (k.startsWith('RAGO_ANDROID_') || k.startsWith('ANDROID_KEYSTORE_') || k.startsWith('ANDROID_KEY_')) {
      delete env[k];
    }
  }
  return { ...env, ...overrides };
}

function testAndroidSigningAnd16KbAlignmentGates(brand, tmpRoot) {
  const shScript = path.join(repoRoot, 'ci', 'build-android.sh');
  const psScript = path.join(repoRoot, 'ci', 'build-android.ps1');
  const androidTmp = path.join(tmpRoot, 'android-gates');
  fs.mkdirSync(androidTmp, { recursive: true });

  const pwshProbe = spawnSync('pwsh', ['-NoProfile', '-Command', '$PSVersionTable.PSVersion.Major'], {
    encoding: 'utf8',
  });
  const hasPwsh = !pwshProbe.error && pwshProbe.status === 0;

  // 1. Unsigned local/PR fallback (0 signing vars) -> exit 0 + warning
  const unsignedSh = spawnSync(shScript, ['--validate-signing-only'], {
    cwd: repoRoot,
    encoding: 'utf8',
    env: cleanAndroidTestEnv(),
  });
  assert.equal(unsignedSh.status, 0, unsignedSh.stderr);
  assert.match(unsignedSh.stderr, /warn: RAGO_ANDROID_KEYSTORE\* not set/);

  if (hasPwsh) {
    const unsignedPs = spawnSync('pwsh', ['-NoProfile', '-File', psScript, '-ValidateSigningOnly'], {
      cwd: repoRoot,
      encoding: 'utf8',
      env: cleanAndroidTestEnv(),
    });
    assert.equal(unsignedPs.status, 0, unsignedPs.stderr || unsignedPs.stdout);
    assert.match(unsignedPs.stdout, /warn: RAGO_ANDROID_KEYSTORE\* not set/);
  }

  // 2. Negative fail-fast parameter guards (before CMake or Qt runs)
  const negativeEnvCases = [
    [{ RAGO_ANDROID_TYPO_VAR: '1' }, /unrecognized Android variable/i],
    [{ RAGO_ANDROID_STRICT_ALIGN: 'maybe' }, /RAGO_ANDROID_STRICT_ALIGN must be/i],
    [{ RAGO_ANDROID_REQUIRE_SIGNING: '1' }, /RAGO_ANDROID_REQUIRE_SIGNING=1 requires RAGO_ANDROID_KEYSTORE/i],
    [{ RAGO_ANDROID_KEYSTORE_PASS: 'only-pass' }, /partially set|neither RAGO_ANDROID_KEYSTORE/i],
    [{ RAGO_ANDROID_KEYSTORE: '/tmp/a.jks', RAGO_ANDROID_KEYSTORE_BASE64: 'AAAA' }, /not both/i],
    [{ RAGO_ANDROID_KEYSTORE_BASE64: 'AAAA', RAGO_ANDROID_KEY_ALIAS: 'upload' }, /RAGO_ANDROID_KEYSTORE_PASS is missing/i],
    [{ RAGO_ANDROID_KEYSTORE_BASE64: 'AAAA', RAGO_ANDROID_KEYSTORE_PASS: 'secret' }, /RAGO_ANDROID_KEY_ALIAS is missing/i],
    [
      {
        RAGO_ANDROID_KEYSTORE_BASE64: 'AAAA',
        RAGO_ANDROID_KEYSTORE_PASS: 'secret',
        RAGO_ANDROID_KEY_ALIAS: '-J-Xmx64m',
      },
      /must not start with '-'/i,
    ],
    [
      {
        RAGO_ANDROID_KEYSTORE_BASE64: 'AAAA',
        RAGO_ANDROID_KEYSTORE_PASS: 'secret',
        RAGO_ANDROID_KEY_ALIAS: 'upload',
        ANDROID_KEY_ALIAS: 'different',
      },
      /conflicting values/i,
    ],
  ];

  // Create stub zipalign/apksigner dir ONLY for pure env/keystore validation if Android SDK build-tools aren't installed on host
  const stubBtDir = path.join(androidTmp, 'stub-build-tools');
  fs.mkdirSync(stubBtDir, { recursive: true });
  for (const toolName of ['zipalign', 'apksigner']) {
    const p = path.join(stubBtDir, toolName);
    fs.writeFileSync(p, '#!/usr/bin/env sh\nexit 0\n', { mode: 0o755 });
  }

  for (const [envOverrides, expectedErr] of negativeEnvCases) {
    const res = spawnSync(shScript, ['--validate-signing-only'], {
      cwd: repoRoot,
      encoding: 'utf8',
      env: cleanAndroidTestEnv(envOverrides),
    });
    assert.notEqual(res.status, 0, `Expected failure for ${JSON.stringify(envOverrides)}`);
    assert.match(`${res.stdout}\n${res.stderr}`, expectedErr);

    if (hasPwsh) {
      const psRes = spawnSync('pwsh', ['-NoProfile', '-File', psScript, '-ValidateSigningOnly'], {
        cwd: repoRoot,
        encoding: 'utf8',
        env: cleanAndroidTestEnv(envOverrides),
      });
      assert.notEqual(psRes.status, 0, `Expected pwsh failure for ${JSON.stringify(envOverrides)}`);
      assert.match(`${psRes.stdout}\n${psRes.stderr}`, expectedErr);
    }
  }

  // Bad Base64 test
  const badB64Res = spawnSync(shScript, ['--validate-signing-only'], {
    cwd: repoRoot,
    encoding: 'utf8',
    env: cleanAndroidTestEnv({
      RAGO_ANDROID_BUILD_TOOLS_DIR: stubBtDir,
      RAGO_ANDROID_KEYSTORE_BASE64: '!!!not-valid-base64!!!',
      RAGO_ANDROID_KEYSTORE_PASS: 'TestStorePass123!',
      RAGO_ANDROID_KEY_ALIAS: 'upload',
    }),
  });
  assert.notEqual(badB64Res.status, 0);
  assert.match(badB64Res.stderr, /valid Base64/);

  // 3. Real ephemeral PKCS#12 keystore validation (positive + wrong password + wrong alias + cert-only entry)
  const keytoolProbe = spawnSync('keytool', ['-help'], { encoding: 'utf8' });
  if (!keytoolProbe.error) {
    const ksPath = path.join(androidTmp, 'ephemeral-upload.p12');
    const storeSecret = 'EphemStorePass#2026!';
    execFileSync(
      'keytool',
      [
        '-genkeypair',
        '-keystore',
        ksPath,
        '-storetype',
        'PKCS12',
        '-storepass:env',
        'TEST_KS_PW',
        '-keypass:env',
        'TEST_KS_PW',
        '-alias',
        'upload',
        '-keyalg',
        'RSA',
        '-keysize',
        '2048',
        '-validity',
        '2',
        '-dname',
        `CN=${brand.productName} CI Test, O=${brand.productName}, C=TR`,
      ],
      { env: { ...process.env, TEST_KS_PW: storeSecret }, stdio: 'pipe' }
    );
    const ksB64 = fs.readFileSync(ksPath).toString('base64');

    // Positive: file mode + Base64 mode (and verify secret never appears in stdout/stderr)
    for (const modeEnv of [
      { RAGO_ANDROID_KEYSTORE: ksPath },
      { RAGO_ANDROID_KEYSTORE_BASE64: ksB64 },
    ]) {
      const okSh = spawnSync(shScript, ['--validate-signing-only'], {
        cwd: repoRoot,
        encoding: 'utf8',
        env: cleanAndroidTestEnv({
          RAGO_ANDROID_BUILD_TOOLS_DIR: stubBtDir,
          RAGO_ANDROID_KEYSTORE_PASS: storeSecret,
          RAGO_ANDROID_KEY_ALIAS: 'upload',
          ...modeEnv,
        }),
      });
      assert.equal(okSh.status, 0, okSh.stderr);
      assert.ok(!okSh.stdout.includes(storeSecret) && !okSh.stderr.includes(storeSecret), 'Keystore password leaked to output');

      if (hasPwsh && process.platform !== 'win32') {
        const okPs = spawnSync('pwsh', ['-NoProfile', '-File', psScript, '-ValidateSigningOnly'], {
          cwd: repoRoot,
          encoding: 'utf8',
          env: cleanAndroidTestEnv({
            RAGO_ANDROID_BUILD_TOOLS_DIR: stubBtDir,
            RAGO_ANDROID_KEYSTORE_PASS: storeSecret,
            RAGO_ANDROID_KEY_ALIAS: 'upload',
            ...modeEnv,
          }),
        });
        assert.equal(okPs.status, 0, `${okPs.stdout}\n${okPs.stderr}`);
        assert.ok(!okPs.stdout.includes(storeSecret) && !okPs.stderr.includes(storeSecret), 'Keystore password leaked to pwsh output');
      }
    }

    // Wrong store password rejected
    const wrongPassRes = spawnSync(shScript, ['--validate-signing-only'], {
      cwd: repoRoot,
      encoding: 'utf8',
      env: cleanAndroidTestEnv({
        RAGO_ANDROID_BUILD_TOOLS_DIR: stubBtDir,
        RAGO_ANDROID_KEYSTORE: ksPath,
        RAGO_ANDROID_KEYSTORE_PASS: 'WrongPassword999!',
        RAGO_ANDROID_KEY_ALIAS: 'upload',
      }),
    });
    assert.notEqual(wrongPassRes.status, 0);

    // Wrong private-key password (RAGO_ANDROID_KEY_PASS) rejected by pre-build jarsigner probe
    const wrongKeyPassRes = spawnSync(shScript, ['--validate-signing-only'], {
      cwd: repoRoot,
      encoding: 'utf8',
      env: cleanAndroidTestEnv({
        RAGO_ANDROID_BUILD_TOOLS_DIR: stubBtDir,
        RAGO_ANDROID_KEYSTORE: ksPath,
        RAGO_ANDROID_KEYSTORE_PASS: storeSecret,
        RAGO_ANDROID_KEY_ALIAS: 'upload',
        RAGO_ANDROID_KEY_PASS: 'WrongKeyPassword999!',
      }),
    });
    assert.notEqual(wrongKeyPassRes.status, 0);

    // Non-existent alias rejected
    const wrongAliasRes = spawnSync(shScript, ['--validate-signing-only'], {
      cwd: repoRoot,
      encoding: 'utf8',
      env: cleanAndroidTestEnv({
        RAGO_ANDROID_BUILD_TOOLS_DIR: stubBtDir,
        RAGO_ANDROID_KEYSTORE: ksPath,
        RAGO_ANDROID_KEYSTORE_PASS: storeSecret,
        RAGO_ANDROID_KEY_ALIAS: 'missing-alias',
      }),
    });
    assert.notEqual(wrongAliasRes.status, 0);

    // Certificate-only keystore (trustedCertEntry without private key) rejected
    const certPem = path.join(androidTmp, 'upload-cert.pem');
    const certOnlyKs = path.join(androidTmp, 'cert-only.p12');
    execFileSync(
      'keytool',
      ['-exportcert', '-rfc', '-keystore', ksPath, '-alias', 'upload', '-storepass:env', 'TEST_KS_PW', '-file', certPem],
      { env: { ...process.env, TEST_KS_PW: storeSecret }, stdio: 'pipe' }
    );
    execFileSync(
      'keytool',
      ['-importcert', '-noprompt', '-keystore', certOnlyKs, '-storetype', 'PKCS12', '-alias', 'upload', '-storepass:env', 'TEST_KS_PW', '-file', certPem],
      { env: { ...process.env, TEST_KS_PW: storeSecret }, stdio: 'pipe' }
    );
    const certOnlyRes = spawnSync(shScript, ['--validate-signing-only'], {
      cwd: repoRoot,
      encoding: 'utf8',
      env: cleanAndroidTestEnv({
        RAGO_ANDROID_BUILD_TOOLS_DIR: stubBtDir,
        RAGO_ANDROID_KEYSTORE: certOnlyKs,
        RAGO_ANDROID_KEYSTORE_PASS: storeSecret,
        RAGO_ANDROID_KEY_ALIAS: 'upload',
      }),
    });
    assert.notEqual(certOnlyRes.status, 0);
    assert.match(certOnlyRes.stderr, /PrivateKeyEntry/);

    // Optional real end-to-end APK (zipalign + apksigner v2/v3) & AAB (jarsigner) signing when Android SDK tools exist
    const sdkCandidates = [
      process.env.ANDROID_SDK_ROOT,
      process.env.ANDROID_HOME,
      path.join(os.homedir(), 'Library/Android/sdk'),
      '/usr/local/lib/android/sdk',
    ].filter((d) => d && fs.existsSync(path.join(d, 'build-tools')) && fs.existsSync(path.join(d, 'platforms')));
    if (sdkCandidates.length > 0) {
      const sdkDir = sdkCandidates[0];
      const btDirs = fs.readdirSync(path.join(sdkDir, 'build-tools')).sort().reverse();
      const platDirs = fs.readdirSync(path.join(sdkDir, 'platforms')).sort().reverse();
      const aapt2Path = btDirs.map((d) => path.join(sdkDir, 'build-tools', d, 'aapt2')).find((p) => fs.existsSync(p));
      const androidJar = platDirs.map((d) => path.join(sdkDir, 'platforms', d, 'android.jar')).find((p) => fs.existsSync(p));
      if (aapt2Path && androidJar) {
        const minManifestXml = path.join(androidTmp, 'AndroidManifest.xml');
        fs.writeFileSync(
          minManifestXml,
          `<?xml version="1.0" encoding="utf-8"?>\n<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="${brand.bundleId}">\n  <uses-sdk android:minSdkVersion="28" android:targetSdkVersion="37" />\n  <application android:label="${brand.productName}" />\n</manifest>\n`
        );
        const baseApk = path.join(androidTmp, 'unsigned-base.apk');
        execFileSync(aapt2Path, ['link', '-o', baseApk, '-I', androidJar, '--manifest', minManifestXml], { stdio: 'pipe' });

        // Add a 16 KB aligned ARM64 .so to an AAB archive and sign both APK & AAB via build-android.{sh,ps1}
        const unsignedAab = path.join(androidTmp, 'unsigned.aab');
        fs.writeFileSync(
          unsignedAab,
          buildStoredZipBuffer([
            { name: `base/lib/arm64-v8a/lib${brand.productId}_arm64-v8a.so`, data: buildSyntheticArm64ElfSo(16384) },
          ])
        );
        const signedApk = path.join(androidTmp, 'out-signed.apk');
        const signedAab = path.join(androidTmp, 'out-signed.aab');
        const signRun = spawnSync(
          shScript,
          ['--sign-packages-only', baseApk, unsignedAab, signedApk, signedAab],
          {
            cwd: repoRoot,
            encoding: 'utf8',
            env: cleanAndroidTestEnv({
              ANDROID_SDK_ROOT: sdkDir,
              RAGO_ANDROID_KEYSTORE_BASE64: ksB64,
              RAGO_ANDROID_KEYSTORE_PASS: storeSecret,
              RAGO_ANDROID_KEY_ALIAS: 'upload',
            }),
          }
        );
        assert.equal(signRun.status, 0, `${signRun.stdout}\n${signRun.stderr}`);
        assert.match(signRun.stdout, /Verified using v2 scheme \(APK Signature Scheme v2\): true/);
        assert.match(signRun.stdout, /Verified using v3 scheme \(APK Signature Scheme v3\): true/);
        assert.ok(fs.existsSync(signedApk) && fs.statSync(signedApk).size > 0);
        assert.ok(fs.existsSync(signedAab) && fs.statSync(signedAab).size > 0);

        if (hasPwsh) {
          const signedApkPs = path.join(androidTmp, 'out-signed-ps.apk');
          const signedAabPs = path.join(androidTmp, 'out-signed-ps.aab');
          const signRunPs = spawnSync(
            'pwsh',
            ['-NoProfile', '-File', psScript, '-SignPackagesOnly', `${baseApk},${unsignedAab},${signedApkPs},${signedAabPs}`],
            {
              cwd: repoRoot,
              encoding: 'utf8',
              env: cleanAndroidTestEnv({
                ANDROID_SDK_ROOT: sdkDir,
                RAGO_ANDROID_KEYSTORE_BASE64: ksB64,
                RAGO_ANDROID_KEYSTORE_PASS: storeSecret,
                RAGO_ANDROID_KEY_ALIAS: 'upload',
              }),
            }
          );
          // Note: PowerShell -File passes [string[]] as separate args or comma-separated depending on CLI parser; invoke via -Command for exact string[] array binding
          const signRunPsCmd = signRunPs.status === 0
            ? signRunPs
            : spawnSync(
                'pwsh',
                [
                  '-NoProfile',
                  '-Command',
                  `& $env:TEST_PS_SCRIPT -SignPackagesOnly @($env:TEST_IN_APK, $env:TEST_IN_AAB, $env:TEST_OUT_APK, $env:TEST_OUT_AAB)`,
                ],
                {
                  cwd: repoRoot,
                  encoding: 'utf8',
                  env: cleanAndroidTestEnv({
                    ANDROID_SDK_ROOT: sdkDir,
                    RAGO_ANDROID_KEYSTORE_BASE64: ksB64,
                    RAGO_ANDROID_KEYSTORE_PASS: storeSecret,
                    RAGO_ANDROID_KEY_ALIAS: 'upload',
                    TEST_PS_SCRIPT: psScript,
                    TEST_IN_APK: baseApk,
                    TEST_IN_AAB: unsignedAab,
                    TEST_OUT_APK: signedApkPs,
                    TEST_OUT_AAB: signedAabPs,
                  }),
                }
              );
          assert.equal(signRunPsCmd.status, 0, `${signRunPsCmd.stdout}\n${signRunPsCmd.stderr}`);
          assert.match(signRunPsCmd.stdout, /Verified using v2 scheme \(APK Signature Scheme v2\): true/);
          assert.match(signRunPsCmd.stdout, /Verified using v3 scheme \(APK Signature Scheme v3\): true/);
          assert.ok(fs.existsSync(signedApkPs) && fs.statSync(signedApkPs).size > 0);
          assert.ok(fs.existsSync(signedAabPs) && fs.statSync(signedAabPs).size > 0);
        }
      }
    }
  }

  // 4. 16 KB ELF LOAD alignment gate (synthetic 64-bit ARM64 .so, .apk, .aab, empty archive, Zip-Slip archive)
  const alignedSoBuf = buildSyntheticArm64ElfSo(16384); // 0x4000 (16 KB) -> PASS
  const misalignedSoBuf = buildSyntheticArm64ElfSo(4096); // 0x1000 (4 KB) -> FAIL

  const alignedSoPath = path.join(androidTmp, 'libgood_16k.so');
  const misalignedSoPath = path.join(androidTmp, 'libbad_4k.so');
  fs.writeFileSync(alignedSoPath, alignedSoBuf);
  fs.writeFileSync(misalignedSoPath, misalignedSoBuf);

  const alignedApkPath = path.join(androidTmp, 'good.apk');
  fs.writeFileSync(
    alignedApkPath,
    buildStoredZipBuffer([{ name: `lib/arm64-v8a/lib${brand.productId}_arm64-v8a.so`, data: alignedSoBuf }], { alignSo16k: true })
  );
  const unalignedZipOffsetApkPath = path.join(androidTmp, 'bad-zip-offset.apk');
  fs.writeFileSync(
    unalignedZipOffsetApkPath,
    buildStoredZipBuffer([{ name: `lib/arm64-v8a/lib${brand.productId}_arm64-v8a.so`, data: alignedSoBuf }], { alignSo16k: false })
  );
  const alignedAabPath = path.join(androidTmp, 'good.aab');
  fs.writeFileSync(
    alignedAabPath,
    buildStoredZipBuffer([{ name: `base/lib/arm64-v8a/lib${brand.productId}_arm64-v8a.so`, data: alignedSoBuf }])
  );

  const misalignedApkPath = path.join(androidTmp, 'bad-4k.apk');
  fs.writeFileSync(
    misalignedApkPath,
    buildStoredZipBuffer(
      [
        { name: `lib/arm64-v8a/lib${brand.productId}_arm64-v8a.so`, data: alignedSoBuf },
        { name: 'lib/arm64-v8a/liblegacy_4k.so', data: misalignedSoBuf },
      ],
      { alignSo16k: true }
    )
  );
  const misalignedAabPath = path.join(androidTmp, 'bad-4k.aab');
  fs.writeFileSync(
    misalignedAabPath,
    buildStoredZipBuffer([{ name: 'base/lib/arm64-v8a/liblegacy_4k.so', data: misalignedSoBuf }])
  );
  const emptyNativeApkPath = path.join(androidTmp, 'no-native.apk');
  fs.writeFileSync(emptyNativeApkPath, buildStoredZipBuffer([{ name: 'classes.dex', data: 'dex' }]));
  const zipSlipApkPath = path.join(androidTmp, 'zip-slip.apk');
  fs.writeFileSync(
    zipSlipApkPath,
    buildStoredZipBuffer([{ name: 'lib/arm64-v8a/../../../escaped.so', data: alignedSoBuf }])
  );

  // Missing readelf when RAGO_ANDROID_STRICT_ALIGN=1 must fail fast
  const badReadelfRes = spawnSync(shScript, ['--check-align-only', alignedSoPath], {
    cwd: repoRoot,
    encoding: 'utf8',
    env: cleanAndroidTestEnv({ RAGO_ANDROID_READELF: '/nonexistent/llvm-readelf' }),
  });
  assert.notEqual(badReadelfRes.status, 0);

  // If system/NDK readelf is unavailable (e.g. minimal container), create a Node-backed ELF program-header reader for the gate test
  const probeReadelf = spawnSync(shScript, ['--check-align-only', alignedSoPath], {
    cwd: repoRoot,
    encoding: 'utf8',
    env: cleanAndroidTestEnv(),
  });
  const alignEnv = {};
  if (probeReadelf.status !== 0 && /readelf not found/i.test(probeReadelf.stderr)) {
    const readerJs = path.join(androidTmp, 'mini-readelf.cjs');
    fs.writeFileSync(
      readerJs,
      [
        'const fs = require("fs");',
        'const f = process.argv[process.argv.length - 1];',
        'const b = fs.readFileSync(f);',
        'if (b.length < 120 || b[0] !== 0x7f || b.toString("ascii", 1, 4) !== "ELF") process.exit(1);',
        'const p = b.readBigUInt64LE(112);',
        'console.log("  LOAD           0x000000 0x0000000000000000 0x0000000000000000 0x000080 0x000080 R E 0x" + p.toString(16));',
      ].join('\n')
    );
    const nodeReadelf = path.join(androidTmp, 'mini-readelf.sh');
    fs.writeFileSync(nodeReadelf, `#!/usr/bin/env sh\nexec "${process.execPath}" "${readerJs}" "$@"\n`, { mode: 0o755 });
    alignEnv.RAGO_ANDROID_READELF = nodeReadelf;
  }

  // Positive: 16 KB aligned .so + .apk + .aab pass together
  const okAlignSh = spawnSync(shScript, ['--check-align-only', alignedSoPath, alignedApkPath, alignedAabPath], {
    cwd: repoRoot,
    encoding: 'utf8',
    env: cleanAndroidTestEnv(alignEnv),
  });
  assert.equal(okAlignSh.status, 0, `${okAlignSh.stdout}\n${okAlignSh.stderr}`);
  assert.match(okAlignSh.stdout, /16 KB ELF LOAD alignment check passed/);

  if (hasPwsh) {
    const okAlignPs = spawnSync(
      'pwsh',
      [
        '-NoProfile',
        '-Command',
        '& $env:TEST_PS_SCRIPT -CheckAlignOnly @($env:TEST_ALIGN_SO, $env:TEST_ALIGN_APK, $env:TEST_ALIGN_AAB)',
      ],
      {
        cwd: repoRoot,
        encoding: 'utf8',
        env: cleanAndroidTestEnv({
          ...alignEnv,
          TEST_PS_SCRIPT: psScript,
          TEST_ALIGN_SO: alignedSoPath,
          TEST_ALIGN_APK: alignedApkPath,
          TEST_ALIGN_AAB: alignedAabPath,
        }),
      }
    );
    assert.equal(okAlignPs.status, 0, `${okAlignPs.stdout}\n${okAlignPs.stderr}`);
  }

  // Negative: 4 KB .so, 4 KB .apk, 4 KB .aab, native-less .apk, and Zip-Slip .apk MUST ALL fail under default RAGO_ANDROID_STRICT_ALIGN=1
  for (const [badTarget, expectedPattern] of [
    [misalignedSoPath, /0x1000|alignment gate failed/i],
    [misalignedApkPath, /0x1000|alignment gate failed/i],
    [unalignedZipOffsetApkPath, /ZIP offset|0x4000/i],
    [misalignedAabPath, /0x1000|alignment gate failed/i],
    [emptyNativeApkPath, /zero native \.so libraries/i],
    [zipSlipApkPath, /traversal|unsafe/i],
  ]) {
    const failSh = spawnSync(shScript, ['--check-align-only', badTarget], {
      cwd: repoRoot,
      encoding: 'utf8',
      env: cleanAndroidTestEnv(alignEnv),
    });
    assert.notEqual(failSh.status, 0, `Expected 16 KB gate to reject ${badTarget}`);
    assert.match(`${failSh.stdout}\n${failSh.stderr}`, expectedPattern);

    if (hasPwsh) {
      const failPs = spawnSync('pwsh', ['-NoProfile', '-File', psScript, '-CheckAlignOnly', badTarget], {
        cwd: repoRoot,
        encoding: 'utf8',
        env: cleanAndroidTestEnv(alignEnv),
      });
      assert.notEqual(failPs.status, 0, `Expected pwsh 16 KB gate to reject ${badTarget}`);
    }
  }
}

function testRemoteManifestSyncVerifier(brand, signedFixture, tmpRoot) {
  const { distDir, version, keyA } = signedFixture;
  const manifestPath = path.join(distDir, 'releases.json');
  const sigPath = path.join(distDir, 'releases.json.minisig');

  // 1. Tag grammar & URL derivation from single-source brand defaults
  assert.deepEqual(normalizeStableReleaseTag('v2.1.2'), { tag: 'v2.1.2', version: '2.1.2' });
  for (const badTag of ['2.1.2', 'v2.1.2-rc.1', 'v2.1.2-alpha.1', 'v02.1.2', 'v2.1.2;id', '../v2.1.2', '']) {
    assert.throws(() => normalizeStableReleaseTag(badTag), /Refusing non-stable or invalid release tag/);
  }

  const urls = buildReleaseSyncUrls('v2.1.2', repoRoot);
  assert.equal(urls.githubRepo, `${brand.githubOwner}/${brand.productId}`);
  assert.equal(
    urls.manifestUrl,
    `https://github.com/${brand.githubOwner}/${brand.productId}/releases/download/v2.1.2/releases.json`
  );
  assert.equal(urls.liveManifestUrl, `${brand.defaultDlBaseUrl}/releases.json`);

  // 2. Positive: signed manifest passes with older live version (upgrade) and equal live version (idempotent re-run)
  const syncDir = path.join(tmpRoot, 'remote-sync-test');
  fs.mkdirSync(syncDir, { recursive: true });
  const olderLivePath = path.join(syncDir, 'live-older.json');
  fs.writeFileSync(olderLivePath, JSON.stringify({ latestVersion: '1.9.5' }), 'utf8');

  const okUpgrade = verifySignedManifestFiles({
    manifestPath,
    sigPath,
    expectedTag: `v${version}`,
    repoRoot,
    currentManifestPath: olderLivePath,
    allowTestPubkey: true,
    pubKeyOverride: keyA.base64,
    checkClockSkew: false,
  });
  assert.equal(okUpgrade.version, version);
  assert.equal(okUpgrade.previousVersion, '1.9.5');

  const sameLivePath = path.join(syncDir, 'live-same.json');
  fs.writeFileSync(sameLivePath, JSON.stringify({ latestVersion: version }), 'utf8');
  const okIdempotent = verifySignedManifestFiles({
    manifestPath,
    sigPath,
    expectedTag: `v${version}`,
    repoRoot,
    currentManifestPath: sameLivePath,
    allowTestPubkey: true,
    pubKeyOverride: keyA.base64,
    checkClockSkew: false,
  });
  assert.equal(okIdempotent.previousVersion, version);

  // 3. Negative: pubKeyOverride without allowTestPubkey is strictly forbidden
  assert.throws(
    () =>
      verifySignedManifestFiles({
        manifestPath,
        sigPath,
        expectedTag: `v${version}`,
        repoRoot,
        pubKeyOverride: keyA.base64,
        checkClockSkew: false,
      }),
    /pubKeyOverride is forbidden/
  );

  // 4. Negative: version downgrade over newer live manifest is refused unless allowRollback=true
  const newerLivePath = path.join(syncDir, 'live-newer.json');
  fs.writeFileSync(newerLivePath, JSON.stringify({ latestVersion: '9.9.9' }), 'utf8');
  assert.throws(
    () =>
      verifySignedManifestFiles({
        manifestPath,
        sigPath,
        expectedTag: `v${version}`,
        repoRoot,
        currentManifestPath: newerLivePath,
        allowTestPubkey: true,
        pubKeyOverride: keyA.base64,
        checkClockSkew: false,
      }),
    /Refusing version downgrade/
  );

  const okRollback = verifySignedManifestFiles({
    manifestPath,
    sigPath,
    expectedTag: `v${version}`,
    repoRoot,
    currentManifestPath: newerLivePath,
    allowRollback: true,
    allowTestPubkey: true,
    pubKeyOverride: keyA.base64,
    checkClockSkew: false,
  });
  assert.equal(okRollback.version, version);

  // 5. Negative: tag mismatch against validly signed manifest is refused
  assert.throws(
    () =>
      verifySignedManifestFiles({
        manifestPath,
        sigPath,
        expectedTag: 'v2.0.1',
        repoRoot,
        allowTestPubkey: true,
        pubKeyOverride: keyA.base64,
        checkClockSkew: false,
      }),
    /Trusted comment release mismatch/
  );

  // 6. Negative: tampered JSON bytes fail Ed25519 verification
  const tamperedPath = path.join(syncDir, 'tampered.json');
  fs.writeFileSync(tamperedPath, `${fs.readFileSync(manifestPath, 'utf8')}\n`, 'utf8');
  assert.throws(
    () =>
      verifySignedManifestFiles({
        manifestPath: tamperedPath,
        sigPath,
        expectedTag: `v${version}`,
        repoRoot,
        allowTestPubkey: true,
        pubKeyOverride: keyA.base64,
        checkClockSkew: false,
      }),
    /signature verification failed/
  );
}

async function main() {
  const brand = readBrandDefaults(repoRoot);
  assert.ok(brand.productId.length > 0);
  assert.ok(brand.domain.length > 0);

  const tmpRoot = makeTempDir(`${brand.productId}-release-pipeline-test-`);
  try {
    await testManifestGenerationAndSchemaCompatibility(brand, tmpRoot);
    testNegativeManifestAndFilesystemGuards(brand, tmpRoot);
    const signedFixture = testMinisignCryptographicAndCommentGuards(brand, tmpRoot);
    testRemoteManifestSyncVerifier(brand, signedFixture, tmpRoot);
    testPublishDryRunAndLiveGuards(brand, signedFixture);
    testPublishPrereleaseSkipsRootManifest(brand, tmpRoot, signedFixture.keyA);
    testRealMinisignCliEndToEndIfInstalled(brand, tmpRoot);
    testAndroidSigningAnd16KbAlignmentGates(brand, tmpRoot);
    process.stdout.write(
      `==> release pipeline test suite passed (${brand.productName}, manifest + site/pulse compatibility + negative guards + Ed25519/minisign + dry-run).\n`
    );
  } finally {
    fs.rmSync(tmpRoot, { recursive: true, force: true });
  }
}

await main();
