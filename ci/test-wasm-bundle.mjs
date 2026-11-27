#!/usr/bin/env node
/**
 * Automated unit, integration & negative security test suite for Roadmap W6:
 * - CSP-compliant WASM loader templates (`src/ragoshell-app/wasm/index.html.in`, `wasm-boot.js.in`)
 * - Brand single-source cleanliness (no hardcoded brand literals in templates or WASM CI scripts)
 * - WebAssembly binary shared-memory (`-pthread`) capability parser (`inspectWasmBinaryCapabilities`)
 * - Streaming Brotli fallback (`ci/compress-brotli.mjs`)
 * - Deterministic POSIX USTAR `.tar.gz` packager & `deploy-receive.sh demo` verifier (`ci/package-wasm-demo.mjs`)
 * - CLI validation modes of `ci/build-wasm.sh` / `ci/build-wasm.ps1`
 * - Coexistence of `dist/wasm/` with `ci/generate-releases-json.mjs` and `ci/generate-sbom.mjs`
 */

import assert from 'node:assert/strict';
import { execFileSync, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import zlib from 'node:zlib';

import { compressFileBrotli } from './compress-brotli.mjs';
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
} from './generate-sbom.mjs';
import { generateEphemeralTestKeyPair, signBufferWithTestKey } from './lib/minisign.mjs';
import {
  createUstarHeader,
  inspectWasmBinaryCapabilities,
  packageWasmDemo,
  validateAndPrepareWasmStage,
  verifyWasmDemoArchive,
  verifyWasmLoaderCsp,
} from './package-wasm-demo.mjs';
import { buildWasmDemoSyncUrls, verifySignedWasmDemoFiles } from './verify-remote-manifest.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(__dirname, '..');

function makeTempDir(prefix) {
  return fs.mkdtempSync(path.join(os.tmpdir(), prefix));
}

function renderWasmTemplates(brand, { mode = 'playground', requiresThreads = true } = {}) {
  const indexIn = fs.readFileSync(
    path.join(repoRoot, 'src', 'ragoshell-app', 'wasm', 'index.html.in'),
    'utf8'
  );
  const bootIn = fs.readFileSync(
    path.join(repoRoot, 'src', 'ragoshell-app', 'wasm', 'wasm-boot.js.in'),
    'utf8'
  );

  let ident = brand.productId.replace(/[^a-zA-Z0-9_]/g, '_');
  if (/^[0-9]/.test(ident)) {
    ident = `_${ident}`;
  }
  const exportName = `${ident}_entry`;
  const envKey = `${brand.productId.toUpperCase()}_WASM_MODE`;

  const replaceVars = (tpl) =>
    tpl
      .replaceAll('@RAGO_PRODUCT_NAME@', brand.productName)
      .replaceAll('@RAGO_PRODUCT_ID@', brand.productId)
      .replaceAll('@RAGO_WASM_EXPORT_NAME@', exportName)
      .replaceAll('@RAGO_WASM_MODE_ENV_KEY@', envKey)
      .replaceAll('@RAGO_WASM_DEFAULT_MODE@', mode)
      .replaceAll('@RAGO_WASM_REQUIRES_THREADS@', requiresThreads ? 'true' : 'false');

  return {
    indexHtml: replaceVars(indexIn),
    bootJs: replaceVars(bootIn),
    exportName,
  };
}

/**
 * Builds a minimal valid WebAssembly v1 binary with an `env.memory` import.
 * When `shared` is true, sets memory limits flags to `0x03` (`has_max | shared`).
 */
function buildMinimalWasmBinary({ shared = true } = {}) {
  // Import section (id=2): 1 import: module="env" (3), field="memory" (6), kind=0x02 (memory),
  // flags = shared ? 0x03 : 0x01, min = 1, max = 2
  const importPayload = Buffer.from([
    0x01, // 1 import
    0x03, 0x65, 0x6e, 0x76, // "env"
    0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, // "memory"
    0x02, // memory import
    shared ? 0x03 : 0x01,
    0x01, // initial pages = 1
    0x02, // max pages = 2
  ]);
  const sectionHeader = Buffer.from([0x02, importPayload.length]);
  // Add compressible padding in a custom section (id=0) so Brotli shrinks it noticeably.
  const customData = Buffer.alloc(4096, 0x41);
  const customName = Buffer.from([0x04, 0x74, 0x65, 0x73, 0x74]); // "test"
  const customLen = customName.length + customData.length;
  // ULEB128 for customLen (4101 = 0x1005 -> 0x85, 0x20)
  const customHeader = Buffer.from([0x00, (customLen & 0x7f) | 0x80, customLen >> 7]);

  return Buffer.concat([
    Buffer.from([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00]),
    sectionHeader,
    importPayload,
    customHeader,
    customName,
    customData,
  ]);
}

function populateValidStage(stageDir, brand, { shared = true, includeVcpkgCopyrights = true } = {}) {
  fs.mkdirSync(stageDir, { recursive: true });
  const { indexHtml, bootJs, exportName } = renderWasmTemplates(brand, {
    mode: 'playground',
    requiresThreads: shared,
  });

  fs.writeFileSync(path.join(stageDir, 'index.html'), indexHtml, 'utf8');
  fs.writeFileSync(path.join(stageDir, 'wasm-boot.js'), bootJs, 'utf8');
  fs.writeFileSync(
    path.join(stageDir, 'qtloader.js'),
    'async function qtLoad(config) { return config; }\n',
    'utf8'
  );
  fs.writeFileSync(
    path.join(stageDir, `${brand.productId}.js`),
    `window.${exportName} = async function(cfg) { return cfg; };\n`,
    'utf8'
  );
  fs.writeFileSync(path.join(stageDir, `${brand.productId}.wasm`), buildMinimalWasmBinary({ shared }));
  fs.copyFileSync(
    path.join(repoRoot, 'src', 'ragoshell-app', 'resources', 'app_icon.png'),
    path.join(stageDir, 'app_icon.png')
  );

  // Populate licenses/ matching cmake/RagoShellLicenses.cmake
  const licDir = path.join(stageDir, 'licenses');
  const tpDir = path.join(licDir, 'third-party');
  fs.mkdirSync(tpDir, { recursive: true });
  for (const rootFile of ROOT_LICENSE_FILES) {
    fs.copyFileSync(path.join(repoRoot, rootFile), path.join(licDir, rootFile));
  }
  for (const spec of EMBEDDED_THIRD_PARTY_LICENSE_MAP) {
    fs.copyFileSync(path.join(repoRoot, spec.relSource), path.join(tpDir, spec.installedName));
  }
  if (includeVcpkgCopyrights) {
    fs.writeFileSync(path.join(tpDir, 'mongo-cxx-driver-COPYRIGHT'), 'Copyright MongoDB\n', 'utf8');
    fs.writeFileSync(path.join(tpDir, 'openssl-COPYRIGHT'), 'Copyright OpenSSL\n', 'utf8');
    fs.writeFileSync(path.join(tpDir, 'libssh2-COPYRIGHT'), 'Copyright libssh2\n', 'utf8');
  }
}

function testBrandSingleSource(brand) {
  const filesToCheck = [
    path.join(repoRoot, 'src', 'ragoshell-app', 'wasm', 'index.html.in'),
    path.join(repoRoot, 'src', 'ragoshell-app', 'wasm', 'wasm-boot.js.in'),
    path.join(repoRoot, 'ci', 'compress-brotli.mjs'),
    path.join(repoRoot, 'ci', 'package-wasm-demo.mjs'),
    path.join(repoRoot, 'ci', 'build-wasm.sh'),
    path.join(repoRoot, 'ci', 'build-wasm.ps1'),
    path.join(repoRoot, 'ci', 'sign-wasm-demo.sh'),
  ];
  const oldBrandRe = /rago[\s_-]?mongo/i;
  const currentBrandRe = new RegExp(brand.productName, 'i');

  for (const filePath of filesToCheck) {
    const content = fs.readFileSync(filePath, 'utf8');
    assert.equal(
      oldBrandRe.test(content),
      false,
      `Forbidden pre-rename brand found in ${path.relative(repoRoot, filePath)}`
    );
    // Strip comment lines (`#` or `//` or `*`) and the single-source filename `RagoShellDefaults.cmake`
    // before checking that code/templates never spell productName literally.
    const codeLines = content
      .split('\n')
      .filter((l) => !/^\s*(?:#|\/\/|\*|\/\*)/.test(l))
      .join('\n')
      .replaceAll('RagoShellDefaults.cmake', '');
    assert.equal(
      currentBrandRe.test(codeLines),
      false,
      `Hardcoded brand literal "${brand.productName}" found in non-comment lines of ${path.relative(repoRoot, filePath)}`
    );
  }
}

async function testHappyPathAndDeterminism(brand) {
  const tmp = makeTempDir('rago-wasm-test-');
  try {
    const stage1 = path.join(tmp, 'stage1');
    const stage2 = path.join(tmp, 'stage2');
    populateValidStage(stage1, brand, { shared: true });
    populateValidStage(stage2, brand, { shared: true });

    const out1 = path.join(tmp, 'out1', `${brand.productId}-wasm-demo.tar.gz`);
    const out2 = path.join(tmp, 'out2', `${brand.productId}-wasm-demo.tar.gz`);

    const res1 = await packageWasmDemo({
      stageDir: stage1,
      outputPath: out1,
      requireThreads: true,
      requireVcpkgCopyrights: true,
    });
    const res2 = await packageWasmDemo({
      stageDir: stage2,
      outputPath: out2,
      requireThreads: true,
      requireVcpkgCopyrights: true,
    });

    assert.equal(res1.hasSharedMemory, true);
    assert.equal(res1.sha256, res2.sha256, 'Deterministic USTAR .tar.gz digests must match byte-for-byte');
    assert.ok(fs.existsSync(`${out1}.sha256`));
    assert.ok(fs.existsSync(path.join(stage1, `${brand.productId}.wasm.br`)));

    const verified = await verifyWasmDemoArchive(out1);
    assert.equal(verified.sha256, res1.sha256);

    // Also test standalone CLI scripts (--validate-stage-only and --verify-archive-only)
    execFileSync(
      process.execPath,
      [path.join(repoRoot, 'ci', 'package-wasm-demo.mjs'), '--validate-stage-only', stage1, '--require-threads'],
      { cwd: repoRoot, stdio: 'pipe' }
    );
    execFileSync(
      process.execPath,
      [path.join(repoRoot, 'ci', 'package-wasm-demo.mjs'), '--verify-archive-only', out1],
      { cwd: repoRoot, stdio: 'pipe' }
    );

    if (process.platform === 'win32') {
      const pwsh = spawnSync('powershell.exe', [
        '-NoProfile',
        '-ExecutionPolicy',
        'Bypass',
        '-File',
        path.join(repoRoot, 'ci', 'build-wasm.ps1'),
        '-ValidateStageOnly',
        stage1,
      ]);
      assert.equal(pwsh.status, 0, `build-wasm.ps1 -ValidateStageOnly failed: ${pwsh.stderr?.toString()}`);

      const pwshArc = spawnSync('powershell.exe', [
        '-NoProfile',
        '-ExecutionPolicy',
        'Bypass',
        '-File',
        path.join(repoRoot, 'ci', 'build-wasm.ps1'),
        '-VerifyArchiveOnly',
        out1,
      ]);
      assert.equal(pwshArc.status, 0, `build-wasm.ps1 -VerifyArchiveOnly failed: ${pwshArc.stderr?.toString()}`);
    } else {
      execFileSync('bash', [path.join(repoRoot, 'ci', 'build-wasm.sh'), '--validate-stage-only', stage1], {
        cwd: repoRoot,
        stdio: 'pipe',
      });
      execFileSync('bash', [path.join(repoRoot, 'ci', 'build-wasm.sh'), '--verify-archive-only', out1], {
        cwd: repoRoot,
        stdio: 'pipe',
      });
    }
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
}

async function testNegativeSecurityAndCspGuards(brand) {
  const tmp = makeTempDir('rago-wasm-neg-');
  try {
    // 1. Inline <script> in index.html must be rejected
    const sInline = path.join(tmp, 'inline-script');
    populateValidStage(sInline, brand);
    const cleanHtml = fs.readFileSync(path.join(sInline, 'index.html'), 'utf8');
    fs.writeFileSync(
      path.join(sInline, 'index.html'),
      cleanHtml.replace('</body>', '<script>init()</script></body>'),
      'utf8'
    );
    assert.throws(() => verifyWasmLoaderCsp(sInline, brand), /forbidden inline <script>/);

    // 2. Inline onload= attribute (including slash-separated <body/onload=...>) in index.html must be rejected
    const sOnload = path.join(tmp, 'onload-attr');
    populateValidStage(sOnload, brand);
    fs.writeFileSync(
      path.join(sOnload, 'index.html'),
      cleanHtml.replace('<body>', '<body/onload="init()">'),
      'utf8'
    );
    assert.throws(() => verifyWasmLoaderCsp(sOnload, brand), /inline event handler/);

    // 2b. Inline style= attribute and unclosed <script> in index.html must be rejected
    const sStyleAttr = path.join(tmp, 'style-attr');
    populateValidStage(sStyleAttr, brand);
    fs.writeFileSync(
      path.join(sStyleAttr, 'index.html'),
      cleanHtml.replace('<body>', '<body style="margin:0">'),
      'utf8'
    );
    assert.throws(() => verifyWasmLoaderCsp(sStyleAttr, brand), /inline style=/);

    const sUnclosedScript = path.join(tmp, 'unclosed-script');
    populateValidStage(sUnclosedScript, brand);
    fs.writeFileSync(
      path.join(sUnclosedScript, 'index.html'),
      cleanHtml.replace('</body>', '<script src="wasm-boot.js"></body>'),
      'utf8'
    );
    assert.throws(() => verifyWasmLoaderCsp(sUnclosedScript, brand), /unclosed or malformed <script>/);

    // 3. External script URL in index.html must be rejected
    const sExt = path.join(tmp, 'ext-script');
    populateValidStage(sExt, brand);
    fs.writeFileSync(
      path.join(sExt, 'index.html'),
      cleanHtml.replace('src="qtloader.js"', 'src="https://cdn.example.com/qtloader.js"'),
      'utf8'
    );
    assert.throws(() => verifyWasmLoaderCsp(sExt, brand), /relative same-origin path/);

    // 4. innerHTML in wasm-boot.js must be rejected
    const sInner = path.join(tmp, 'inner-html');
    populateValidStage(sInner, brand);
    fs.appendFileSync(path.join(sInner, 'wasm-boot.js'), '\nstatus.innerHTML = "bad";\n', 'utf8');
    assert.throws(() => verifyWasmLoaderCsp(sInner, brand), /\.innerHTML/);

    // 5. new Function( in <product>.js (missing -s DYNAMIC_EXECUTION=0) must be rejected
    const sDynExec = path.join(tmp, 'dyn-exec');
    populateValidStage(sDynExec, brand);
    fs.appendFileSync(path.join(sDynExec, `${brand.productId}.js`), '\nvar f = new Function("return 1");\n', 'utf8');
    assert.throws(() => verifyWasmLoaderCsp(sDynExec, brand), /DYNAMIC_EXECUTION=0/);

    // 6. Single-threaded WASM binary rejected when --require-threads is enabled
    const sNoThreads = path.join(tmp, 'no-threads');
    populateValidStage(sNoThreads, brand, { shared: false });
    const caps = inspectWasmBinaryCapabilities(path.join(sNoThreads, `${brand.productId}.wasm`));
    assert.equal(caps.hasSharedMemory, false);
    await assert.rejects(
      () => validateAndPrepareWasmStage(sNoThreads, { requireThreads: true }),
      /shared linear memory/
    );

    // 7. Dotfile in stage rejected (50-demo.conf.template denies /\.)
    const sDot = path.join(tmp, 'dotfile');
    populateValidStage(sDot, brand);
    fs.writeFileSync(path.join(sDot, '.DS_Store'), 'junk', 'utf8');
    await assert.rejects(() => validateAndPrepareWasmStage(sDot, { requireThreads: true }), /Dotfile/);

    // 8. Stray <product>.html in stage rejected
    const sStrayHtml = path.join(tmp, 'stray-html');
    populateValidStage(sStrayHtml, brand);
    fs.writeFileSync(path.join(sStrayHtml, `${brand.productId}.html`), '<html></html>', 'utf8');
    await assert.rejects(() => validateAndPrepareWasmStage(sStrayHtml, { requireThreads: true }), /Refusing stray/);

    // 9. Malicious symlink tar archive rejected by verifyWasmDemoArchive
    const badTarGz = path.join(tmp, 'bad-symlink.tar.gz');
    const symHdr = createUstarHeader('evil-link', 0, false);
    symHdr[156] = '2'.charCodeAt(0); // '2' = symlink!
    // Recompute header checksum after mutating typeflag
    symHdr.fill(0x20, 148, 156);
    let chk = 0;
    for (let i = 0; i < 512; i += 1) chk += symHdr[i];
    symHdr.write(chk.toString(8).padStart(6, '0'), 148, 6, 'ascii');
    symHdr[154] = 0;
    symHdr[155] = 0x20;
    const rawTar = Buffer.concat([symHdr, Buffer.alloc(1024, 0)]);
    fs.writeFileSync(badTarGz, zlib.gzipSync(rawTar));
    await assert.rejects(() => verifyWasmDemoArchive(badTarGz), /forbidden entry type '2'/);

    // 10. Path traversal tar archive rejected by verifyWasmDemoArchive
    const travTarGz = path.join(tmp, 'bad-traversal.tar.gz');
    const travHdr = createUstarHeader('../escape.txt', 4, false);
    const travBody = Buffer.alloc(512, 0);
    travBody.write('test', 0, 4, 'ascii');
    fs.writeFileSync(travTarGz, zlib.gzipSync(Buffer.concat([travHdr, travBody, Buffer.alloc(1024, 0)])));
    await assert.rejects(() => verifyWasmDemoArchive(travTarGz), /traversal/);

    // 11. Directory entry with non-zero size (header smuggling) rejected by verifyWasmDemoArchive
    const dirSmuggleTarGz = path.join(tmp, 'bad-dir-size.tar.gz');
    const dirHdr = createUstarHeader('licenses/', 0, true);
    // Write non-zero size (512 -> octal 00000001000) into directory header bytes 124..135
    dirHdr.write('00000001000', 124, 11, 'ascii');
    dirHdr[135] = 0;
    dirHdr.fill(0x20, 148, 156);
    let dirChk = 0;
    for (let i = 0; i < 512; i += 1) dirChk += dirHdr[i];
    dirHdr.write(dirChk.toString(8).padStart(6, '0'), 148, 6, 'ascii');
    dirHdr[154] = 0;
    dirHdr[155] = 0x20;
    fs.writeFileSync(dirSmuggleTarGz, zlib.gzipSync(Buffer.concat([dirHdr, Buffer.alloc(1536, 0)])));
    await assert.rejects(() => verifyWasmDemoArchive(dirSmuggleTarGz), /must have size 0/);

    // 12. Mismatched .wasm.br (valid 8-byte \0asm prefix, altered tail) rejected by validateAndPrepareWasmStage
    const sBrMismatch = path.join(tmp, 'br-mismatch');
    populateValidStage(sBrMismatch, brand);
    const mismatchedWasm = buildMinimalWasmBinary({ shared: false });
    fs.writeFileSync(path.join(sBrMismatch, `${brand.productId}.wasm.br`), zlib.brotliCompressSync(mismatchedWasm));
    await assert.rejects(
      () => validateAndPrepareWasmStage(sBrMismatch, { requireThreads: true, autoBrotli: false }),
      /does not match/
    );

    // 13. Brotli helper rejects empty input
    const emptyFile = path.join(tmp, 'empty.wasm');
    fs.writeFileSync(emptyFile, '');
    await assert.rejects(() => compressFileBrotli(emptyFile, `${emptyFile}.br`, 9), /empty file/);
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
}

async function testDistWasmReleasePipelineCoexistence(brand) {
  const tmp = makeTempDir('rago-wasm-coexist-');
  try {
    const distDir = path.join(tmp, 'dist');
    const version = readDefaultCmakeVersion(repoRoot);
    fs.mkdirSync(distDir, { recursive: true });

    // Place desktop/mobile release packages in dist/ AND a WASM demo bundle in dist/wasm/
    const specs = [
      [`${brand.productId}-${version}-windows-x64-setup.exe`, 'MZ-win'],
      [`${brand.productId}-${version}-macos-arm64.dmg`, 'koly-mac'],
      [`${brand.productId}-${version}-linux-x64.AppImage`, 'ELF-lin'],
    ];
    for (const [name, body] of specs) {
      fs.writeFileSync(path.join(distDir, name), body, 'utf8');
    }

    const stage = path.join(tmp, 'stage');
    populateValidStage(stage, brand, { shared: true });
    await packageWasmDemo({
      stageDir: stage,
      outputPath: path.join(distDir, 'wasm', `${brand.productId}-wasm-demo.tar.gz`),
      requireThreads: true,
      requireVcpkgCopyrights: true,
    });

    // Both generateSbom and generateReleaseManifest must succeed cleanly with dist/wasm/ present!
    const sbomRes = generateSbom({
      repoRoot,
      version,
      distDir,
      autoDiscoverVcpkg: false,
    });
    assert.ok(fs.existsSync(sbomRes.outputPath));

    generateReleaseManifest({
      repoRoot,
      version,
      distDir,
      allowUnlistedChangelog: true,
      notesEn: 'WASM coexistence test',
    });
    assert.ok(fs.existsSync(path.join(distDir, 'releases.json')));
    verifyReleaseDirectory({ repoRoot, distDir, expectedVersion: version });
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
}

const brand = readBrandDefaults(repoRoot);
testBrandSingleSource(brand);
await testHappyPathAndDeterminism(brand);
await testNegativeSecurityAndCspGuards(brand);
await testDistWasmReleasePipelineCoexistence(brand);

// Test Ed25519/Minisign remote WASM sync verifier (verify-remote-manifest.mjs verify-demo)
{
  const tmpSync = makeTempDir('rago-wasm-sync-');
  try {
    const version = readDefaultCmakeVersion(repoRoot);
    const tag = `v${version}`;
    const urls = buildWasmDemoSyncUrls(tag, repoRoot);
    assert.equal(urls.archiveName, `${brand.productId}-wasm-demo.tar.gz`);
    assert.ok(urls.archiveUrl.endsWith(`/${tag}/${brand.productId}-wasm-demo.tar.gz`));
    assert.ok(urls.minisigUrl.endsWith(`/${tag}/${brand.productId}-wasm-demo.tar.gz.minisig`));

    const stage = path.join(tmpSync, 'stage');
    populateValidStage(stage, brand, { shared: true });
    const archivePath = path.join(tmpSync, urls.archiveName);
    await packageWasmDemo({
      stageDir: stage,
      outputPath: archivePath,
      requireThreads: true,
      requireVcpkgCopyrights: true,
    });

    const kp = generateEphemeralTestKeyPair('wasm-sync-test');
    const ts = new Date().toISOString().replace(/\.\d{3}Z$/, 'Z');
    const archiveBytes = fs.readFileSync(archivePath);
    const sigPath = `${archivePath}.minisig`;
    fs.writeFileSync(
      sigPath,
      signBufferWithTestKey(archiveBytes, kp, {
        untrustedComment: 'wasm demo sig',
        trustedComment: `release:${version}\ttimestamp:${ts}\tfile:${urls.archiveName}`,
      }),
      'utf8'
    );

    const okRes = await verifySignedWasmDemoFiles({
      archivePath,
      sigPath,
      expectedTag: tag,
      repoRoot,
      allowTestPubkey: true,
      pubKeyOverride: kp.base64,
    });
    assert.equal(okRes.tag, tag);

    // Negative: wrong version in trusted comment must fail (replay guard)
    await assert.rejects(
      () =>
        verifySignedWasmDemoFiles({
          archivePath,
          sigPath,
          expectedTag: 'v9.9.9',
          repoRoot,
          allowTestPubkey: true,
          pubKeyOverride: kp.base64,
        }),
      /release mismatch/
    );

    // Negative: cross-file signature swap (e.g. releases.json signature attached to wasm-demo.tar.gz) must fail
    const swappedSigPath = `${archivePath}.swapped.minisig`;
    fs.writeFileSync(
      swappedSigPath,
      signBufferWithTestKey(archiveBytes, kp, {
        untrustedComment: 'swapped sig',
        trustedComment: `release:${version}\ttimestamp:${ts}\tfile:releases.json`,
      }),
      'utf8'
    );
    await assert.rejects(
      () =>
        verifySignedWasmDemoFiles({
          archivePath,
          sigPath: swappedSigPath,
          expectedTag: tag,
          repoRoot,
          allowTestPubkey: true,
          pubKeyOverride: kp.base64,
        }),
      /file mismatch/
    );

    // Negative: downgrade against newer live manifest must fail unless allowRollback=true
    const newerLiveManifest = path.join(tmpSync, 'live-releases.json');
    fs.writeFileSync(newerLiveManifest, JSON.stringify({ latestVersion: '99.0.0' }), 'utf8');
    await assert.rejects(
      () =>
        verifySignedWasmDemoFiles({
          archivePath,
          sigPath,
          expectedTag: tag,
          repoRoot,
          currentManifestPath: newerLiveManifest,
          allowTestPubkey: true,
          pubKeyOverride: kp.base64,
        }),
      /Refusing WASM demo downgrade/
    );
    const rollbackOk = await verifySignedWasmDemoFiles({
      archivePath,
      sigPath,
      expectedTag: tag,
      repoRoot,
      currentManifestPath: newerLiveManifest,
      allowRollback: true,
      allowTestPubkey: true,
      pubKeyOverride: kp.base64,
    });
    assert.equal(rollbackOk.previousVersion, '99.0.0');
  } finally {
    fs.rmSync(tmpSync, { recursive: true, force: true });
  }
}

process.stdout.write('==> test-wasm-bundle.mjs: all WASM loader CSP, USTAR archive, Minisign sync, and coexistence tests passed\n');
