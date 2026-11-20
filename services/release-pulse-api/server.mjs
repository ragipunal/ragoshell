#!/usr/bin/env node
/**
 * RagoShell Release Check & Anonymous Usage Pulse API
 * Zero external dependencies — built strictly on Node.js standard modules
 * (node:http, node:sqlite, node:crypto, node:fs, node:path).
 *
 * Security guarantees:
 * - Binds to 127.0.0.1 by default (override via PULSE_BIND_HOST behind TLS reverse proxy).
 * - HTTP method allow-list: strictly GET and POST (405 on PUT/DELETE/TRACE/etc.).
 * - 100% parameterized SQLite (WAL) prepared statements; zero SQL string interpolation.
 * - Ephemeral salted IP hashing for rate limiting; raw IP addresses are NEVER stored or logged.
 * - Constant-time Bearer token check (crypto.timingSafeEqual) with multi-tiered secret resolution.
 * - Strict CSP, X-Content-Type-Options, X-Frame-Options, Permissions-Policy, and Cache-Control headers.
 */

import crypto from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { DatabaseSync } from 'node:sqlite';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

const MAX_BODY_BYTES = 16 * 1024;
const ALLOWED_PLATFORMS = new Set(['windows', 'macos', 'linux', 'android', 'ios']);
const ALLOWED_FORM_FACTORS = new Set(['desktop', 'tablet', 'phone']);
const SEMVER_RE = /^v?\d{1,4}(?:\.\d{1,4}){0,3}(?:-[0-9A-Za-z.-]{1,32})?$/;
const STRICT_MANIFEST_SEMVER_RE = /^(?:0|[1-9]\d{0,3})\.(?:0|[1-9]\d{0,3})\.(?:0|[1-9]\d{0,3})(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$/;
const SAFE_TOKEN_RE = /^[0-9A-Za-z._+-]{1,32}$/;
const SAFE_ARTIFACT_NAME_RE = /^[0-9A-Za-z][0-9A-Za-z._+-]{0,127}$/;
const SHA256_RE = /^[0-9a-f]{64}$/;
const INSTALL_ID_RE = /^[0-9a-f]{32}$/;
const DB_VER_RE = /^\d{1,2}\.\d{1,2}$/;

function resolveDefaultDomain() {
  const defaultsFile = path.resolve(__dirname, '..', '..', 'cmake', 'RagoShellDefaults.cmake');
  try {
    if (fs.existsSync(defaultsFile)) {
      const text = fs.readFileSync(defaultsFile, 'utf8');
      const m = /^set\(RAGO_DOMAIN\s+"([^"]+)"\)/m.exec(text);
      if (m && SAFE_TOKEN_RE.test(m[1])) {
        return m[1];
      }
    }
  } catch {
    // Standalone container deployments may not carry the CMake directory.
  }
  return 'ragoshell.com.tr';
}

const DEFAULT_DOMAIN = resolveDefaultDomain();
const DEFAULT_ALL_DOWNLOADS_URL = `https://dl.${DEFAULT_DOMAIN}/releases/`;
const DEFAULT_ALLOWED_DOWNLOAD_HOSTS = new Set([
  `dl.${DEFAULT_DOMAIN}`,
  'play.google.com',
  'apps.apple.com',
]);
const DEFAULT_ALLOWED_PAGE_HOSTS = new Set([
  `dl.${DEFAULT_DOMAIN}`,
  DEFAULT_DOMAIN,
  `www.${DEFAULT_DOMAIN}`,
]);

// TODO(security): Admin endpoints use stateless Bearer token authentication (no cookies, so CSRF
// does not apply). Consider OAuth 2.0 / OIDC + MFA if a browser-based admin console is added.
// TODO(security): SQLite runs embedded in-process under OS file permissions; if migrated to an
// external RDBMS (PostgreSQL/MySQL), enforce mTLS and least-privilege DB roles.

function resolveAdminToken(explicitToken) {
  if (typeof explicitToken === 'string' && explicitToken.trim().length >= 16) {
    return explicitToken.trim();
  }
  if (process.env.PULSE_ADMIN_TOKEN && process.env.PULSE_ADMIN_TOKEN.trim().length >= 16) {
    return process.env.PULSE_ADMIN_TOKEN.trim();
  }
  if (process.env.PULSE_ADMIN_TOKEN_FILE) {
    const resolvedFile = path.resolve(process.env.PULSE_ADMIN_TOKEN_FILE);
    if (fs.existsSync(resolvedFile)) {
      const fromFile = fs.readFileSync(resolvedFile, 'utf8').trim();
      if (fromFile.length >= 16) {
        return fromFile;
      }
    }
  }
  if (process.env.NODE_ENV === 'production') {
    throw new Error('PULSE_ADMIN_TOKEN (>= 16 chars) or PULSE_ADMIN_TOKEN_FILE is required in production.');
  }
  console.warn('[release-pulse-api] WARNING: PULSE_ADMIN_TOKEN not set; generated ephemeral instance-isolated admin token.');
  return crypto.randomBytes(32).toString('hex');
}

function verifyBearerToken(authHeader, expectedSecret) {
  if (typeof authHeader !== 'string' || !authHeader.startsWith('Bearer ')) {
    return false;
  }
  const presented = authHeader.slice(7).trim();
  if (!presented) {
    return false;
  }
  const expectedDigest = crypto.createHash('sha256').update(expectedSecret, 'utf8').digest();
  const presentedDigest = crypto.createHash('sha256').update(presented, 'utf8').digest();
  return crypto.timingSafeEqual(expectedDigest, presentedDigest);
}

function isSafeHttpsUrl(value, allowedHostSet = null) {
  if (typeof value !== 'string' || value.length === 0 || value.length > 1024 || /[\x00-\x20\x7f\\]/.test(value)) {
    return false;
  }
  const schemeEnd = value.indexOf('://');
  if (schemeEnd > 0) {
    const afterHostSlash = value.indexOf('/', schemeEnd + 3);
    if (afterHostSlash >= 0) {
      const rawPath = value.slice(afterHostSlash).split(/[?#]/, 1)[0];
      if (/(?:^|\/)\.\.(?:\/|$)/.test(rawPath) || /%2e%2e/i.test(rawPath) || rawPath.includes('//')) {
        return false;
      }
    }
  }
  try {
    const parsed = new URL(value);
    if (parsed.protocol !== 'https:' || parsed.username !== '' || parsed.password !== '' || parsed.hostname.length === 0) {
      return false;
    }
    if (allowedHostSet && !allowedHostSet.has(parsed.hostname)) {
      return false;
    }
    return true;
  } catch {
    return false;
  }
}

function isStrictManifestSemVer(ver) {
  return typeof ver === 'string' && ver.length > 0 && ver.length <= 32 && !ver.includes('..') && STRICT_MANIFEST_SEMVER_RE.test(ver);
}

function validateReleasesManifest(raw, allowedHosts = DEFAULT_ALLOWED_DOWNLOAD_HOSTS, allowedPageHosts = DEFAULT_ALLOWED_PAGE_HOSTS) {
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) {
    throw new Error('Manifest must be a JSON object.');
  }
  const schemaVersion = raw.schemaVersion === undefined ? 1 : raw.schemaVersion;
  if (schemaVersion !== 1) {
    throw new Error('Unsupported schemaVersion (expected 1).');
  }
  const latestVersion = typeof raw.latestVersion === 'string' ? raw.latestVersion.trim() : '';
  if (!isStrictManifestSemVer(latestVersion)) {
    throw new Error('Invalid latestVersion (expected strict MAJOR.MINOR.PATCH[-PRERELEASE]).');
  }
  const minSupportedVersion = typeof raw.minSupportedVersion === 'string' ? raw.minSupportedVersion.trim() : '';
  if (minSupportedVersion && !isStrictManifestSemVer(minSupportedVersion)) {
    throw new Error('Invalid minSupportedVersion.');
  }
  const allDownloadsUrl = typeof raw.allDownloadsUrl === 'string' ? raw.allDownloadsUrl.trim() : '';
  if (allDownloadsUrl && !isSafeHttpsUrl(allDownloadsUrl, allowedPageHosts)) {
    throw new Error('allDownloadsUrl must be an allowlisted https:// URL.');
  }

  const cleanDownloads = {};
  if (raw.downloads && typeof raw.downloads === 'object' && !Array.isArray(raw.downloads)) {
    for (const [plat, entry] of Object.entries(raw.downloads)) {
      if (!ALLOWED_PLATFORMS.has(plat)) {
        throw new Error(`Unknown platform "${plat}" in downloads.`);
      }
      if (!entry || typeof entry !== 'object' || Array.isArray(entry)) {
        throw new Error(`Invalid downloads.${plat} entry.`);
      }
      const trimmedUrl = typeof entry.url === 'string' ? entry.url.trim() : '';
      if (!isSafeHttpsUrl(trimmedUrl, allowedHosts)) {
        throw new Error(`Disallowed or unsafe URL in downloads.${plat}.url.`);
      }
      if (trimmedUrl) {
        let sha256;
        if (entry.sha256 !== undefined) {
          if (typeof entry.sha256 !== 'string' || !SHA256_RE.test(entry.sha256.trim())) {
            throw new Error(`Invalid sha256 in downloads.${plat}.`);
          }
          sha256 = entry.sha256.trim();
        }
        let size;
        if (entry.size !== undefined) {
          if (!Number.isSafeInteger(entry.size) || entry.size <= 0) {
            throw new Error(`Invalid size in downloads.${plat}.`);
          }
          size = entry.size;
        }
        const dlEntry = {
          url: trimmedUrl,
        };
        if (typeof entry.label === 'string' && entry.label.trim().length > 0 && entry.label.trim().length <= 40) {
          if (/<\/?[a-z!][\s\S]*?>/i.test(entry.label)) {
            throw new Error(`Invalid HTML markup in downloads.${plat}.label.`);
          }
          dlEntry.label = entry.label.trim();
        }
        if (sha256 !== undefined) dlEntry.sha256 = sha256;
        if (size !== undefined) dlEntry.size = size;
        if (entry.latestVersion !== undefined) {
          const pv = typeof entry.latestVersion === 'string' ? entry.latestVersion.trim() : '';
          if (!isStrictManifestSemVer(pv)) {
            throw new Error(`Invalid latestVersion in downloads.${plat}.`);
          }
          dlEntry.latestVersion = pv;
        }
        if (entry.minSupportedVersion !== undefined) {
          const pm = typeof entry.minSupportedVersion === 'string' ? entry.minSupportedVersion.trim() : '';
          if (!isStrictManifestSemVer(pm)) {
            throw new Error(`Invalid minSupportedVersion in downloads.${plat}.`);
          }
          dlEntry.minSupportedVersion = pm;
        }
        if (typeof entry.mandatory === 'boolean') {
          dlEntry.mandatory = entry.mandatory;
        }
        cleanDownloads[plat] = dlEntry;
      }
    }
  }

  let cleanArtifacts;
  if (raw.artifacts !== undefined) {
    if (!Array.isArray(raw.artifacts) || raw.artifacts.length > 64) {
      throw new Error('artifacts must be an array of at most 64 items.');
    }
    cleanArtifacts = [];
    for (const item of raw.artifacts) {
      if (!item || typeof item !== 'object' || Array.isArray(item)) {
        throw new Error('Invalid artifact entry.');
      }
      const name = typeof item.name === 'string' ? item.name.trim() : '';
      const plat = typeof item.platform === 'string' ? item.platform.trim().toLowerCase() : '';
      const arch = typeof item.arch === 'string' ? item.arch.trim() : '';
      const format = typeof item.format === 'string' ? item.format.trim() : '';
      const url = typeof item.url === 'string' ? item.url.trim() : '';
      const sha256 = typeof item.sha256 === 'string' ? item.sha256.trim() : '';
      if (
        !SAFE_ARTIFACT_NAME_RE.test(name) ||
        name.includes('..') ||
        !ALLOWED_PLATFORMS.has(plat) ||
        !SAFE_TOKEN_RE.test(arch) ||
        !SAFE_TOKEN_RE.test(format) ||
        !isSafeHttpsUrl(url, allowedHosts) ||
        !SHA256_RE.test(sha256) ||
        !Number.isSafeInteger(item.size) ||
        item.size <= 0
      ) {
        throw new Error(`Invalid artifact metadata for "${name || 'unknown'}".`);
      }
      cleanArtifacts.push({
        name,
        platform: plat,
        arch,
        format,
        primary: Boolean(item.primary),
        url,
        sha256,
        size: item.size,
      });
    }
  }

  let cleanNotes = '';
  if (typeof raw.releaseNotes === 'string') {
    const trimmed = raw.releaseNotes.trim().slice(0, 2000);
    if (/<\/?[a-z!][\s\S]*?>/i.test(trimmed)) {
      throw new Error('Invalid HTML markup in releaseNotes.');
    }
    cleanNotes = trimmed;
  } else if (raw.releaseNotes && typeof raw.releaseNotes === 'object' && !Array.isArray(raw.releaseNotes)) {
    cleanNotes = Object.create(null);
    for (const [lang, val] of Object.entries(raw.releaseNotes)) {
      if (!/^[a-z]{2}(?:-[A-Z]{2})?$/.test(lang)) {
        throw new Error(`Invalid locale key "${lang}" in releaseNotes.`);
      }
      if (typeof val !== 'string' || val.trim().length === 0 || val.trim().length > 2000) {
        throw new Error(`Invalid note text for locale "${lang}".`);
      }
      if (/<\/?[a-z!][\s\S]*?>/i.test(val)) {
        throw new Error(`Invalid HTML markup in releaseNotes.${lang}.`);
      }
      cleanNotes[lang] = val.trim();
    }
  }

  let cleanSbom;
  if (raw.sbom !== undefined) {
    if (!raw.sbom || typeof raw.sbom !== 'object' || Array.isArray(raw.sbom)) {
      throw new Error('Invalid sbom entry.');
    }
    const sbomName = typeof raw.sbom.name === 'string' ? raw.sbom.name.trim() : '';
    const sbomFormat = typeof raw.sbom.format === 'string' ? raw.sbom.format.trim() : '';
    const sbomSpdxVersion = typeof raw.sbom.spdxVersion === 'string' ? raw.sbom.spdxVersion.trim() : '';
    const sbomUrl = typeof raw.sbom.url === 'string' ? raw.sbom.url.trim() : '';
    const sbomSha256 = typeof raw.sbom.sha256 === 'string' ? raw.sbom.sha256.trim() : '';
    if (
      !SAFE_ARTIFACT_NAME_RE.test(sbomName) ||
      !sbomName.endsWith('-sbom.spdx.json') ||
      sbomName.includes('..') ||
      sbomFormat !== 'spdx-json' ||
      sbomSpdxVersion !== 'SPDX-2.3' ||
      !isSafeHttpsUrl(sbomUrl, allowedHosts) ||
      !SHA256_RE.test(sbomSha256) ||
      !Number.isSafeInteger(raw.sbom.size) ||
      raw.sbom.size <= 0
    ) {
      throw new Error(`Invalid sbom metadata for "${sbomName || 'unknown'}".`);
    }
    cleanSbom = {
      name: sbomName,
      format: sbomFormat,
      spdxVersion: sbomSpdxVersion,
      url: sbomUrl,
      sha256: sbomSha256,
      size: raw.sbom.size,
    };
  }

  const out = {
    schemaVersion,
    latestVersion,
    minSupportedVersion,
    mandatory: Boolean(raw.mandatory),
    publishedAt: typeof raw.publishedAt === 'string' ? raw.publishedAt.trim().slice(0, 32) : '',
    allDownloadsUrl: allDownloadsUrl || DEFAULT_ALL_DOWNLOADS_URL,
    releaseNotes: cleanNotes,
    downloads: cleanDownloads,
  };
  if (cleanArtifacts !== undefined) {
    out.artifacts = cleanArtifacts;
  }
  if (cleanSbom !== undefined) {
    out.sbom = cleanSbom;
  }
  return out;
}

function clampNonNegInt(val, max = 1_000_000_000) {
  if (typeof val !== 'number' || !Number.isFinite(val)) return 0;
  return Math.min(max, Math.max(0, Math.trunc(val)));
}

function sanitizeString(val, maxLen, fallback = 'unknown') {
  if (typeof val !== 'string') return fallback;
  const trimmed = val.trim().slice(0, maxLen);
  return SAFE_TOKEN_RE.test(trimmed) ? trimmed : fallback;
}

function initDatabase(dbPath) {
  if (dbPath !== ':memory:') {
    fs.mkdirSync(path.dirname(path.resolve(dbPath)), { recursive: true });
  }
  const db = new DatabaseSync(dbPath);
  db.exec(`
    PRAGMA journal_mode = WAL;
    PRAGMA busy_timeout = 5000;
    PRAGMA foreign_keys = ON;

    CREATE TABLE IF NOT EXISTS installs (
      install_id TEXT PRIMARY KEY,
      first_seen_at TEXT NOT NULL,
      last_seen_at TEXT NOT NULL,
      app_version TEXT NOT NULL,
      platform TEXT NOT NULL,
      arch TEXT NOT NULL,
      form_factor TEXT NOT NULL,
      os_version TEXT NOT NULL,
      ui_language TEXT NOT NULL,
      ui_theme TEXT NOT NULL,
      ui_density TEXT NOT NULL,
      launch_count INTEGER NOT NULL DEFAULT 0,
      last_session_sec INTEGER NOT NULL DEFAULT 0,
      saved_connections INTEGER NOT NULL DEFAULT 0,
      ssh_connections INTEGER NOT NULL DEFAULT 0,
      tls_connections INTEGER NOT NULL DEFAULT 0,
      replica_set_connections INTEGER NOT NULL DEFAULT 0,
      shells_opened INTEGER NOT NULL DEFAULT 0,
      queries_executed INTEGER NOT NULL DEFAULT 0,
      ai_enabled INTEGER NOT NULL DEFAULT 0,
      db_versions_json TEXT NOT NULL DEFAULT '[]'
    );

    CREATE TABLE IF NOT EXISTS daily_active_installs (
      day_utc TEXT NOT NULL,
      install_id TEXT NOT NULL,
      platform TEXT NOT NULL,
      app_version TEXT NOT NULL,
      PRIMARY KEY (day_utc, install_id)
    );

    CREATE TABLE IF NOT EXISTS daily_requests (
      day_utc TEXT NOT NULL,
      platform TEXT NOT NULL,
      app_version TEXT NOT NULL,
      with_usage INTEGER NOT NULL,
      check_count INTEGER NOT NULL DEFAULT 0,
      PRIMARY KEY (day_utc, platform, app_version, with_usage)
    );

    CREATE INDEX IF NOT EXISTS idx_installs_last_seen ON installs(last_seen_at);
    CREATE INDEX IF NOT EXISTS idx_dai_day ON daily_active_installs(day_utc);
  `);
  return db;
}

export function createReleasePulseServer(options = {}) {
  const dbPath = options.dbPath || process.env.PULSE_DB_PATH || path.join(__dirname, 'data', 'pulse.sqlite');
  const releasesPath = options.releasesPath || process.env.PULSE_RELEASES_PATH || path.join(__dirname, 'releases.json');
  const adminToken = resolveAdminToken(options.adminToken);
  const allowedOrigin = options.allowedOrigin || process.env.PULSE_ALLOWED_ORIGIN || '';
  const rateLimitMaxPerMin = options.rateLimitMaxPerMin ?? 60;
  const allowedHosts = options.allowedHosts ? new Set(options.allowedHosts) : DEFAULT_ALLOWED_DOWNLOAD_HOSTS;
  const allowedPageHosts = options.allowedPageHosts ? new Set(options.allowedPageHosts) : DEFAULT_ALLOWED_PAGE_HOSTS;

  let manifest = validateReleasesManifest(
    JSON.parse(fs.readFileSync(path.resolve(releasesPath), 'utf8')),
    allowedHosts,
    allowedPageHosts
  );

  const db = initDatabase(dbPath);

  const stmtUpsertDailyReq = db.prepare(`
    INSERT INTO daily_requests (day_utc, platform, app_version, with_usage, check_count)
    VALUES (?, ?, ?, ?, 1)
    ON CONFLICT(day_utc, platform, app_version, with_usage)
    DO UPDATE SET check_count = check_count + 1
  `);

  const stmtUpsertInstall = db.prepare(`
    INSERT INTO installs (
      install_id, first_seen_at, last_seen_at, app_version, platform, arch, form_factor,
      os_version, ui_language, ui_theme, ui_density, launch_count, last_session_sec,
      saved_connections, ssh_connections, tls_connections, replica_set_connections,
      shells_opened, queries_executed, ai_enabled, db_versions_json
    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    ON CONFLICT(install_id) DO UPDATE SET
      last_seen_at = excluded.last_seen_at,
      app_version = excluded.app_version,
      platform = excluded.platform,
      arch = excluded.arch,
      form_factor = excluded.form_factor,
      os_version = excluded.os_version,
      ui_language = excluded.ui_language,
      ui_theme = excluded.ui_theme,
      ui_density = excluded.ui_density,
      launch_count = MAX(installs.launch_count, excluded.launch_count),
      last_session_sec = excluded.last_session_sec,
      saved_connections = excluded.saved_connections,
      ssh_connections = excluded.ssh_connections,
      tls_connections = excluded.tls_connections,
      replica_set_connections = excluded.replica_set_connections,
      shells_opened = MAX(installs.shells_opened, excluded.shells_opened),
      queries_executed = MAX(installs.queries_executed, excluded.queries_executed),
      ai_enabled = excluded.ai_enabled,
      db_versions_json = excluded.db_versions_json
  `);

  const stmtUpsertDailyActive = db.prepare(`
    INSERT INTO daily_active_installs (day_utc, install_id, platform, app_version)
    VALUES (?, ?, ?, ?)
    ON CONFLICT(day_utc, install_id) DO UPDATE SET
      platform = excluded.platform,
      app_version = excluded.app_version
  `);

  const stmtCountActiveSince = db.prepare(`
    SELECT COUNT(DISTINCT install_id) AS cnt FROM daily_active_installs WHERE day_utc >= ?
  `);
  const stmtTotals = db.prepare(`
    SELECT
      COUNT(*) AS total_installs,
      COALESCE(SUM(launch_count), 0) AS total_launches,
      COALESCE(ROUND(AVG(last_session_sec)), 0) AS avg_session_sec,
      COALESCE(SUM(saved_connections), 0) AS total_saved_connections,
      COALESCE(SUM(shells_opened), 0) AS total_shells_opened,
      COALESCE(SUM(queries_executed), 0) AS total_queries_executed,
      COALESCE(SUM(CASE WHEN ssh_connections > 0 THEN 1 ELSE 0 END), 0) AS ssh_users,
      COALESCE(SUM(CASE WHEN tls_connections > 0 THEN 1 ELSE 0 END), 0) AS tls_users,
      COALESCE(SUM(CASE WHEN replica_set_connections > 0 THEN 1 ELSE 0 END), 0) AS replica_set_users,
      COALESCE(SUM(CASE WHEN ai_enabled > 0 THEN 1 ELSE 0 END), 0) AS ai_enabled_users
    FROM installs
  `);
  const stmtByPlatform = db.prepare('SELECT platform AS key, COUNT(*) AS installs FROM installs GROUP BY platform ORDER BY installs DESC');
  const stmtByFormFactor = db.prepare('SELECT form_factor AS key, COUNT(*) AS installs FROM installs GROUP BY form_factor ORDER BY installs DESC');
  const stmtByArch = db.prepare('SELECT arch AS key, COUNT(*) AS installs FROM installs GROUP BY arch ORDER BY installs DESC');
  const stmtByVersion = db.prepare('SELECT app_version AS key, COUNT(*) AS installs FROM installs GROUP BY app_version ORDER BY installs DESC');
  const stmtByLanguage = db.prepare('SELECT ui_language AS key, COUNT(*) AS installs FROM installs GROUP BY ui_language ORDER BY installs DESC');
  const stmtByTheme = db.prepare('SELECT ui_theme AS key, COUNT(*) AS installs FROM installs GROUP BY ui_theme ORDER BY installs DESC');
  const stmtAllDbJson = db.prepare('SELECT db_versions_json FROM installs');
  const stmtDailyTrend = db.prepare(`
    SELECT day_utc AS day, COUNT(DISTINCT install_id) AS active_installs
    FROM daily_active_installs
    WHERE day_utc >= ?
    GROUP BY day_utc
    ORDER BY day_utc ASC
  `);
  const stmtRequestTotals = db.prepare(`
    SELECT
      COALESCE(SUM(check_count), 0) AS total_checks,
      COALESCE(SUM(CASE WHEN with_usage = 1 THEN check_count ELSE 0 END), 0) AS checks_with_usage
    FROM daily_requests
  `);

  // Privacy-preserving in-memory rate limiter (ephemeral random salt; raw IPs never stored)
  const rateSalt = crypto.randomBytes(16);
  const rateBuckets = new Map();

  function allowRequest(req) {
    const nowMin = Math.floor(Date.now() / 60_000);
    if (rateBuckets.size > 4096) {
      for (const [k, v] of rateBuckets.entries()) {
        if (v.minute < nowMin) rateBuckets.delete(k);
      }
    }
    const remote = req.socket?.remoteAddress || 'local';
    const bucketKey = crypto.createHmac('sha256', rateSalt).update(`${nowMin}:${remote}`).digest('hex').slice(0, 24);
    const entry = rateBuckets.get(bucketKey);
    if (!entry || entry.minute !== nowMin) {
      rateBuckets.set(bucketKey, { minute: nowMin, count: 1 });
      return true;
    }
    entry.count += 1;
    return entry.count <= rateLimitMaxPerMin;
  }

  function sendJson(req, res, statusCode, payload, extraHeaders = {}) {
    const headers = {
      'Content-Type': 'application/json; charset=utf-8',
      'Content-Security-Policy': "default-src 'none'; frame-ancestors 'none'",
      'X-Content-Type-Options': 'nosniff',
      'X-Frame-Options': 'DENY',
      'Permissions-Policy': 'camera=(), microphone=(), geolocation=()',
      'Referrer-Policy': 'no-referrer',
      'Cache-Control': 'no-store',
      ...extraHeaders,
    };
    if (allowedOrigin && req.headers.origin === allowedOrigin) {
      headers['Access-Control-Allow-Origin'] = allowedOrigin;
      headers['Vary'] = 'Origin';
    }
    const body = Buffer.from(JSON.stringify(payload), 'utf8');
    headers['Content-Length'] = String(body.byteLength);
    res.writeHead(statusCode, headers);
    res.end(body);
  }

  function readJsonBody(req) {
    return new Promise((resolve, reject) => {
      const contentType = String(req.headers['content-type'] || '').toLowerCase();
      if (!contentType.startsWith('application/json')) {
        reject(Object.assign(new Error('Expected application/json'), { statusCode: 415 }));
        return;
      }
      const chunks = [];
      let total = 0;
      req.on('data', (chunk) => {
        total += chunk.length;
        if (total > MAX_BODY_BYTES) {
          reject(Object.assign(new Error('Payload too large'), { statusCode: 413 }));
          req.destroy();
          return;
        }
        chunks.push(chunk);
      });
      req.on('error', (err) => reject(err));
      req.on('end', () => {
        try {
          const text = Buffer.concat(chunks).toString('utf8');
          const parsed = JSON.parse(text);
          if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
            reject(Object.assign(new Error('JSON root must be an object'), { statusCode: 400 }));
            return;
          }
          resolve(parsed);
        } catch {
          reject(Object.assign(new Error('Malformed JSON body'), { statusCode: 400 }));
        }
      });
    });
  }

  function utcDayOffset(daysAgo) {
    const d = new Date(Date.now() - daysAgo * 86_400_000);
    return d.toISOString().slice(0, 10);
  }

  function buildReleaseResponse(platform) {
    const platOverride = manifest.downloads[platform] || null;
    const latestVersion = platOverride?.latestVersion || manifest.latestVersion;
    const minSupportedVersion = platOverride?.minSupportedVersion || manifest.minSupportedVersion;
    const mandatory = typeof platOverride?.mandatory === 'boolean' ? platOverride.mandatory : manifest.mandatory;
    const res = {
      schemaVersion: manifest.schemaVersion ?? 1,
      latestVersion,
      minSupportedVersion,
      mandatory,
      publishedAt: manifest.publishedAt,
      allDownloadsUrl: manifest.allDownloadsUrl,
      releaseNotes: manifest.releaseNotes,
      downloads: manifest.downloads,
    };
    if (manifest.artifacts !== undefined) {
      res.artifacts = manifest.artifacts;
    }
    return res;
  }

  function computeAdminStats() {
    const today = utcDayOffset(0);
    const sevenDaysAgo = utcDayOffset(6);
    const thirtyDaysAgo = utcDayOffset(29);

    const dau = Number(stmtCountActiveSince.get(today)?.cnt ?? 0);
    const wau = Number(stmtCountActiveSince.get(sevenDaysAgo)?.cnt ?? 0);
    const mau = Number(stmtCountActiveSince.get(thirtyDaysAgo)?.cnt ?? 0);
    const totals = stmtTotals.get();
    const reqTotals = stmtRequestTotals.get();

    const mongoCounts = new Map();
    for (const row of stmtAllDbJson.all()) {
      try {
        const arr = JSON.parse(row.db_versions_json);
        if (Array.isArray(arr)) {
          for (const ver of arr) {
            if (typeof ver === 'string' && DB_VER_RE.test(ver)) {
              mongoCounts.set(ver, (mongoCounts.get(ver) || 0) + 1);
            }
          }
        }
      } catch {
        // ignore corrupt legacy row
      }
    }
    const byMongoVersion = [...mongoCounts.entries()]
      .map(([key, installs]) => ({ key, installs }))
      .sort((a, b) => b.installs - a.installs);

    return {
      generatedAt: new Date().toISOString(),
      currentRelease: {
        latestVersion: manifest.latestVersion,
        minSupportedVersion: manifest.minSupportedVersion,
        mandatory: manifest.mandatory,
      },
      requests: {
        totalChecks: Number(reqTotals?.total_checks ?? 0),
        checksWithAnonymousUsage: Number(reqTotals?.checks_with_usage ?? 0),
      },
      activeInstalls: {
        dau,
        wau,
        mau,
        totalKnownInstalls: Number(totals?.total_installs ?? 0),
      },
      engagement: {
        totalLaunches: Number(totals?.total_launches ?? 0),
        avgLastSessionSec: Number(totals?.avg_session_sec ?? 0),
        totalSavedConnections: Number(totals?.total_saved_connections ?? 0),
        totalShellsOpened: Number(totals?.total_shells_opened ?? 0),
        totalQueriesExecuted: Number(totals?.total_queries_executed ?? 0),
      },
      featureAdoption: {
        sshUsers: Number(totals?.ssh_users ?? 0),
        tlsUsers: Number(totals?.tls_users ?? 0),
        replicaSetUsers: Number(totals?.replica_set_users ?? 0),
        aiEnabledUsers: Number(totals?.ai_enabled_users ?? 0),
      },
      distributions: {
        byPlatform: stmtByPlatform.all(),
        byFormFactor: stmtByFormFactor.all(),
        byArch: stmtByArch.all(),
        byAppVersion: stmtByVersion.all(),
        byLanguage: stmtByLanguage.all(),
        byTheme: stmtByTheme.all(),
        byMongoVersion,
      },
      dailyTrendLast30d: stmtDailyTrend.all(thirtyDaysAgo),
    };
  }

  const server = http.createServer(async (req, res) => {
    try {
      if (req.method !== 'GET' && req.method !== 'POST') {
        sendJson(req, res, 405, { error: 'Method not allowed' }, { Allow: 'GET, POST' });
        return;
      }

      if (!allowRequest(req)) {
        sendJson(req, res, 429, { error: 'Too many requests' }, { 'Retry-After': '60' });
        return;
      }

      const urlObj = new URL(req.url || '/', 'http://localhost');
      const pathname = urlObj.pathname;

      if (req.method === 'GET' && pathname === '/v1/health') {
        sendJson(req, res, 200, { status: 'ok', latestVersion: manifest.latestVersion });
        return;
      }

      if (req.method === 'POST' && pathname === '/v1/release-check') {
        const body = await readJsonBody(req);
        const appVersion = typeof body.appVersion === 'string' && SEMVER_RE.test(body.appVersion.trim())
          ? body.appVersion.trim().slice(0, 32)
          : '0.0.0';
        const rawPlatform = typeof body.platform === 'string' ? body.platform.trim().toLowerCase() : '';
        const platform = ALLOWED_PLATFORMS.has(rawPlatform) ? rawPlatform : 'unknown';
        const arch = sanitizeString(body.arch, 24, 'unknown');
        const rawForm = typeof body.formFactor === 'string' ? body.formFactor.trim().toLowerCase() : '';
        const formFactor = ALLOWED_FORM_FACTORS.has(rawForm) ? rawForm : 'desktop';
        const osVersion = sanitizeString(body.osVersion, 32, 'unknown');
        const uiLanguage = sanitizeString(body.uiLanguage, 16, 'en');

        const nowIso = new Date().toISOString();
        const dayUtc = nowIso.slice(0, 10);
        const hasUsage = Boolean(body.usage && typeof body.usage === 'object' && !Array.isArray(body.usage));

        stmtUpsertDailyReq.run(dayUtc, platform, appVersion, hasUsage ? 1 : 0);

        if (hasUsage) {
          const u = body.usage;
          const installId = typeof u.installId === 'string' && INSTALL_ID_RE.test(u.installId)
            ? u.installId
            : '';
          if (installId) {
            const cleanDbs = [];
            if (Array.isArray(u.dbVersions)) {
              for (const item of u.dbVersions.slice(0, 16)) {
                if (typeof item === 'string' && DB_VER_RE.test(item.trim()) && !cleanDbs.includes(item.trim())) {
                  cleanDbs.push(item.trim());
                }
              }
            }
            stmtUpsertInstall.run(
              installId,
              nowIso,
              nowIso,
              appVersion,
              platform,
              arch,
              formFactor,
              osVersion,
              uiLanguage,
              sanitizeString(u.uiTheme, 16, 'System'),
              sanitizeString(u.uiDensity, 16, 'Automatic'),
              clampNonNegInt(u.launchCount),
              clampNonNegInt(u.lastSessionSec, 7 * 86400),
              clampNonNegInt(u.savedConnections, 10000),
              clampNonNegInt(u.sshConnections, 10000),
              clampNonNegInt(u.tlsConnections, 10000),
              clampNonNegInt(u.replicaSetConnections, 10000),
              clampNonNegInt(u.shellsOpened),
              clampNonNegInt(u.queriesExecuted),
              u.aiEnabled === true ? 1 : 0,
              JSON.stringify(cleanDbs)
            );
            stmtUpsertDailyActive.run(dayUtc, installId, platform, appVersion);
          }
        }

        sendJson(req, res, 200, buildReleaseResponse(platform));
        return;
      }

      if (pathname === '/v1/admin/stats' && req.method === 'GET') {
        if (!verifyBearerToken(req.headers.authorization, adminToken)) {
          sendJson(req, res, 401, { error: 'Unauthorized' }, { 'WWW-Authenticate': 'Bearer' });
          return;
        }
        sendJson(req, res, 200, computeAdminStats());
        return;
      }

      if (pathname === '/v1/admin/releases') {
        if (!verifyBearerToken(req.headers.authorization, adminToken)) {
          sendJson(req, res, 401, { error: 'Unauthorized' }, { 'WWW-Authenticate': 'Bearer' });
          return;
        }
        if (req.method === 'GET') {
          sendJson(req, res, 200, manifest);
          return;
        }
        if (req.method === 'POST') {
          const incoming = await readJsonBody(req);
          const nextManifest = validateReleasesManifest(incoming, allowedHosts, allowedPageHosts);
          if (options.persistReleases !== false) {
            const resolvedOut = path.resolve(releasesPath);
            try {
              const st = fs.lstatSync(resolvedOut);
              if (st.isSymbolicLink() || !st.isFile()) {
                throw new Error('Refusing to overwrite non-regular releases file.');
              }
            } catch (err) {
              if (!err || err.code !== 'ENOENT') throw err;
            }
            const tmpOut = `${resolvedOut}.tmp.${process.pid}.${crypto.randomBytes(4).toString('hex')}`;
            fs.writeFileSync(tmpOut, `${JSON.stringify(nextManifest, null, 2)}\n`, { encoding: 'utf8', mode: 0o644, flag: 'wx' });
            fs.renameSync(tmpOut, resolvedOut);
          }
          manifest = nextManifest;
          sendJson(req, res, 200, { status: 'updated', manifest: nextManifest });
          return;
        }
      }

      sendJson(req, res, 404, { error: 'Not found' });
    } catch (err) {
      const code = typeof err?.statusCode === 'number' ? err.statusCode : 400;
      const safeMsg = code === 413
        ? 'Payload too large'
        : code === 415
          ? 'Unsupported media type'
          : (err instanceof Error && code < 500 ? err.message : 'Internal server error');
      sendJson(req, res, code, { error: safeMsg });
    }
  });

  return {
    server,
    close() {
      return new Promise((resolve) => {
        server.close(() => {
          db.close();
          resolve();
        });
      });
    },
  };
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const host = process.env.PULSE_BIND_HOST || '127.0.0.1';
  const port = Number.parseInt(process.env.PULSE_PORT || '8787', 10);
  const app = createReleasePulseServer();
  app.server.listen(port, host, () => {
    console.log(`[release-pulse-api] listening on http://${host}:${port}`);
  });
  for (const sig of ['SIGINT', 'SIGTERM']) {
    process.on(sig, async () => {
      await app.close();
      process.exit(0);
    });
  }
}
