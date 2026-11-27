#!/usr/bin/env node
/**
 * Deterministic WebAssembly Live Playground (`Mod A`) stage verifier and
 * POSIX USTAR `.tar.gz` packager for `ragoshell-infra/scripts/deploy-receive.sh demo`
 * and `ragoshell-infra/nginx/templates/50-demo.conf.template` (Roadmap W6).
 *
 * Enforces fail-closed checks on both staging directories and `.tar.gz` archives:
 * 1. Brand single-source: reads product identity strictly from `cmake/RagoShellDefaults.cmake`.
 * 2. CSP & XSS safety (`50-demo.conf.template`: `script-src 'self' 'wasm-unsafe-eval'`):
 *    - Rejects inline `<script>` bodies, `<script>` without `src`, external script URLs,
 *      inline `on*=` event attributes, `javascript:` URLs, and `<meta http-equiv="Content-Security-Policy">`.
 *    - Rejects `.innerHTML`, `.outerHTML`, `document.write`, `eval(`, and `new Function(` in `wasm-boot.js`
 *      and rejects `new Function(` in `<product>.js` (`-s DYNAMIC_EXECUTION=0`).
 * 3. Binary & multithreading verification:
 *    - Verifies `\0asm` v1 magic on `<product>.wasm`, PNG magic on `app_icon.png`, and Brotli
 *      decompression round-trip header check on `<product>.wasm.br` (generating `.wasm.br` via
 *      Node `zlib` quality 9 when absent and allowed).
 *    - When `--require-threads` is set (`wasm-multi`), parses the WebAssembly binary sections to
 *      prove shared linear memory (`flags & 0x02`) was linked (`-pthread` / `--shared-memory`).
 * 4. License bundle verification: delegates to `verifyInstalledLicensesDir` (`ci/generate-sbom.mjs`).
 * 5. Exact `deploy-receive.sh demo` archive contract:
 *    - Regular files (`0`) and directories (`5`) only — zero symlinks, hardlinks, FIFOs, or PAX records.
 *    - Relative POSIX paths only — no leading `/`, no `..`, no dotfiles (`50-demo.conf.template` denies `/\.`).
 *    - Compressed size `<= 64 MiB` (`67,108,864` B) and unpacked tar size `<= 160 MiB` (`167,772,160` B).
 */

import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { pipeline } from 'node:stream/promises';
import { Readable } from 'node:stream';
import { fileURLToPath, pathToFileURL } from 'node:url';
import zlib from 'node:zlib';

import { compressFileBrotli } from './compress-brotli.mjs';
import { computeFileSha256, readBrandDefaults } from './generate-releases-json.mjs';
import { verifyInstalledLicensesDir } from './generate-sbom.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const DEFAULT_REPO_ROOT = path.resolve(__dirname, '..');

export const MAX_DEMO_TGZ_BYTES = 64 * 1024 * 1024;
export const MAX_DEMO_TREE_BYTES = 160 * 1024 * 1024;

// Fixed past UTC epoch (2023-11-14T22:13:20Z) for reproducible tar headers without future-clock warnings.
const DETERMINISTIC_MTIME_SEC = 1700000000;

const SAFE_SEGMENT_RE = /^[A-Za-z0-9][A-Za-z0-9._+-]{0,127}$/;
const WASM_MAGIC = Buffer.from([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00]);
const PNG_MAGIC = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

function readFileHeaderBytes(filePath, count) {
  const fd = fs.openSync(filePath, 'r');
  try {
    const buf = Buffer.alloc(count);
    const read = fs.readSync(fd, buf, 0, count, 0);
    return buf.subarray(0, read);
  } finally {
    fs.closeSync(fd);
  }
}

function readUleb128(buf, offsetObj) {
  let result = 0;
  let shift = 0;
  for (let i = 0; i < 5; i += 1) {
    if (offsetObj.pos >= buf.length) {
      throw new Error('Unexpected EOF while reading WebAssembly LEB128 integer.');
    }
    const byte = buf[offsetObj.pos];
    offsetObj.pos += 1;
    result |= (byte & 0x7f) << shift;
    if ((byte & 0x80) === 0) {
      return result >>> 0;
    }
    shift += 7;
  }
  throw new Error('Invalid overlong WebAssembly LEB128 integer.');
}

function readSleb128Skip(buf, offsetObj) {
  for (let i = 0; i < 5; i += 1) {
    if (offsetObj.pos >= buf.length) {
      throw new Error('Unexpected EOF while skipping WebAssembly SLEB128 integer.');
    }
    const byte = buf[offsetObj.pos];
    offsetObj.pos += 1;
    if ((byte & 0x80) === 0) {
      return;
    }
  }
  throw new Error('Invalid overlong WebAssembly SLEB128 integer.');
}

function readWasmString(buf, offsetObj) {
  const len = readUleb128(buf, offsetObj);
  if (offsetObj.pos + len > buf.length) {
    throw new Error('Unexpected EOF while reading WebAssembly UTF-8 name.');
  }
  const str = buf.subarray(offsetObj.pos, offsetObj.pos + len).toString('utf8');
  offsetObj.pos += len;
  return str;
}

/**
 * Inspects WebAssembly binary sections (streaming only the section headers up to Import/Memory,
 * which always precede Code section 10 in valid `.wasm` modules) and returns whether linear memory
 * has the shared flag (`0x02`) enabled (`-pthread` / `--shared-memory`).
 */
export function inspectWasmBinaryCapabilities(wasmPath) {
  // Sections 1..5 (Type, Import, Function, Table, Memory) appear in the first few hundred KB
  // before Section 10 (Code). Reading up to 4 MiB is fast and covers huge import tables safely.
  const headerSlice = readFileHeaderBytes(wasmPath, 4 * 1024 * 1024);
  if (headerSlice.length < 8 || !headerSlice.subarray(0, 8).equals(WASM_MAGIC)) {
    throw new Error(`File does not start with valid WebAssembly v1 magic (\\0asm): ${wasmPath}`);
  }

  const cursor = { pos: 8 };
  let hasMemory = false;
  let hasSharedMemory = false;

  while (cursor.pos < headerSlice.length) {
    const sectionId = headerSlice[cursor.pos];
    cursor.pos += 1;
    if (cursor.pos >= headerSlice.length) {
      break;
    }
    const sectionLen = readUleb128(headerSlice, cursor);
    const sectionEnd = cursor.pos + sectionLen;
    if (sectionEnd > headerSlice.length) {
      if (sectionId <= 5) {
        throw new Error(
          `WebAssembly section ${sectionId} exceeds 4 MiB header inspection window in ${wasmPath}`
        );
      }
      // Reached a large section (e.g. Code section 10 or Data section 11) past Import (2) and Memory (5).
      break;
    }

    if (sectionId === 2) {
      // Import section
      const importCount = readUleb128(headerSlice, cursor);
      for (let i = 0; i < importCount; i += 1) {
        readWasmString(headerSlice, cursor); // module
        readWasmString(headerSlice, cursor); // field
        if (cursor.pos >= sectionEnd) {
          throw new Error(`Malformed WebAssembly Import section in ${wasmPath}`);
        }
        const kind = headerSlice[cursor.pos];
        cursor.pos += 1;
        if (kind === 0x00) {
          // func: typeidx
          readUleb128(headerSlice, cursor);
        } else if (kind === 0x01) {
          // table: reftype + limits
          if (cursor.pos >= sectionEnd) {
            throw new Error(`Truncated table import in ${wasmPath}`);
          }
          const refType = headerSlice[cursor.pos];
          cursor.pos += 1;
          if (refType === 0x63 || refType === 0x64) {
            readSleb128Skip(headerSlice, cursor);
          }
          const flags = readUleb128(headerSlice, cursor);
          readUleb128(headerSlice, cursor);
          if ((flags & 0x01) !== 0) {
            readUleb128(headerSlice, cursor);
          }
        } else if (kind === 0x02) {
          // memory: limits (flags bit 0x01 = has_max, bit 0x02 = shared)
          const flags = readUleb128(headerSlice, cursor);
          readUleb128(headerSlice, cursor);
          if ((flags & 0x01) !== 0) {
            readUleb128(headerSlice, cursor);
          }
          hasMemory = true;
          if ((flags & 0x02) !== 0) {
            hasSharedMemory = true;
          }
        } else if (kind === 0x03) {
          // global: valtype + mutability
          if (cursor.pos === sectionEnd) {
            throw new Error(`Truncated global import in ${wasmPath}`);
          }
          const valType = headerSlice[cursor.pos];
          if (valType === 0x63 || valType === 0x64) {
            // GC ref null / ref heaptype (SLEB128)
            cursor.pos += 1;
            readSleb128Skip(headerSlice, cursor);
          } else {
            cursor.pos += 1;
          }
          cursor.pos += 1; // mut
        } else if (kind === 0x04) {
          // exception tag: attribute (u8) + typeidx
          cursor.pos += 1;
          readUleb128(headerSlice, cursor);
        } else {
          throw new Error(`Unsupported WebAssembly import descriptor kind 0x${kind.toString(16)} in ${wasmPath}`);
        }
      }
      if (cursor.pos !== sectionEnd) {
        throw new Error(`WebAssembly Import section length mismatch in ${wasmPath}`);
      }
    } else if (sectionId === 5) {
      // Memory section (internal non-imported linear memories)
      const memCount = readUleb128(headerSlice, cursor);
      for (let i = 0; i < memCount; i += 1) {
        const flags = readUleb128(headerSlice, cursor);
        readUleb128(headerSlice, cursor);
        if ((flags & 0x01) !== 0) {
          readUleb128(headerSlice, cursor);
        }
        hasMemory = true;
        if ((flags & 0x02) !== 0) {
          hasSharedMemory = true;
        }
      }
      if (cursor.pos !== sectionEnd) {
        throw new Error(`WebAssembly Memory section length mismatch in ${wasmPath}`);
      }
    }

    cursor.pos = sectionEnd;
    if (sectionId >= 6) {
      // Import (2) and Memory (5) sections always have IDs <= 5 in canonical WASM section order.
      break;
    }
  }

  return { hasMemory, hasSharedMemory };
}

/**
 * Streams the entire `brotliPath` through `zlib.createBrotliDecompress()`, enforcing
 * `MAX_DEMO_TREE_BYTES` against Brotli bombs and verifying that the decompressed byte count
 * and SHA-256 digest match `wasmPath` byte-for-byte.
 */
async function verifyWasmBrotliMatchesWasm(wasmPath, brotliPath) {
  const wasmStat = fs.lstatSync(wasmPath);
  const brStat = fs.lstatSync(brotliPath);
  if (brStat.size <= 0) {
    throw new Error(`Brotli file is empty: ${brotliPath}`);
  }
  const expectedSha256 = computeFileSha256(wasmPath);

  const rs = fs.createReadStream(brotliPath, { highWaterMark: 256 * 1024 });
  const decomp = zlib.createBrotliDecompress();
  rs.on('error', (err) => decomp.destroy(err));
  rs.pipe(decomp);

  const hash = crypto.createHash('sha256');
  let decompressedBytes = 0;
  try {
    for await (const chunk of decomp) {
      decompressedBytes += chunk.length;
      if (decompressedBytes > MAX_DEMO_TREE_BYTES) {
        throw new Error(
          `Decompressed ${path.basename(brotliPath)} exceeds MAX_DEMO_TREE (${MAX_DEMO_TREE_BYTES} bytes).`
        );
      }
      hash.update(chunk);
    }
  } finally {
    rs.destroy();
    decomp.destroy();
    if (rs.fd !== null && !rs.closed) {
      await new Promise((resolve) => rs.once('close', resolve));
    }
  }

  const actualSha256 = hash.digest('hex');
  if (decompressedBytes !== wasmStat.size || actualSha256 !== expectedSha256) {
    throw new Error(
      `Decompressed ${path.basename(brotliPath)} (${decompressedBytes} B, sha256=${actualSha256}) does not match ${path.basename(wasmPath)} (${wasmStat.size} B, sha256=${expectedSha256}).`
    );
  }
}

/**
 * Validates `index.html` and `wasm-boot.js` (plus `<product>.js` and `qtloader.js`) against the strict CSP
 * served by `ragoshell-infra/nginx/templates/50-demo.conf.template`:
 * `default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; connect-src 'self'; form-action 'none'`.
 */
export function verifyWasmLoaderCsp(stageDir, brand) {
  const indexHtmlPath = path.join(stageDir, 'index.html');
  const bootJsPath = path.join(stageDir, 'wasm-boot.js');
  const appJsPath = path.join(stageDir, `${brand.productId}.js`);
  const qtLoaderPath = path.join(stageDir, 'qtloader.js');

  const html = fs.readFileSync(indexHtmlPath, 'utf8');
  if (html.includes(`@${'RAGO_'}`) || html.includes('@APPNAME@') || html.includes('@APPEXPORTNAME@')) {
    throw new Error('index.html contains unexpanded CMake @...@ template placeholders.');
  }

  // Strip HTML comments before scanning tags so comments cannot hide or false-positive attributes.
  const strippedHtml = html.replace(/<!--[\s\S]*?-->/g, '');

  if (/http-equiv\s*=\s*["']?Content-Security-Policy/i.test(strippedHtml)) {
    throw new Error('index.html must not embed <meta http-equiv="Content-Security-Policy"> (headers belong in nginx).');
  }
  if (/javascript\s*:/i.test(strippedHtml)) {
    throw new Error('index.html contains a forbidden javascript: URL.');
  }

  // Inspect every HTML opening tag (including slash-separated `<body/onload=...>`) for inline event
  // handlers (`on*=`) and inline `style=` attributes.
  const tagRe = /<([A-Za-z][A-Za-z0-9:-]*)((?:[\s/][^>]*)?)>/g;
  let tagMatch;
  while ((tagMatch = tagRe.exec(strippedHtml)) !== null) {
    const attrs = tagMatch[2] || '';
    if (/(?:^|[\s/"'])on[a-z0-9_-]+\s*=/i.test(attrs)) {
      throw new Error(`index.html contains a forbidden inline event handler attribute in <${tagMatch[1]}>.`);
    }
    if (/(?:^|[\s/"'])style\s*=/i.test(attrs)) {
      throw new Error(`index.html contains a forbidden inline style= attribute in <${tagMatch[1]}>.`);
    }
  }

  // Inspect every <script ...>...</script> block: must have same-origin relative src= and empty body,
  // and total <script tag count must match closed blocks (rejecting unclosed <script> tags).
  const openScriptCount = (strippedHtml.match(/<script(?=[\s/>])/gi) || []).length;
  const scriptBlockRe = /<script(?=[\s/>])([^>]*)>([\s\S]*?)<\/script\s*>/gi;
  const referencedScripts = [];
  let scriptMatch;
  while ((scriptMatch = scriptBlockRe.exec(strippedHtml)) !== null) {
    const attrs = scriptMatch[1] || '';
    const body = scriptMatch[2] || '';
    if (body.trim().length > 0) {
      throw new Error(
        'index.html contains a forbidden inline <script> body (blocked by 50-demo.conf.template script-src).'
      );
    }
    const srcMatch = /(?:^|[\s/])src\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+))/i.exec(attrs);
    const srcVal = srcMatch ? (srcMatch[1] ?? srcMatch[2] ?? srcMatch[3] ?? '').trim() : '';
    if (!srcVal) {
      throw new Error('index.html contains a <script> tag without a src attribute.');
    }
    if (/^(?:[a-z][a-z0-9+.-]*:|\/\/)/i.test(srcVal) || srcVal.startsWith('/') || srcVal.includes('..')) {
      throw new Error(`index.html <script src="${srcVal}"> must be a relative same-origin path.`);
    }
    const scriptDiskPath = path.resolve(stageDir, srcVal);
    if (!scriptDiskPath.startsWith(`${path.resolve(stageDir)}${path.sep}`) || !fs.existsSync(scriptDiskPath)) {
      throw new Error(`index.html references missing script "${srcVal}".`);
    }
    referencedScripts.push(srcVal);
  }
  if (openScriptCount !== referencedScripts.length) {
    throw new Error('index.html contains an unclosed or malformed <script> tag.');
  }

  for (const requiredScript of [`${brand.productId}.js`, 'qtloader.js', 'wasm-boot.js']) {
    if (!referencedScripts.includes(requiredScript)) {
      throw new Error(`index.html must load "${requiredScript}" via <script src="${requiredScript}">.`);
    }
  }

  const bootJs = fs.readFileSync(bootJsPath, 'utf8');
  if (bootJs.includes(`@${'RAGO_'}`)) {
    throw new Error('wasm-boot.js contains unexpanded CMake @RAGO_...@ placeholders.');
  }
  const forbiddenBootPatterns = [
    { re: /\.\s*innerHTML\b/, label: '.innerHTML' },
    { re: /\.\s*outerHTML\b/, label: '.outerHTML' },
    { re: /\bdocument\.write\s*\(/, label: 'document.write()' },
    { re: /\beval\s*\(/, label: 'eval()' },
    { re: /\b(?:new\s+)?Function\s*\(/, label: 'Function()' },
  ];
  for (const { re, label } of forbiddenBootPatterns) {
    if (re.test(bootJs)) {
      throw new Error(`wasm-boot.js contains forbidden dynamic DOM/code sink: ${label}`);
    }
  }

  for (const [jsLabel, jsPath] of [
    [`${brand.productId}.js`, appJsPath],
    ['qtloader.js', qtLoaderPath],
  ]) {
    const jsText = fs.readFileSync(jsPath, 'utf8');
    if (/\b(?:new\s+)?Function\s*\(/.test(jsText) || /\beval\s*\(/.test(jsText)) {
      throw new Error(
        `${jsLabel} contains dynamic JS code evaluation (Function/eval) — link with "-s DYNAMIC_EXECUTION=0" for CSP 'wasm-unsafe-eval' compatibility.`
      );
    }
  }
}

/**
 * Recursively collects and validates all directories and regular files under `stageDir`.
 */
export async function validateAndPrepareWasmStage(stageDir, options = {}) {
  const repoRoot = path.resolve(options.repoRoot || DEFAULT_REPO_ROOT);
  const brand = readBrandDefaults(repoRoot);
  const resolvedStage = path.resolve(stageDir);

  if (!fs.existsSync(resolvedStage)) {
    throw new Error(`WASM stage directory does not exist: ${resolvedStage}`);
  }
  const rootStat = fs.lstatSync(resolvedStage);
  if (rootStat.isSymbolicLink() || !rootStat.isDirectory()) {
    throw new Error(`WASM stage path must be a regular directory (not a symlink): ${resolvedStage}`);
  }

  const wasmPath = path.join(resolvedStage, `${brand.productId}.wasm`);
  const wasmBrPath = path.join(resolvedStage, `${brand.productId}.wasm.br`);
  if (fs.existsSync(wasmPath) && !fs.existsSync(wasmBrPath) && options.autoBrotli !== false) {
    await compressFileBrotli(wasmPath, wasmBrPath, 9);
  }

  const requiredTopFiles = [
    'index.html',
    'wasm-boot.js',
    'qtloader.js',
    `${brand.productId}.js`,
    `${brand.productId}.wasm`,
    `${brand.productId}.wasm.br`,
    'app_icon.png',
  ];
  const optionalTopFiles = new Set([
    `${brand.productId}.worker.js`,
    'qtlogo.svg',
    'qt_plugins.json',
    'qt_qml_imports.json',
  ]);
  const allowedTopDirs = new Set(['licenses', 'qt']);
  const allowedTopFiles = new Set([...requiredTopFiles, ...optionalTopFiles]);

  for (const reqFile of requiredTopFiles) {
    const p = path.join(resolvedStage, reqFile);
    if (!fs.existsSync(p)) {
      throw new Error(`Missing required top-level WASM asset in stage directory: ${reqFile}`);
    }
  }

  // Refuse stray Qt default `<product>.html` so only `index.html` is served.
  const strayQtHtml = path.join(resolvedStage, `${brand.productId}.html`);
  if (fs.existsSync(strayQtHtml)) {
    throw new Error(
      `Refusing stray "${brand.productId}.html" in WASM stage directory (only CSP-clean index.html may be packaged).`
    );
  }

  const entries = [];
  let totalBytes = 0;

  function walk(currentAbs, relPrefix, depth) {
    if (depth > 8) {
      throw new Error(`Directory nesting too deep (>8) in WASM stage: ${relPrefix}`);
    }
    const names = fs.readdirSync(currentAbs).sort();
    for (const name of names) {
      if (name.startsWith('.')) {
        throw new Error(
          `Dotfile or dot-directory refused in WASM stage ("${relPrefix ? `${relPrefix}/` : ''}${name}") — 50-demo.conf.template denies /\\.`
        );
      }
      if (!SAFE_SEGMENT_RE.test(name) || name.includes('..')) {
        throw new Error(`Unsafe filename "${name}" in WASM stage directory.`);
      }

      const relPosix = relPrefix ? `${relPrefix}/${name}` : name;
      const fullPath = path.resolve(currentAbs, name);
      if (!fullPath.startsWith(`${resolvedStage}${path.sep}`)) {
        throw new Error(`Path traversal refused in WASM stage: ${relPosix}`);
      }

      const st = fs.lstatSync(fullPath);
      if (st.isSymbolicLink()) {
        throw new Error(`Symbolic link refused in WASM stage: ${relPosix}`);
      }

      if (depth === 0) {
        if (st.isDirectory() && !allowedTopDirs.has(name)) {
          throw new Error(`Unexpected top-level directory "${name}" in WASM stage.`);
        }
        if (st.isFile() && !allowedTopFiles.has(name)) {
          throw new Error(`Unexpected top-level file "${name}" in WASM stage.`);
        }
      }

      if (st.isDirectory()) {
        entries.push({ relPosix: `${relPosix}/`, isDirectory: true, fullPath, size: 0 });
        walk(fullPath, relPosix, depth + 1);
      } else if (st.isFile()) {
        if (st.size <= 0) {
          throw new Error(`Empty (0-byte) file refused in WASM stage: ${relPosix}`);
        }
        totalBytes += st.size;
        entries.push({ relPosix, isDirectory: false, fullPath, size: st.size });
      } else {
        throw new Error(`Special/non-regular file refused in WASM stage: ${relPosix}`);
      }
    }
  }

  walk(resolvedStage, '', 0);

  // Verify open-source license notices under stage/licenses/
  const licensesDir = path.join(resolvedStage, 'licenses');
  if (!fs.existsSync(licensesDir)) {
    throw new Error('Missing required licenses/ directory in WASM stage.');
  }
  verifyInstalledLicensesDir(licensesDir, {
    requireVcpkgCopyrights: Boolean(options.requireVcpkgCopyrights),
  });

  // Verify CSP and XSS invariants in index.html, wasm-boot.js, and <product>.js
  verifyWasmLoaderCsp(resolvedStage, brand);

  // Verify PNG magic on app_icon.png
  const iconHeader = readFileHeaderBytes(path.join(resolvedStage, 'app_icon.png'), 8);
  if (iconHeader.length < 8 || !iconHeader.equals(PNG_MAGIC)) {
    throw new Error('app_icon.png does not have a valid PNG signature.');
  }

  // Verify WASM binary magic and optional pthread shared-memory requirement
  const wasmCaps = inspectWasmBinaryCapabilities(wasmPath);
  if (options.requireThreads && !wasmCaps.hasSharedMemory) {
    throw new Error(
      `${brand.productId}.wasm does not declare shared linear memory — wasm-multi builds must compile and link with -pthread.`
    );
  }

  // Verify .wasm.br decompresses completely and matches <product>.wasm SHA-256 & byte length
  await verifyWasmBrotliMatchesWasm(wasmPath, wasmBrPath);

  if (totalBytes > MAX_DEMO_TREE_BYTES) {
    throw new Error(
      `Unpacked WASM stage (${totalBytes} bytes) exceeds deploy-receive.sh MAX_DEMO_TREE (${MAX_DEMO_TREE_BYTES} bytes).`
    );
  }

  return { brand, stageDir: resolvedStage, entries, totalContentBytes: totalBytes, wasmCaps };
}

function writeOctalField(buf, offset, length, value) {
  const digits = length - 1;
  const oct = value.toString(8);
  if (oct.length > digits) {
    throw new Error(`Value ${value} overflows ${digits}-digit USTAR octal field.`);
  }
  buf.write(oct.padStart(digits, '0'), offset, digits, 'ascii');
  buf[offset + digits] = 0;
}

function splitUstarPath(relPosix) {
  const nameBytes = Buffer.byteLength(relPosix, 'utf8');
  if (nameBytes <= 100) {
    return { prefix: '', name: relPosix };
  }
  let searchFrom = relPosix.length - 2;
  while (searchFrom > 0) {
    const slashIdx = relPosix.lastIndexOf('/', searchFrom);
    if (slashIdx <= 0) {
      break;
    }
    const prefix = relPosix.slice(0, slashIdx);
    const name = relPosix.slice(slashIdx + 1);
    if (Buffer.byteLength(prefix, 'utf8') <= 155 && Buffer.byteLength(name, 'utf8') <= 100) {
      return { prefix, name };
    }
    searchFrom = slashIdx - 1;
  }
  throw new Error(`Archive path "${relPosix}" exceeds POSIX USTAR 100+155 byte path limit.`);
}

export function createUstarHeader(relPosix, size, isDirectory, mtimeSec = DETERMINISTIC_MTIME_SEC) {
  const header = Buffer.alloc(512, 0);
  const { prefix, name } = splitUstarPath(relPosix);

  header.write(name, 0, Math.min(100, Buffer.byteLength(name, 'utf8')), 'utf8');
  writeOctalField(header, 100, 8, isDirectory ? 0o755 : 0o644);
  writeOctalField(header, 108, 8, 0); // uid
  writeOctalField(header, 116, 8, 0); // gid
  writeOctalField(header, 124, 12, isDirectory ? 0 : size);
  writeOctalField(header, 136, 12, mtimeSec);

  // Fill checksum field (148..155) with spaces before computing sum
  header.fill(0x20, 148, 156);
  header[156] = isDirectory ? 0x35 : 0x30; // '5' = dir, '0' = regular file
  header.write('ustar', 257, 5, 'ascii');
  header[262] = 0;
  header.write('00', 263, 2, 'ascii');
  header.write('root', 265, 4, 'ascii');
  header.write('root', 297, 4, 'ascii');
  writeOctalField(header, 329, 8, 0);
  writeOctalField(header, 337, 8, 0);
  if (prefix) {
    header.write(prefix, 345, Math.min(155, Buffer.byteLength(prefix, 'utf8')), 'utf8');
  }

  let sum = 0;
  for (let i = 0; i < 512; i += 1) {
    sum += header[i];
  }
  const chkOct = sum.toString(8).padStart(6, '0');
  header.write(chkOct, 148, 6, 'ascii');
  header[154] = 0;
  header[155] = 0x20;
  return header;
}

async function* generateTarBlocks(entries, mtimeSec) {
  let unpackedStreamBytes = 0;
  for (const entry of entries) {
    const hdr = createUstarHeader(entry.relPosix, entry.size, entry.isDirectory, mtimeSec);
    unpackedStreamBytes += hdr.length;
    if (unpackedStreamBytes > MAX_DEMO_TREE_BYTES) {
      throw new Error(
        `Unpacked tar stream exceeds deploy-receive.sh MAX_DEMO_TREE (${MAX_DEMO_TREE_BYTES} bytes).`
      );
    }
    yield hdr;

    if (!entry.isDirectory) {
      let writtenForFile = 0;
      const rs = fs.createReadStream(entry.fullPath, { highWaterMark: 256 * 1024 });
      try {
        for await (const chunk of rs) {
          writtenForFile += chunk.length;
          unpackedStreamBytes += chunk.length;
          if (unpackedStreamBytes > MAX_DEMO_TREE_BYTES) {
            throw new Error(
              `Unpacked tar stream exceeds deploy-receive.sh MAX_DEMO_TREE (${MAX_DEMO_TREE_BYTES} bytes).`
            );
          }
          yield chunk;
        }
      } finally {
        rs.destroy();
        if (rs.fd !== null && !rs.closed) {
          await new Promise((resolve) => rs.once('close', resolve));
        }
      }
      if (writtenForFile !== entry.size) {
        throw new Error(
          `File size changed while archiving ${entry.relPosix} (expected ${entry.size}, read ${writtenForFile}).`
        );
      }
      const rem = writtenForFile % 512;
      if (rem !== 0) {
        const pad = Buffer.alloc(512 - rem, 0);
        unpackedStreamBytes += pad.length;
        yield pad;
      }
    }
  }

  const trailer = Buffer.alloc(1024, 0);
  unpackedStreamBytes += trailer.length;
  if (unpackedStreamBytes > MAX_DEMO_TREE_BYTES) {
    throw new Error(
      `Unpacked tar stream (${unpackedStreamBytes} bytes) exceeds deploy-receive.sh MAX_DEMO_TREE (${MAX_DEMO_TREE_BYTES} bytes).`
    );
  }
  yield trailer;
}

function readNullTerminatedString(buf, start, length) {
  let end = start;
  const max = start + length;
  while (end < max && buf[end] !== 0) {
    end += 1;
  }
  return buf.subarray(start, end).toString('utf8');
}

function parseOctalField(buf, start, length) {
  const raw = readNullTerminatedString(buf, start, length).trim();
  if (!raw) {
    return 0;
  }
  if (!/^[0-7]+$/.test(raw)) {
    throw new Error(`Malformed octal field "${raw}" in tar header.`);
  }
  return Number.parseInt(raw, 8);
}

/**
 * Verifies a `.tar.gz` archive against every rule in `ragoshell-infra/scripts/deploy-receive.sh`
 * (`receive_demo`) plus `50-demo.conf.template` asset/dotfile rules.
 */
export async function verifyWasmDemoArchive(archivePath, options = {}) {
  const repoRoot = path.resolve(options.repoRoot || DEFAULT_REPO_ROOT);
  const brand = readBrandDefaults(repoRoot);
  const resolvedArchive = path.resolve(archivePath);

  if (!fs.existsSync(resolvedArchive)) {
    throw new Error(`WASM demo archive does not exist: ${resolvedArchive}`);
  }
  const st = fs.lstatSync(resolvedArchive);
  if (st.isSymbolicLink() || !st.isFile()) {
    throw new Error(`WASM demo archive must be a regular file (not a symlink): ${resolvedArchive}`);
  }
  if (st.size <= 0) {
    throw new Error(`WASM demo archive is empty: ${resolvedArchive}`);
  }
  if (st.size > MAX_DEMO_TGZ_BYTES) {
    throw new Error(
      `WASM demo archive (${st.size} bytes) exceeds deploy-receive.sh MAX_DEMO_TGZ (${MAX_DEMO_TGZ_BYTES} bytes).`
    );
  }

  const gunzip = zlib.createGunzip();
  const source = fs.createReadStream(resolvedArchive);
  source.on('error', (err) => gunzip.destroy(err));
  source.pipe(gunzip);

  let unpackedBytes = 0;
  let pending = Buffer.alloc(0);
  let skipDataBytes = 0;
  let sawEofBlocks = false;
  const entries = new Map();

  try {
    for await (const chunk of gunzip) {
      unpackedBytes += chunk.length;
      if (unpackedBytes > MAX_DEMO_TREE_BYTES) {
        throw new Error(
          `Unpacked WASM demo archive exceeds deploy-receive.sh MAX_DEMO_TREE (${MAX_DEMO_TREE_BYTES} bytes).`
        );
      }
      if (sawEofBlocks) {
        // Trailing zero blocks at the end of a tar stream are normal.
        for (let i = 0; i < chunk.length; i += 1) {
          if (chunk[i] !== 0) {
            throw new Error('Unexpected non-zero data after tar end-of-archive blocks.');
          }
        }
        continue;
      }

      pending = pending.length === 0 ? chunk : Buffer.concat([pending, chunk]);

      while (pending.length > 0) {
        if (skipDataBytes > 0) {
          const consume = Math.min(skipDataBytes, pending.length);
          pending = pending.subarray(consume);
          skipDataBytes -= consume;
          if (skipDataBytes > 0) {
            break;
          }
        }
        if (pending.length < 512) {
          break;
        }

        const block = pending.subarray(0, 512);
        pending = pending.subarray(512);

        let allZero = true;
        for (let i = 0; i < 512; i += 1) {
          if (block[i] !== 0) {
            allZero = false;
            break;
          }
        }
        if (allZero) {
          sawEofBlocks = true;
          for (let i = 0; i < pending.length; i += 1) {
            if (pending[i] !== 0) {
              throw new Error('Unexpected non-zero tar block after zero EOF block.');
            }
          }
          pending = Buffer.alloc(0);
          break;
        }

        // Require standard POSIX USTAR magic ("ustar\0") so prefix bytes (345..499) are unambiguous
        const ustarMagic = block.subarray(257, 263);
        if (!ustarMagic.equals(Buffer.from('ustar\0', 'ascii'))) {
          throw new Error('Tar header is missing standard POSIX USTAR magic ("ustar\\0").');
        }

        // Verify USTAR header checksum
        const expectedChk = parseOctalField(block, 148, 8);
        let actualChk = 0;
        for (let i = 0; i < 512; i += 1) {
          actualChk += i >= 148 && i < 156 ? 0x20 : block[i];
        }
        if (actualChk !== expectedChk) {
          throw new Error(`Tar header checksum mismatch (expected ${expectedChk}, got ${actualChk}).`);
        }

        const typeByte = block[156];
        const typeChar = typeByte === 0 ? '0' : String.fromCharCode(typeByte);
        if (typeChar !== '0' && typeChar !== '5') {
          throw new Error(
            `Tar archive contains forbidden entry type '${typeChar}' (deploy-receive.sh allows only regular files and directories).`
          );
        }

        const namePart = readNullTerminatedString(block, 0, 100);
        const prefixPart = readNullTerminatedString(block, 345, 155);
        const rawName = prefixPart ? `${prefixPart}/${namePart}` : namePart;
        if (!rawName) {
          throw new Error('Tar archive contains an entry with an empty name.');
        }
        if (rawName.startsWith('/') || rawName.includes('\\') || /(?:^|\/)\.\.(?:\/|$)/.test(rawName)) {
          throw new Error(`Tar archive contains forbidden absolute or traversal path: "${rawName}"`);
        }

        const cleanRel = rawName.replace(/^\.(?:\/|$)/, '').replace(/\/$/, '');
        if (!cleanRel) {
          if (typeChar !== '5') {
            throw new Error(`Tar archive contains invalid root file entry "${rawName}".`);
          }
        } else {
          for (const seg of cleanRel.split('/')) {
            if (!seg || seg.startsWith('.') || !SAFE_SEGMENT_RE.test(seg)) {
              throw new Error(`Tar archive contains dotfile or unsafe path segment in "${rawName}".`);
            }
          }
        }

        const size = parseOctalField(block, 124, 12);
        if (typeChar === '5') {
          if (size !== 0) {
            throw new Error(`Tar archive directory entry "${rawName}" must have size 0 (got ${size}).`);
          }
        } else if (size <= 0) {
          throw new Error(`Tar archive contains empty (0-byte) regular file: "${rawName}"`);
        }

        const entryKey = cleanRel || '.';
        if (entries.has(entryKey)) {
          throw new Error(`Duplicate path "${entryKey}" inside WASM demo tar archive.`);
        }
        entries.set(entryKey, { type: typeChar, size, rawName });

        const paddedSize = Math.ceil(size / 512) * 512;
        skipDataBytes = paddedSize;
      }
    }
  } finally {
    source.destroy();
    gunzip.destroy();
    if (source.fd !== null && !source.closed) {
      await new Promise((resolve) => source.once('close', resolve));
    }
  }

  if (skipDataBytes > 0 || pending.length > 0) {
    throw new Error('Truncated tar entry or trailing partial block inside WASM demo archive.');
  }
  if (!sawEofBlocks) {
    throw new Error('Tar archive is missing zero-filled end-of-archive blocks.');
  }

  const requiredEntries = [
    'index.html',
    'wasm-boot.js',
    'qtloader.js',
    `${brand.productId}.js`,
    `${brand.productId}.wasm`,
    `${brand.productId}.wasm.br`,
    'app_icon.png',
    'licenses/LICENSE',
    'licenses/LICENSE.LGPLv3',
    'licenses/COPYRIGHT',
    'licenses/THIRD-PARTY-NOTICES.txt',
  ];
  for (const req of requiredEntries) {
    const rec = entries.get(req);
    if (!rec || rec.type !== '0' || rec.size <= 0) {
      throw new Error(`WASM demo archive is missing required regular file "${req}".`);
    }
  }

  const sha256 = computeFileSha256(resolvedArchive);
  return {
    archivePath: resolvedArchive,
    compressedBytes: st.size,
    unpackedBytes,
    entryCount: entries.size,
    sha256,
  };
}

/**
 * Validates `stageDir`, builds a deterministic POSIX USTAR `.tar.gz` archive at `outputPath`,
 * writes `<outputPath>.sha256`, and verifies the resulting archive against `deploy-receive.sh demo`.
 */
export async function packageWasmDemo(options = {}) {
  const repoRoot = path.resolve(options.repoRoot || DEFAULT_REPO_ROOT);
  const prepared = await validateAndPrepareWasmStage(options.stageDir, {
    repoRoot,
    requireThreads: Boolean(options.requireThreads),
    requireVcpkgCopyrights: Boolean(options.requireVcpkgCopyrights),
    autoBrotli: options.autoBrotli !== false,
  });

  const defaultOut = path.join(repoRoot, 'dist', 'wasm', `${prepared.brand.productId}-wasm-demo.tar.gz`);
  const resolvedOut = path.resolve(options.outputPath || defaultOut);
  const outDir = path.dirname(resolvedOut);
  fs.mkdirSync(outDir, { recursive: true });
  if (fs.lstatSync(outDir).isSymbolicLink()) {
    throw new Error(`Refusing symlink output directory: ${outDir}`);
  }
  if (fs.existsSync(resolvedOut) && fs.lstatSync(resolvedOut).isSymbolicLink()) {
    throw new Error(`Refusing to overwrite symlink output path: ${resolvedOut}`);
  }

  const mtimeSec =
    process.env.SOURCE_DATE_EPOCH && /^\d+$/.test(process.env.SOURCE_DATE_EPOCH)
      ? Number.parseInt(process.env.SOURCE_DATE_EPOCH, 10)
      : DETERMINISTIC_MTIME_SEC;

  const tmpArchive = path.join(
    outDir,
    `.wasm-demo-tmp-${process.pid}-${Date.now()}-${Math.random().toString(16).slice(2, 10)}.tar.gz`
  );
  let tmpSha = '';

  try {
    const tarReadable = Readable.from(generateTarBlocks(prepared.entries, mtimeSec));
    const gzip = zlib.createGzip({ level: 6, mtime: 0 });
    const outStream = fs.createWriteStream(tmpArchive, { flags: 'wx', mode: 0o644 });
    await pipeline(tarReadable, gzip, outStream);

    const verified = await verifyWasmDemoArchive(tmpArchive, { repoRoot });
    fs.renameSync(tmpArchive, resolvedOut);

    const shaFile = `${resolvedOut}.sha256`;
    if (fs.existsSync(shaFile) && fs.lstatSync(shaFile).isSymbolicLink()) {
      throw new Error(`Refusing to overwrite symlink checksum file: ${shaFile}`);
    }
    tmpSha = `${shaFile}.tmp-${process.pid}-${Date.now()}`;
    fs.writeFileSync(tmpSha, `${verified.sha256}  ${path.basename(resolvedOut)}\n`, {
      encoding: 'utf8',
      mode: 0o644,
      flag: 'wx',
    });
    fs.renameSync(tmpSha, shaFile);
    tmpSha = '';

    return {
      ...verified,
      archivePath: resolvedOut,
      sha256Path: shaFile,
      hasSharedMemory: prepared.wasmCaps.hasSharedMemory,
    };
  } catch (err) {
    for (const p of [tmpArchive, tmpSha]) {
      try {
        if (p && fs.existsSync(p)) {
          fs.unlinkSync(p);
        }
      } catch {
        // Ignore cleanup error on failure path.
      }
    }
    throw err;
  }
}

function parseCli(argv) {
  const opts = {
    requireThreads: false,
    requireVcpkgCopyrights: false,
    autoBrotli: true,
  };
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
      case '--stage':
        opts.stageDir = next();
        break;
      case '--output':
        opts.outputPath = next();
        break;
      case '--validate-stage-only':
        opts.validateStageOnly = next();
        break;
      case '--verify-archive-only':
        opts.verifyArchiveOnly = next();
        break;
      case '--require-threads':
        opts.requireThreads = true;
        break;
      case '--require-vcpkg-copyrights':
        opts.requireVcpkgCopyrights = true;
        break;
      case '--no-auto-brotli':
        opts.autoBrotli = false;
        break;
      case '--help':
      case '-h':
        opts.help = true;
        break;
      default:
        throw new Error(`Unknown option "${arg}". Run with --help for usage.`);
    }
  }
  return opts;
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  try {
    const cli = parseCli(process.argv.slice(2));
    if (cli.help) {
      process.stdout.write(
        [
          'Usage: node ci/package-wasm-demo.mjs [options]',
          '',
          'Options:',
          '  --stage <dir>                  Staging directory to validate and archive',
          '  --output <tar.gz>              Output .tar.gz path (default: dist/wasm/<product>-wasm-demo.tar.gz)',
          '  --validate-stage-only <dir>    Validate stage directory (CSP, WASM, Brotli, licenses/) without archiving',
          '  --verify-archive-only <tar.gz> Verify an existing .tar.gz against deploy-receive.sh demo rules',
          '  --require-threads              Require WebAssembly shared linear memory (-pthread / wasm-multi)',
          '  --require-vcpkg-copyrights     Require vcpkg third-party/<port>-COPYRIGHT files under licenses/',
          '  --no-auto-brotli               Fail instead of generating <product>.wasm.br when absent',
          '',
        ].join('\n')
      );
      process.exit(0);
    }

    const modeCount =
      (cli.stageDir ? 1 : 0) + (cli.validateStageOnly ? 1 : 0) + (cli.verifyArchiveOnly ? 1 : 0);
    if (modeCount !== 1) {
      throw new Error(
        'Specify exactly one of --stage <dir>, --validate-stage-only <dir>, or --verify-archive-only <tar.gz>.'
      );
    }

    if (cli.validateStageOnly) {
      const res = await validateAndPrepareWasmStage(cli.validateStageOnly, {
        requireThreads: cli.requireThreads,
        requireVcpkgCopyrights: cli.requireVcpkgCopyrights,
        autoBrotli: cli.autoBrotli,
      });
      process.stdout.write(
        `==> verified WASM stage: ${res.stageDir} (${res.entries.length} entries, ${res.totalContentBytes} bytes, sharedMemory=${res.wasmCaps.hasSharedMemory})\n`
      );
    } else if (cli.verifyArchiveOnly) {
      const res = await verifyWasmDemoArchive(cli.verifyArchiveOnly);
      process.stdout.write(
        `==> verified WASM demo archive: ${res.archivePath} (${res.compressedBytes} B compressed, ${res.unpackedBytes} B unpacked, ${res.entryCount} entries, sha256=${res.sha256})\n`
      );
    } else {
      const res = await packageWasmDemo(cli);
      process.stdout.write(
        `==> packaged WASM demo archive: ${res.archivePath}\n` +
          `    compressed: ${(res.compressedBytes / (1024 * 1024)).toFixed(2)} MiB (${res.compressedBytes} B)\n` +
          `    unpacked:   ${(res.unpackedBytes / (1024 * 1024)).toFixed(2)} MiB (${res.unpackedBytes} B)\n` +
          `    entries:    ${res.entryCount} (sharedMemory=${res.hasSharedMemory})\n` +
          `    sha256:     ${res.sha256}\n`
      );
    }
  } catch (err) {
    process.stderr.write(`error: ${err instanceof Error ? err.message : String(err)}\n`);
    process.exit(1);
  }
}
