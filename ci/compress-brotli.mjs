#!/usr/bin/env node
/**
 * Zero-dependency streaming Brotli compressor for WebAssembly build artifacts.
 * Used as a deterministic cross-platform fallback when the `brotli` CLI binary
 * is not installed on the build host (e.g. Windows local builds).
 */

import fs from 'node:fs';
import path from 'node:path';
import { pipeline } from 'node:stream/promises';
import { pathToFileURL } from 'node:url';
import zlib from 'node:zlib';

export async function compressFileBrotli(inputPath, outputPath, quality = 9) {
  const inResolved = path.resolve(inputPath);
  const outResolved = path.resolve(outputPath);
  if (inResolved === outResolved) {
    throw new Error(`Input and output paths must differ: ${inResolved}`);
  }
  if (!Number.isInteger(quality) || quality < 0 || quality > 11) {
    throw new Error(`Invalid Brotli quality "${quality}" (expected integer 0..11).`);
  }
  if (!fs.existsSync(inResolved)) {
    throw new Error(`Input file does not exist: ${inResolved}`);
  }
  const st = fs.lstatSync(inResolved);
  if (st.isSymbolicLink() || !st.isFile()) {
    throw new Error(`Input path must be a regular file (not a symlink): ${inResolved}`);
  }
  if (st.size <= 0) {
    throw new Error(`Refusing to Brotli-compress empty file: ${inResolved}`);
  }

  const outDir = path.dirname(outResolved);
  fs.mkdirSync(outDir, { recursive: true });
  if (fs.existsSync(outResolved) && fs.lstatSync(outResolved).isSymbolicLink()) {
    throw new Error(`Refusing to overwrite symlink output path: ${outResolved}`);
  }

  const tmpPath = path.join(
    outDir,
    `.brotli-tmp-${process.pid}-${Date.now()}-${Math.random().toString(16).slice(2, 10)}`
  );

  try {
    const brotli = zlib.createBrotliCompress({
      params: {
        [zlib.constants.BROTLI_PARAM_QUALITY]: quality,
        [zlib.constants.BROTLI_PARAM_SIZE_HINT]: st.size,
      },
    });
    const source = fs.createReadStream(inResolved);
    const dest = fs.createWriteStream(tmpPath, { flags: 'wx', mode: 0o644 });
    await pipeline(source, brotli, dest);

    const outStat = fs.lstatSync(tmpPath);
    if (!outStat.isFile() || outStat.size <= 0) {
      throw new Error(`Brotli compression produced an empty output for ${inResolved}`);
    }
    fs.renameSync(tmpPath, outResolved);
    return { inputBytes: st.size, outputBytes: outStat.size };
  } catch (err) {
    try {
      if (fs.existsSync(tmpPath)) {
        fs.unlinkSync(tmpPath);
      }
    } catch {
      // Ignore cleanup error on failure path.
    }
    throw err;
  }
}

function parseCli(argv) {
  let quality = 9;
  const positional = [];
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === '--quality' || arg === '-q') {
      if (i + 1 >= argv.length) {
        throw new Error(`${arg} requires an integer value (0..11).`);
      }
      i += 1;
      quality = Number.parseInt(argv[i], 10);
    } else if (arg.startsWith('-')) {
      throw new Error(`Unknown option "${arg}". Usage: node ci/compress-brotli.mjs [--quality 9] <in> <out>`);
    } else {
      positional.push(arg);
    }
  }
  if (positional.length !== 2) {
    throw new Error('Usage: node ci/compress-brotli.mjs [--quality 9] <input-file> <output-file>');
  }
  return { inputPath: positional[0], outputPath: positional[1], quality };
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  try {
    const { inputPath, outputPath, quality } = parseCli(process.argv.slice(2));
    const res = await compressFileBrotli(inputPath, outputPath, quality);
    process.stdout.write(
      `==> brotli-compressed ${path.basename(inputPath)} (${res.inputBytes} B -> ${res.outputBytes} B, q=${quality})\n`
    );
  } catch (err) {
    process.stderr.write(`error: ${err instanceof Error ? err.message : String(err)}\n`);
    process.exit(1);
  }
}
