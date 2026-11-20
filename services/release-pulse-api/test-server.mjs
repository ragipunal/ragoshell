#!/usr/bin/env node
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { createReleasePulseServer } from './server.mjs';

const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'rago-pulse-test-'));
const releasesFile = path.join(tmpDir, 'releases.json');
const dbFile = path.join(tmpDir, 'pulse.sqlite');
const adminToken = crypto.randomBytes(24).toString('hex');

fs.writeFileSync(releasesFile, JSON.stringify({
  latestVersion: '2.3.0',
  minSupportedVersion: '2.1.0',
  mandatory: false,
  publishedAt: '2026-10-30',
  allDownloadsUrl: 'https://dl.ragoshell.com.tr/releases/',
  releaseNotes: {
    en: 'MongoDB 8.0 performance improvements.',
    tr: 'MongoDB 8.0 performans iyileştirmeleri.',
  },
  downloads: {
    macos: {
      url: 'https://dl.ragoshell.com.tr/releases/2.3.0/ragoshell-2.3.0-macos-arm64.dmg',
      label: 'macOS',
      sha256: 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad',
      size: 73400320,
    },
    android: {
      url: 'https://play.google.com/store/apps/details?id=tr.com.ragoshell.app',
      label: 'Google Play',
    },
  },
  artifacts: [
    {
      name: 'ragoshell-2.3.0-macos-arm64.dmg',
      platform: 'macos',
      arch: 'arm64',
      format: 'dmg',
      primary: true,
      url: 'https://dl.ragoshell.com.tr/releases/2.3.0/ragoshell-2.3.0-macos-arm64.dmg',
      sha256: 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad',
      size: 73400320,
    },
    {
      name: 'ragoshell-2.3.0-android-arm64.apk',
      platform: 'android',
      arch: 'arm64',
      format: 'apk',
      primary: true,
      url: 'https://dl.ragoshell.com.tr/releases/2.3.0/ragoshell-2.3.0-android-arm64.apk',
      sha256: 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855',
      size: 41943040,
    },
    {
      name: 'ragoshell-2.3.0-android-arm64.aab',
      platform: 'android',
      arch: 'arm64',
      format: 'aab',
      primary: false,
      url: 'https://dl.ragoshell.com.tr/releases/2.3.0/ragoshell-2.3.0-android-arm64.aab',
      sha256: '2c26b46b68ffc68ff99b453c1d30413413422d706483bfa0f98a5e886266e7ae',
      size: 39845888,
    },
  ],
}, null, 2));

const service = createReleasePulseServer({
  dbPath: dbFile,
  releasesPath: releasesFile,
  adminToken,
});

await new Promise((resolve) => service.server.listen(0, '127.0.0.1', resolve));
const { port } = service.server.address();
const baseUrl = `http://127.0.0.1:${port}`;

try {
  // 1. Health check & security headers
  const healthRes = await fetch(`${baseUrl}/v1/health`);
  assert.equal(healthRes.status, 200);
  assert.equal(healthRes.headers.get('x-content-type-options'), 'nosniff');
  assert.equal(healthRes.headers.get('x-frame-options'), 'DENY');
  const healthJson = await healthRes.json();
  assert.equal(healthJson.latestVersion, '2.3.0');

  // 2. Disallowed HTTP method returns 405
  const putRes = await fetch(`${baseUrl}/v1/health`, { method: 'DELETE' });
  assert.equal(putRes.status, 405);

  // 3. Release check with anonymous usage pulse
  const installA = 'a'.repeat(32);
  const pulseRes = await fetch(`${baseUrl}/v1/release-check`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      schemaVersion: 1,
      appVersion: '2.0.0',
      platform: 'macos',
      arch: 'arm64',
      formFactor: 'desktop',
      osVersion: '15.4',
      uiLanguage: 'tr',
      usage: {
        installId: installA,
        launchCount: 7,
        lastSessionSec: 940,
        savedConnections: 4,
        sshConnections: 2,
        tlsConnections: 3,
        replicaSetConnections: 1,
        shellsOpened: 19,
        queriesExecuted: 84,
        aiEnabled: true,
        uiTheme: 'Dark',
        uiDensity: 'Compact',
        dbVersions: ['6.0', '7.0'],
      },
    }),
  });
  assert.equal(pulseRes.status, 200);
  const pulseJson = await pulseRes.json();
  assert.equal(pulseJson.latestVersion, '2.3.0');
  assert.equal(pulseJson.minSupportedVersion, '2.1.0');
  assert.equal(pulseJson.downloads.macos.label, 'macOS');
  assert.equal(pulseJson.downloads.macos.sha256, 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
  assert.equal(pulseJson.downloads.macos.size, 73400320);
  assert.equal(pulseJson.artifacts.length, 3);
  assert.equal(pulseJson.artifacts.some((a) => a.platform === 'android' && a.format === 'aab' && a.primary === false), true);

  // 4. Release check WITHOUT usage stats (privacy opt-out)
  const noUsageRes = await fetch(`${baseUrl}/v1/release-check`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      schemaVersion: 1,
      appVersion: '2.2.0',
      platform: 'android',
      arch: 'arm64',
      formFactor: 'phone',
      osVersion: '16',
      uiLanguage: 'en',
    }),
  });
  assert.equal(noUsageRes.status, 200);

  // 5. Admin endpoint rejects missing/wrong token
  const unauthRes = await fetch(`${baseUrl}/v1/admin/stats`);
  assert.equal(unauthRes.status, 401);
  const wrongTokenRes = await fetch(`${baseUrl}/v1/admin/stats`, {
    headers: { Authorization: 'Bearer wrong-token' },
  });
  assert.equal(wrongTokenRes.status, 401);

  // 6. Admin endpoint returns aggregated statistics
  const statsRes = await fetch(`${baseUrl}/v1/admin/stats`, {
    headers: { Authorization: `Bearer ${adminToken}` },
  });
  assert.equal(statsRes.status, 200);
  const stats = await statsRes.json();
  assert.equal(stats.requests.totalChecks, 2);
  assert.equal(stats.requests.checksWithAnonymousUsage, 1);
  assert.equal(stats.activeInstalls.dau, 1);
  assert.equal(stats.activeInstalls.mau, 1);
  assert.equal(stats.engagement.totalLaunches, 7);
  assert.equal(stats.engagement.totalQueriesExecuted, 84);
  assert.equal(stats.featureAdoption.sshUsers, 1);
  assert.equal(stats.featureAdoption.aiEnabledUsers, 1);
  assert.equal(stats.distributions.byMongoVersion.length, 2);

  // 7. Live release rule update (mandatory toggle)
  const updateManifestRes = await fetch(`${baseUrl}/v1/admin/releases`, {
    method: 'POST',
    headers: {
      Authorization: `Bearer ${adminToken}`,
      'Content-Type': 'application/json',
    },
    body: JSON.stringify({
      latestVersion: '2.4.0',
      minSupportedVersion: '2.4.0',
      mandatory: true,
      publishedAt: '2026-10-30',
      allDownloadsUrl: 'https://dl.ragoshell.com.tr/releases/',
      releaseNotes: { en: 'Mandatory security update.' },
      downloads: pulseJson.downloads,
    }),
  });
  assert.equal(updateManifestRes.status, 200);

  const afterMandRes = await fetch(`${baseUrl}/v1/release-check`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ appVersion: '2.3.0', platform: 'macos' }),
  });
  const afterMandJson = await afterMandRes.json();
  assert.equal(afterMandJson.latestVersion, '2.4.0');
  assert.equal(afterMandJson.mandatory, true);

  // 8. Disallowed download host rejected with 400
  const badHostRes = await fetch(`${baseUrl}/v1/admin/releases`, {
    method: 'POST',
    headers: {
      Authorization: `Bearer ${adminToken}`,
      'Content-Type': 'application/json',
    },
    body: JSON.stringify({
      latestVersion: '2.4.1',
      minSupportedVersion: '2.4.0',
      mandatory: true,
      publishedAt: '2026-10-31',
      allDownloadsUrl: 'https://dl.ragoshell.com.tr/releases/',
      releaseNotes: { en: 'Evil redirect attempt.' },
      downloads: {
        macos: { url: 'https://evil.example/payload.dmg', label: 'macOS' },
      },
    }),
  });
  assert.equal(badHostRes.status, 400);

  console.log('release-pulse-api self-test passed.');
} finally {
  await service.close();
  fs.rmSync(tmpDir, { recursive: true, force: true });
}
