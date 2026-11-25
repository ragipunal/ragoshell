#!/usr/bin/env node
/**
 * Release tag preflight verifier (`docs/VersioningAndReleases.md`).
 *
 * Ensures a release tag (`vMAJOR.MINOR.PATCH` or `vMAJOR.MINOR.PATCH-(alpha|beta|rc).N`)
 * follows strict SemVer 2.0.0 rules and matches both `CMakeLists.txt` `PROJECT_VERSION_*`
 * and a populated `CHANGELOG` release section before 4-platform release builds start.
 */

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { normalizeAndValidateVersion, parseChangelog, readBrandDefaults } from './generate-releases-json.mjs';

const STRICT_RELEASE_TAG_RE =
  /^v(0|[1-9]\d{0,3})\.(0|[1-9]\d{0,3})\.(0|[1-9]\d{0,3})(?:-(?:alpha|beta|rc)\.(?:0|[1-9]\d*))?$/;

function readCmakeProjectVersion(cmakeListsPath) {
  const text = fs.readFileSync(cmakeListsPath, 'utf8');
  const get = (key) => {
    const m = text.match(new RegExp(`^\\s*set\\(${key}\\s+"([^"]*)"\\)`, 'm'));
    if (!m) {
      throw new Error(`Missing ${key} in ${cmakeListsPath}`);
    }
    return m[1];
  };
  return {
    major: get('PROJECT_VERSION_MAJOR'),
    minor: get('PROJECT_VERSION_MINOR'),
    patch: get('PROJECT_VERSION_PATCH'),
    build: get('PROJECT_VERSION_BUILD'),
  };
}

export function verifyReleaseTag(options = {}) {
  const repoRoot = path.resolve(options.repoRoot || path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..'));
  const rawTag = String(options.tag ?? process.env.RELEASE_TAG ?? process.env.GITHUB_REF_NAME ?? '').trim();

  if (!rawTag) {
    throw new Error('Release tag is required (pass as CLI argument or GITHUB_REF_NAME).');
  }
  const m = STRICT_RELEASE_TAG_RE.exec(rawTag);
  if (!m) {
    throw new Error(
      `Invalid release tag "${rawTag}": expected strict SemVer tag vMAJOR.MINOR.PATCH or vMAJOR.MINOR.PATCH-(alpha|beta|rc).N.`
    );
  }

  const version = normalizeAndValidateVersion(rawTag, 'release tag');
  const baseVersion = `${m[1]}.${m[2]}.${m[3]}`;
  const isPrerelease = version.includes('-');

  const cmakeVer = readCmakeProjectVersion(path.join(repoRoot, 'CMakeLists.txt'));
  const cmakeCore = `${cmakeVer.major}.${cmakeVer.minor}.${cmakeVer.patch}`;
  if (cmakeCore !== baseVersion) {
    throw new Error(
      `Release tag "${rawTag}" (${baseVersion}) does not match CMakeLists.txt PROJECT_VERSION (${cmakeCore}).`
    );
  }
  if (!isPrerelease && cmakeVer.build !== '') {
    throw new Error(
      `Stable release tag "${rawTag}" requires empty PROJECT_VERSION_BUILD in CMakeLists.txt (found "${cmakeVer.build}").`
    );
  }

  const brand = readBrandDefaults(repoRoot);
  const changelogPath = path.join(repoRoot, 'CHANGELOG');
  const entries = parseChangelog(fs.readFileSync(changelogPath, 'utf8'));
  const matched =
    entries.find((e) => e.version === version) ||
    (isPrerelease ? entries.find((e) => e.version === baseVersion) : null);
  if (!matched) {
    throw new Error(
      `Version ${isPrerelease ? `${version} (or ${baseVersion})` : version} not found in CHANGELOG. Add a "${brand.productName} ${baseVersion}" section before tagging.`
    );
  }

  return {
    tag: rawTag,
    version,
    baseVersion,
    isPrerelease,
    cmakeVersion: `${cmakeCore}${cmakeVer.build}`,
    changelogVersion: matched.version,
  };
}

const isDirectRun =
  process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url);

if (isDirectRun) {
  try {
    const tagArg = process.argv[2] || process.env.RELEASE_TAG || process.env.GITHUB_REF_NAME || '';
    const info = verifyReleaseTag({ tag: tagArg });
    console.log(
      `release-tag check passed: ${info.tag} (CMake ${info.cmakeVersion}, CHANGELOG ${info.changelogVersion}, ${info.isPrerelease ? 'prerelease' : 'stable'})`
    );
  } catch (err) {
    console.error(`error: ${err instanceof Error ? err.message : String(err)}`);
    process.exit(1);
  }
}
