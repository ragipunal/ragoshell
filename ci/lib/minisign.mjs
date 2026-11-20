#!/usr/bin/env node
/**
 * Pure Node.js (zero external dependencies) Minisign (Ed25519 / BLAKE2b-512)
 * public-key parser, signature + trusted-comment verifier, secret-key header
 * inspector, and deterministic test-vector signer.
 *
 * Specification: https://jedisct1.github.io/minisign/
 * See docs/ReleasePipeline.md §3 and §6.
 */

import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

// RFC 8410 SubjectPublicKeyInfo (SPKI) DER prefix for a 32-byte raw Ed25519 public key.
const ED25519_SPKI_PREFIX = Buffer.from('302a300506032b6570032100', 'hex');

const BASE64_STRICT_RE = /^[A-Za-z0-9+/]+={0,2}$/;
const UTC_ISO_RE = /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$/;
const SAFE_FILENAME_RE = /^[A-Za-z0-9][A-Za-z0-9._+-]{0,127}$/;

function decodeStrictBase64(b64, expectedByteLength, label) {
  if (typeof b64 !== 'string') {
    throw new Error(`Invalid ${label}: expected Base64 string.`);
  }
  const trimmed = b64.trim();
  if (trimmed.length === 0 || trimmed.length % 4 !== 0 || !BASE64_STRICT_RE.test(trimmed)) {
    throw new Error(`Invalid ${label}: malformed Base64 encoding.`);
  }
  const buf = Buffer.from(trimmed, 'base64');
  if (buf.length !== expectedByteLength) {
    throw new Error(`Invalid ${label}: expected ${expectedByteLength} bytes, got ${buf.length}.`);
  }
  return buf;
}

/**
 * Parses either a single-line Minisign public key ("RW...") or a 2-line .pub file.
 */
export function parseMinisignPublicKey(input) {
  if (typeof input !== 'string' || input.trim().length === 0) {
    throw new Error('Minisign public key is empty.');
  }
  const lines = input
    .split(/\r?\n/)
    .map((l) => l.trim())
    .filter((l) => l.length > 0);

  let b64Line = '';
  if (lines.length === 1) {
    b64Line = lines[0];
  } else if (lines.length === 2 && lines[0].startsWith('untrusted comment:')) {
    b64Line = lines[1];
  } else {
    throw new Error('Invalid Minisign public key format (expected "RW..." or 2-line .pub file).');
  }

  const decoded = decodeStrictBase64(b64Line, 42, 'Minisign public key');
  const alg = decoded.subarray(0, 2).toString('ascii');
  if (alg !== 'Ed') {
    throw new Error(`Unsupported Minisign public key algorithm "${alg}" (expected "Ed").`);
  }

  const keyId = Buffer.from(decoded.subarray(2, 10));
  const rawPublicKey = Buffer.from(decoded.subarray(10, 42));
  const publicKeyObj = crypto.createPublicKey({
    key: Buffer.concat([ED25519_SPKI_PREFIX, rawPublicKey]),
    format: 'der',
    type: 'spki',
  });

  return {
    base64: b64Line,
    keyId,
    keyIdHex: keyId.toString('hex').toUpperCase(),
    rawPublicKey,
    publicKeyObj,
  };
}

/**
 * Inspects a Minisign secret key (.key) envelope WITHOUT exposing or logging secret bytes.
 * Determines whether the secret key is scrypt password-encrypted ("Sc") or unencrypted ("\0\0").
 */
export function inspectMinisignSecretKey(secretKeyText) {
  if (typeof secretKeyText !== 'string' || secretKeyText.trim().length === 0) {
    throw new Error('Minisign secret key is empty.');
  }
  const lines = secretKeyText
    .split(/\r?\n/)
    .map((l) => l.trim())
    .filter((l) => l.length > 0);

  let b64Line = '';
  if (lines.length === 1) {
    b64Line = lines[0];
  } else if (lines.length === 2 && lines[0].startsWith('untrusted comment:')) {
    b64Line = lines[1];
  } else {
    throw new Error('Invalid Minisign secret key format (expected 2-line .key file).');
  }

  const decoded = decodeStrictBase64(b64Line, 158, 'Minisign secret key');
  try {
    const sigAlg = decoded.subarray(0, 2).toString('ascii');
    const kdfBytes = decoded.subarray(2, 4);
    const cksumAlg = decoded.subarray(4, 6).toString('ascii');

    if (sigAlg !== 'Ed' || cksumAlg !== 'B2') {
      throw new Error('Unsupported Minisign secret key header algorithms.');
    }
    if (kdfBytes[0] === 0x53 && kdfBytes[1] === 0x63) {
      return { encrypted: true, kdf: 'Sc' };
    }
    if (kdfBytes[0] === 0x00 && kdfBytes[1] === 0x00) {
      return { encrypted: false, kdf: 'none' };
    }
    throw new Error('Unrecognized Minisign secret key KDF algorithm.');
  } finally {
    decoded.fill(0);
  }
}

/**
 * Parses strictly tab-separated key:value claims from a verified Minisign trusted comment.
 * Rejects malformed segments and duplicate keys to eliminate parser differentials.
 */
export function parseTrustedCommentTokens(trustedComment) {
  if (typeof trustedComment !== 'string' || trustedComment.length === 0 || trustedComment.length > 1024) {
    throw new Error('Trusted comment must be a non-empty string (<= 1024 chars).');
  }
  // Reject control characters except horizontal tab (\t), which separates claims.
  if (/[\x00-\x08\x0a-\x1f\x7f]/.test(trustedComment)) {
    throw new Error('Trusted comment contains forbidden control characters.');
  }

  const tokens = Object.create(null);
  const parts = trustedComment.split('\t');

  for (const part of parts) {
    if (!part || /\s/.test(part)) {
      throw new Error(`Malformed or whitespace-containing trusted comment segment: "${part}"`);
    }
    const colonIdx = part.indexOf(':');
    if (colonIdx <= 0 || colonIdx === part.length - 1) {
      throw new Error(`Malformed trusted comment segment (expected key:value): "${part}"`);
    }
    const key = part.slice(0, colonIdx);
    const value = part.slice(colonIdx + 1);
    if (!/^[a-z0-9_-]{1,32}$/i.test(key)) {
      throw new Error(`Invalid trusted comment claim key: "${key}"`);
    }
    if (Object.prototype.hasOwnProperty.call(tokens, key)) {
      throw new Error(`Duplicate claim key "${key}" in signed trusted comment.`);
    }
    tokens[key] = value;
  }
  return tokens;
}

/**
 * Cryptographically verifies a Minisign .minisig signature buffer against fileBytes,
 * including the global signature over the trusted comment and mandatory release/timestamp/file claims.
 */
export function verifyMinisigBuffer(fileBytes, sigText, pubKeyInput, expectations = {}) {
  if (!Buffer.isBuffer(fileBytes)) {
    throw new Error('fileBytes must be a Buffer.');
  }
  if (typeof sigText !== 'string' || sigText.length === 0 || sigText.length > 8192) {
    throw new Error('Invalid .minisig content.');
  }

  const pubKey = typeof pubKeyInput === 'string' ? parseMinisignPublicKey(pubKeyInput) : pubKeyInput;
  if (!pubKey || !pubKey.publicKeyObj || !Buffer.isBuffer(pubKey.keyId)) {
    throw new Error('Invalid parsed Minisign public key object.');
  }

  const rawLines = sigText.replace(/\r\n/g, '\n').split('\n');
  while (rawLines.length > 0 && rawLines[rawLines.length - 1] === '') {
    rawLines.pop();
  }
  if (rawLines.length !== 4) {
    throw new Error(`Invalid .minisig structure: expected 4 lines, got ${rawLines.length}.`);
  }

  if (!rawLines[0].startsWith('untrusted comment:')) {
    throw new Error('Invalid .minisig line 1: missing "untrusted comment:" prefix.');
  }

  const sigStruct = decodeStrictBase64(rawLines[1], 74, '.minisig signature line');
  const sigAlg = sigStruct.subarray(0, 2).toString('ascii');
  if (sigAlg !== 'ED') {
    throw new Error(`Unsupported .minisig signature algorithm "${sigAlg}" (only BLAKE2b-512 prehashed "ED" is accepted).`);
  }

  const sigKeyId = sigStruct.subarray(2, 10);
  if (!crypto.timingSafeEqual(sigKeyId, pubKey.keyId)) {
    throw new Error(
      `Minisign key ID mismatch: signature uses ${sigKeyId.toString('hex').toUpperCase()}, expected ${pubKey.keyIdHex}.`
    );
  }

  const rawSig = sigStruct.subarray(10, 74);

  const trustedPrefix = 'trusted comment: ';
  if (!rawLines[2].startsWith(trustedPrefix)) {
    throw new Error('Invalid .minisig line 3: missing "trusted comment: " prefix.');
  }
  const trustedComment = rawLines[2].slice(trustedPrefix.length);
  if (trustedComment.length === 0) {
    throw new Error('Invalid .minisig line 3: trusted comment is empty.');
  }

  const globalSig = decodeStrictBase64(rawLines[3], 64, '.minisig global signature line');

  // 1. Verify Ed25519 signature over BLAKE2b-512(fileBytes).
  const signedPayload = crypto.createHash('blake2b512').update(fileBytes).digest();

  if (!crypto.verify(null, signedPayload, pubKey.publicKeyObj, rawSig)) {
    throw new Error('Minisign Ed25519 file signature verification failed.');
  }

  // 2. Verify Ed25519 global signature over (rawSig || UTF-8 bytes of trustedComment).
  const globalPayload = Buffer.concat([rawSig, Buffer.from(trustedComment, 'utf8')]);
  if (!crypto.verify(null, globalPayload, pubKey.publicKeyObj, globalSig)) {
    throw new Error('Minisign Ed25519 global signature over trusted comment failed.');
  }

  // 3. Validate signed trusted-comment claims (anti-downgrade, anti-replay, anti-cross-file swap).
  const tokens = parseTrustedCommentTokens(trustedComment);

  if (!tokens.release) {
    throw new Error('Signed trusted comment is missing required "release:<version>" claim.');
  }
  if (expectations.expectedVersion && tokens.release !== expectations.expectedVersion) {
    throw new Error(
      `Trusted comment release mismatch: signed for "${tokens.release}", expected "${expectations.expectedVersion}".`
    );
  }

  if (!tokens.timestamp || !UTC_ISO_RE.test(tokens.timestamp)) {
    throw new Error('Signed trusted comment is missing valid UTC ISO-8601 "timestamp:<YYYY-MM-DDTHH:MM:SSZ>" claim.');
  }
  const parsedTsMs = Date.parse(tokens.timestamp);
  if (!Number.isFinite(parsedTsMs)) {
    throw new Error(`Signed trusted comment contains unparseable timestamp "${tokens.timestamp}".`);
  }
  if (expectations.checkClockSkew !== false) {
    const maxFutureSkewMs = (expectations.maxFutureSkewSec ?? 300) * 1000;
    const nowMs = expectations.nowMs ?? Date.now();
    if (parsedTsMs > nowMs + maxFutureSkewMs) {
      throw new Error(`Signed trusted comment timestamp "${tokens.timestamp}" is in the future.`);
    }
  }

  if (!tokens.file || !SAFE_FILENAME_RE.test(tokens.file) || tokens.file.includes('..')) {
    throw new Error('Signed trusted comment is missing a valid "file:<basename>" claim.');
  }
  if (expectations.expectedFile && tokens.file !== expectations.expectedFile) {
    throw new Error(
      `Trusted comment file mismatch: signed for "${tokens.file}", expected "${expectations.expectedFile}".`
    );
  }

  return {
    ok: true,
    algorithm: sigAlg,
    keyIdHex: pubKey.keyIdHex,
    trustedComment,
    tokens,
  };
}

/**
 * Verifies a file and its detached .minisig file on disk (refusing symlinks).
 */
export function verifyMinisigFile(filePath, sigPath, pubKeyInput, expectations = {}) {
  const resolvedFile = path.resolve(filePath);
  const resolvedSig = path.resolve(sigPath);
  const fileStat = fs.lstatSync(resolvedFile);
  const sigStat = fs.lstatSync(resolvedSig);
  if (!fileStat.isFile() || fileStat.isSymbolicLink()) {
    throw new Error(`Refusing to verify non-regular or symlink file: ${resolvedFile}`);
  }
  if (!sigStat.isFile() || sigStat.isSymbolicLink()) {
    throw new Error(`Refusing to read non-regular or symlink signature: ${resolvedSig}`);
  }
  if (sigStat.size <= 0 || sigStat.size > 8192) {
    throw new Error(`Signature file has invalid size (${sigStat.size} bytes): ${resolvedSig}`);
  }

  const fileBytes = fs.readFileSync(resolvedFile);
  const sigText = fs.readFileSync(resolvedSig, 'utf8');
  return verifyMinisigBuffer(fileBytes, sigText, pubKeyInput, {
    ...expectations,
    expectedFile: expectations.expectedFile ?? path.basename(resolvedFile),
  });
}

/**
 * Generates an ephemeral in-memory Minisign-compatible Ed25519 keypair for automated unit tests.
 * Never used for production signing (production uses `minisign` CLI via ci/sign-release.sh).
 */
export function generateEphemeralTestKeyPair(comment = 'ephemeral pipeline test key') {
  const { publicKey, privateKey } = crypto.generateKeyPairSync('ed25519');
  const spki = publicKey.export({ format: 'der', type: 'spki' });
  const rawPublicKey = spki.subarray(spki.length - 32);
  const keyId = crypto.randomBytes(8);
  const pubPayload = Buffer.concat([Buffer.from('Ed', 'ascii'), keyId, rawPublicKey]);
  const base64 = pubPayload.toString('base64');
  const pubFileText = `untrusted comment: ${comment}\n${base64}\n`;
  return {
    base64,
    pubFileText,
    keyId,
    keyIdHex: keyId.toString('hex').toUpperCase(),
    rawPublicKey,
    publicKeyObj: publicKey,
    privateKeyObj: privateKey,
  };
}

/**
 * Signs a buffer in standard Minisign ("ED" BLAKE2b-512 prehashed) format using an ephemeral test keypair.
 */
export function signBufferWithTestKey(fileBytes, testKeyPair, options = {}) {
  const prehashed = options.prehashed !== false;
  const algTag = prehashed ? 'ED' : 'Ed';
  const untrustedComment = options.untrustedComment || 'signature from ephemeral test key';
  const trustedComment = options.trustedComment || `timestamp:${new Date().toISOString().replace(/\.\d{3}Z$/, 'Z')}`;

  if (/[\r\n\0]/.test(trustedComment) || /[\r\n\0]/.test(untrustedComment)) {
    throw new Error('Comments must not contain CR, LF, or NUL.');
  }

  const payload = prehashed
    ? crypto.createHash('blake2b512').update(fileBytes).digest()
    : fileBytes;
  const rawSig = crypto.sign(null, payload, testKeyPair.privateKeyObj);
  const sigStruct = Buffer.concat([Buffer.from(algTag, 'ascii'), testKeyPair.keyId, rawSig]);

  const globalPayload = Buffer.concat([rawSig, Buffer.from(trustedComment, 'utf8')]);
  const globalSig = crypto.sign(null, globalPayload, testKeyPair.privateKeyObj);

  return [
    `untrusted comment: ${untrustedComment}`,
    sigStruct.toString('base64'),
    `trusted comment: ${trustedComment}`,
    globalSig.toString('base64'),
    '',
  ].join('\n');
}

function parseCliArgs(argv) {
  const out = { cmd: argv[0] || '', flags: Object.create(null), positional: [] };
  for (let i = 1; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg.startsWith('--')) {
      const key = arg.slice(2);
      const next = argv[i + 1];
      if (next === undefined || next.startsWith('--')) {
        out.flags[key] = 'true';
      } else {
        out.flags[key] = next;
        i += 1;
      }
    } else {
      out.positional.push(arg);
    }
  }
  return out;
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  try {
    const { cmd, flags, positional } = parseCliArgs(process.argv.slice(2));
    if (cmd === 'inspect-secret-key') {
      const keyFile = flags.file || positional[0];
      if (!keyFile) {
        throw new Error('Usage: node ci/lib/minisign.mjs inspect-secret-key --file <path>');
      }
      const resolved = path.resolve(keyFile);
      const st = fs.lstatSync(resolved);
      if (!st.isFile() || st.isSymbolicLink()) {
        throw new Error('Secret key path must be a regular file (not a symlink).');
      }
      const info = inspectMinisignSecretKey(fs.readFileSync(resolved, 'utf8'));
      process.stdout.write(`${info.encrypted ? 'encrypted' : 'unencrypted'}\n`);
    } else if (cmd === 'extract-pubkey') {
      const pubInput = flags.pubkey
        ? flags.pubkey
        : flags.file
          ? fs.readFileSync(path.resolve(flags.file), 'utf8')
          : '';
      const parsed = parseMinisignPublicKey(pubInput);
      process.stdout.write(`${parsed.base64}\n`);
    } else if (cmd === 'verify') {
      const targetFile = flags.file;
      const sigFile = flags.sig || (targetFile ? `${targetFile}.minisig` : '');
      const pubInput = flags.pubkey
        ? flags.pubkey
        : flags['pubkey-file']
          ? fs.readFileSync(path.resolve(flags['pubkey-file']), 'utf8')
          : '';
      if (!targetFile || !pubInput) {
        throw new Error(
          'Usage: node ci/lib/minisign.mjs verify --file <path> [--sig <path>] --pubkey <RW...> [--version <ver>] [--expected-file <name>]'
        );
      }
      const result = verifyMinisigFile(targetFile, sigFile, pubInput, {
        expectedVersion: flags.version || undefined,
        expectedFile: flags['expected-file'] || path.basename(path.resolve(targetFile)),
      });
      process.stdout.write(
        `[minisign-verify] OK: ${path.basename(targetFile)} (alg=${result.algorithm}, keyId=${result.keyIdHex}, release=${result.tokens.release}, timestamp=${result.tokens.timestamp})\n`
      );
    } else {
      throw new Error(`Unknown command "${cmd}". Expected: inspect-secret-key | extract-pubkey | verify`);
    }
  } catch (err) {
    process.stderr.write(`error: ${err instanceof Error ? err.message : String(err)}\n`);
    process.exit(1);
  }
}
