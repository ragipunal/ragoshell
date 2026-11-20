#!/usr/bin/env node
// Checks the UI translations (docs/Localization.md):
//
//   1. No sentence is built by gluing text onto qsTr(): word order differs
//      between languages, so every value goes in as %1 and .arg().
//   2. Every translation keeps the placeholders (%1, %n ...) of its source.
//   3. Prints how complete each language is. Missing translations are not an
//      error; the app falls back to English.
//
//   node ci/check-i18n.mjs            # exit code 1 on a problem
//
// Uses only Node built-in modules (dev/CI check only; Node.js is not part of the app).
import { readdirSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const qmlDir = join(root, 'src', 'ragoshell-app', 'qml');
const tsDir = join(root, 'src', 'ragoshell-app', 'i18n');
let problems = 0;

const fail = (message) => {
  problems++;
  console.error(`error: ${message}`);
};

// ---- 1. gluing text onto qsTr() --------------------------------------------
// "● " + qsTr("Query") and qsTr("...") + "\n" glue only symbols or whitespace,
// which is fine; anything with letters is a sentence in pieces.
// Backslash escapes (a newline written as backslash-n) are not letters.
const hasLetters = (literal) => /\p{L}/u.test(literal.replace(/\\./g, ''));
const LITERAL = String.raw`(?:"(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*')`;
const glueBefore = new RegExp(`(${LITERAL}|[\\w.\\])]+)\\s*\\+\\s*qsTr\\(`, 'gu');
const glueAfter = new RegExp(`qsTr\\((?:[^()]|\\([^()]*\\))*\\)(?:\\.arg\\((?:[^()]|\\([^()]*\\))*\\))*\\s*\\+\\s*(${LITERAL}|[\\w.]+)`, 'gu');

for (const name of readdirSync(qmlDir).filter((f) => f.endsWith('.qml'))) {
  const lines = readFileSync(join(qmlDir, name), 'utf8').split(/\r?\n/);
  lines.forEach((line, index) => {
    const code = line.replace(/^\s*\/\/.*$/, '');
    if (!code.includes('qsTr(')) return;
    for (const m of code.matchAll(glueBefore)) {
      const left = m[1];
      const isLiteral = /^["']/.test(left);
      if (isLiteral && !hasLetters(left)) continue;
      fail(`${name}:${index + 1}: text is glued before qsTr(); use one string with %1 and .arg(): ${line.trim()}`);
    }
    for (const m of code.matchAll(glueAfter)) {
      const right = m[1];
      const isLiteral = /^["']/.test(right);
      if (isLiteral && !hasLetters(right)) continue;
      fail(`${name}:${index + 1}: text is glued after qsTr(); use one string with %1 and .arg(): ${line.trim()}`);
    }
  });
}

// ---- 2 + 3. .ts files ---------------------------------------------------------
const unescapeXml = (s) => s.replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&quot;/g, '"')
  .replace(/&apos;/g, "'").replace(/&amp;/g, '&');
const placeholders = (s) => (s.match(/%(?:\d{1,2}|n)/g) ?? []).sort().join(',');

const rows = [];
for (const name of readdirSync(tsDir).filter((f) => f.endsWith('.ts')).sort()) {
  const text = readFileSync(join(tsDir, name), 'utf8');
  const language = /<TS[^>]*\slanguage="([^"]+)"/.exec(text)?.[1] ?? name;
  let total = 0;
  let finished = 0;
  for (const m of text.matchAll(/<message([^>]*)>([\s\S]*?)<\/message>/g)) {
    const numerus = /numerus="yes"/.test(m[1]);
    const source = unescapeXml(/<source>([\s\S]*?)<\/source>/.exec(m[2])?.[1] ?? '');
    const tr = /<translation([^>]*)>([\s\S]*?)<\/translation>/.exec(m[2]);
    total++;
    if (!tr || /type="(unfinished|vanished|obsolete)"/.test(tr[1])) continue;
    const forms = numerus
      ? [...tr[2].matchAll(/<numerusform[^>]*>([\s\S]*?)<\/numerusform>/g)].map((f) => unescapeXml(f[1]))
      : [unescapeXml(tr[2])];
    if (forms.every((f) => f.trim() === '')) continue;
    finished++;
    for (const form of forms) {
      if (form.trim() === '') continue;
      const want = placeholders(source);
      // A plural form may leave out %n (Arabic "two" form can say "both"); the other
      // placeholders must all be there.
      const got = placeholders(form);
      const wantNoN = want.split(',').filter((p) => p && p !== '%n').join(',');
      const gotNoN = got.split(',').filter((p) => p && p !== '%n').join(',');
      if (wantNoN !== gotNoN) {
        fail(`${name}: placeholders differ (${want || 'none'} vs ${got || 'none'}): ${source}`);
      }
    }
    if (numerus === false && placeholders(source) !== placeholders(forms[0])) {
      // already reported above for the non-%n ones; %n in a singular message is a mistake
      if (placeholders(source).includes('%n') !== placeholders(forms[0]).includes('%n')) {
        fail(`${name}: %n mismatch: ${source}`);
      }
    }
  }
  rows.push({ file: name, language, finished, total });
}

console.log('\nTranslation completeness (finished / total):');
for (const r of rows) {
  const pct = r.total ? Math.round((100 * r.finished) / r.total) : 0;
  console.log(`  ${r.language.padEnd(8)} ${String(r.finished).padStart(5)} / ${String(r.total).padEnd(5)} ${String(pct).padStart(3)}%  ${r.file}`);
}

if (problems > 0) {
  console.error(`\n${problems} problem(s) found.`);
  process.exit(1);
}
console.log('\ni18n check passed.');
