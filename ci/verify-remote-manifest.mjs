#!/usr/bin/env node
/**
 * Cryptographic & anti-downgrade verifier for syncing signed `releases.json` + `releases.json.minisig`
 * from GitHub Releases onto the origin static `/srv/site` directory via Gitea Actions (`docs/ReleasePipeline.md` §9).
 *
 * Zero external dependencies. Reads brand, domain, GitHub owner, and Minisign public key strictly
 * from `cmake/RagoShellDefaults.cmake`.
 */

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

import {
  compareSemVer,
  normalizeAndValidateVersion,
  readBrandDefaults,
  validateManifestObject,
} from './generate-releases-json.mjs';
import { verifyMinisigFile } from './lib/minisign.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const DEFAULT_REPO_ROOT = path.resolve(__dirname, '..');

const STABLE_RELEASE_TAG_RE = /^v(0|[1-9]\d{0,3})\.(0|[1-9]\d{0,3})\.(0|[1-9]\d{0,3})$/;
const REQUIRED_DESKTOP_PLATFORMS = ['windows', 'macos', 'linux'];

/**
 * Validates that `rawTag` is a strict stable `vMAJOR.MINOR.PATCH` release tag (never a pre-release).
 */
export function normalizeStableReleaseTag(rawTag) {
  if (typeof rawTag !== 'string') {
    throw new Error('Release tag must be a non-empty string.');
  }
  const trimmed = rawTag.trim();
  const match = STABLE_RELEASE_TAG_RE.exec(trimmed);
  if (!match) {
    throw new Error(
      `Refusing non-stable or invalid release tag "${trimmed}" (expected strict stable vMAJOR.MINOR.PATCH; pre-releases never update root releases.json).`
    );
  }
  const version = trimmed.slice(1);
  return { tag: trimmed, version };
}

/**
 * Builds canonical GitHub Releases download URLs and live manifest URL from single-source brand defaults.
 */
export function buildReleaseSyncUrls(rawTag = '', repoRoot = DEFAULT_REPO_ROOT) {
  const brand = readBrandDefaults(repoRoot);
  const githubRepo = `${brand.githubOwner}/${brand.productId}`;
  const latestApiUrl = `https://api.github.com/repos/${githubRepo}/releases/latest`;
  const liveManifestUrl = `${brand.defaultDlBaseUrl}/releases.json`;

  if (!rawTag) {
    return { brand, githubRepo, latestApiUrl, liveManifestUrl };
  }

  const { tag, version } = normalizeStableReleaseTag(rawTag);
  const baseDownloadUrl = `https://github.com/${githubRepo}/releases/download/${tag}`;
  return {
    brand,
    githubRepo,
    tag,
    version,
    latestApiUrl,
    liveManifestUrl,
    manifestUrl: `${baseDownloadUrl}/releases.json`,
    minisigUrl: `${baseDownloadUrl}/releases.json.minisig`,
  };
}

function assertRegularFileWithinBounds(filePath, maxBytes, label) {
  const resolved = path.resolve(filePath);
  let st;
  try {
    st = fs.lstatSync(resolved);
  } catch {
    throw new Error(`${label} file does not exist: ${resolved}`);
  }
  if (st.isSymbolicLink() || !st.isFile()) {
    throw new Error(`${label} must be a regular file (symlinks and special files refused): ${resolved}`);
  }
  if (st.size <= 0 || st.size > maxBytes) {
    throw new Error(`${label} file size (${st.size} bytes) is outside allowed bounds (1..${maxBytes}).`);
  }
  return resolved;
}

/**
 * Cryptographically verifies downloaded `releases.json` + `releases.json.minisig` before SSH deployment.
 */
export function verifySignedManifestFiles(options = {}) {
  const {
    manifestPath,
    sigPath,
    expectedTag,
    repoRoot = DEFAULT_REPO_ROOT,
    currentManifestPath = '',
    allowRollback = false,
    allowTestPubkey = false,
    pubKeyOverride = '',
    checkClockSkew = true,
  } = options;

  const { tag, version } = normalizeStableReleaseTag(expectedTag);
  const brand = readBrandDefaults(repoRoot);

  let pubKey = brand.minisignPubKey;
  if (pubKeyOverride) {
    if (!allowTestPubkey) {
      throw new Error('pubKeyOverride is forbidden unless allowTestPubkey is explicitly enabled for unit tests.');
    }
    pubKey = pubKeyOverride;
  }
  if (!pubKey) {
    throw new Error('RAGO_MINISIGN_PUBKEY is empty in cmake/RagoShellDefaults.cmake.');
  }

  const resolvedManifest = assertRegularFileWithinBounds(manifestPath, 65536, 'releases.json');
  const resolvedSig = assertRegularFileWithinBounds(sigPath, 8192, 'releases.json.minisig');

  // 1. Cryptographic Ed25519 + BLAKE2b-512 + signed trusted comment verification.
  const sigResult = verifyMinisigFile(resolvedManifest, resolvedSig, pubKey, {
    expectedVersion: version,
    expectedFile: 'releases.json',
    checkClockSkew,
  });

  // 2. Strict JSON schema & URL host allowlist verification.
  let manifest;
  try {
    manifest = JSON.parse(fs.readFileSync(resolvedManifest, 'utf8'));
  } catch (err) {
    throw new Error(`Invalid JSON in releases.json: ${err instanceof Error ? err.message : String(err)}`);
  }
  validateManifestObject(manifest, brand.allowedHosts);

  if (manifest.latestVersion !== version) {
    throw new Error(
      `Manifest latestVersion ("${manifest.latestVersion}") does not match expected tag ${tag} ("${version}").`
    );
  }
  if (manifest.latestVersion.includes('-')) {
    throw new Error(`Pre-release manifest "${manifest.latestVersion}" must never replace root releases.json.`);
  }

  for (const plat of REQUIRED_DESKTOP_PLATFORMS) {
    if (!manifest.downloads || !manifest.downloads[plat]) {
      throw new Error(`Manifest is missing required desktop download entry "${plat}".`);
    }
  }

  // 3. Anti-downgrade guard against currently live manifest (unless operator explicitly set allowRollback).
  let previousVersion = null;
  if (currentManifestPath) {
    const resolvedCurrent = path.resolve(currentManifestPath);
    if (fs.existsSync(resolvedCurrent)) {
      const curStat = fs.lstatSync(resolvedCurrent);
      if (curStat.isSymbolicLink() || !curStat.isFile()) {
        throw new Error(`Current live manifest snapshot is not a regular file: ${resolvedCurrent}`);
      }
      if (curStat.size > 0 && curStat.size <= 65536) {
        const curObj = JSON.parse(fs.readFileSync(resolvedCurrent, 'utf8'));
        if (curObj && typeof curObj.latestVersion === 'string') {
          previousVersion = normalizeAndValidateVersion(curObj.latestVersion, 'current.latestVersion');
          if (!allowRollback && compareSemVer(version, previousVersion) < 0) {
            throw new Error(
              `Refusing version downgrade: requested ${version} is older than live ${previousVersion} (pass --allow-rollback to override intentionally).`
            );
          }
        }
      }
    }
  }

  return {
    tag,
    version,
    previousVersion,
    keyIdHex: sigResult.keyIdHex,
    timestamp: sigResult.tokens.timestamp,
  };
}

function parseArgs(argv) {
  const out = { cmd: argv[0] || '', flags: Object.create(null) };
  for (let i = 1; i < argv.length; i += 1) {
    const arg = argv[i];
    if (!arg.startsWith('--')) {
      throw new Error(`Unexpected positional argument "${arg}".`);
    }
    const key = arg.slice(2);
    const next = argv[i + 1];
    if (next === undefined || next.startsWith('--')) {
      out.flags[key] = 'true';
    } else {
      out.flags[key] = next;
      i += 1;
    }
  }
  return out;
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  try {
    const { cmd, flags } = parseArgs(process.argv.slice(2));
    if (cmd === 'print-urls') {
      const info = buildReleaseSyncUrls(flags.tag || '', flags['repo-root'] || DEFAULT_REPO_ROOT);
      process.stdout.write(`${JSON.stringify(info)}\n`);
    } else if (cmd === 'verify') {
      if (!flags.manifest || !flags.sig || !flags.tag) {
        throw new Error(
          'Usage: node ci/verify-remote-manifest.mjs verify --manifest <releases.json> --sig <releases.json.minisig> --tag <vX.Y.Z> [--current-manifest <live.json>] [--allow-rollback]'
        );
      }
      const res = verifySignedManifestFiles({
        manifestPath: flags.manifest,
        sigPath: flags.sig,
        expectedTag: flags.tag,
        repoRoot: flags['repo-root'] || DEFAULT_REPO_ROOT,
        currentManifestPath: flags['current-manifest'] || '',
        allowRollback: flags['allow-rollback'] === 'true',
      });
      process.stdout.write(
        `[release-sync-verify] OK: ${res.tag} (keyId=${res.keyIdHex}, signedAt=${res.timestamp}, live=${res.previousVersion || 'none'})\n`
      );
    } else {
      throw new Error(`Unknown command "${cmd}". Expected: print-urls | verify`);
    }
  } catch (err) {
    process.stderr.write(`error: ${err instanceof Error ? err.message : String(err)}\n`);
    process.exit(1);
  }
}
