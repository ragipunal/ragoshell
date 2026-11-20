// Turns the output of dev/ui-shots/geometry.sh (run over sizes and languages) into Markdown tables:
//   node dev/ui-shots/matrix-table.mjs matrix.txt
// One table per window size: a row per view, a column per language; "ok" or the number of items beyond an edge
// (or buttons with cut text) that the guard found.
import { readFileSync } from 'node:fs';

const file = process.argv[2];
if (!file) {
    console.error('usage: node matrix-table.mjs <geometry output>');
    process.exit(2);
}
const sizes = new Map();   // "360x800 Touch" -> Map(view -> Map(lang -> n))
const langs = [];
const views = [];
for (const line of readFileSync(file, 'utf8').split(/\r?\n/)) {
    const m = /^== (\S+) @ (\S+) (\S+) (\S+)? ?: (\d+) item/.exec(line);
    if (!m) continue;
    const [, view, size, density, lang = 'en', n] = m;
    const key = `${size} ${density}`;
    if (!sizes.has(key)) sizes.set(key, new Map());
    const byView = sizes.get(key);
    if (!byView.has(view)) byView.set(view, new Map());
    byView.get(view).set(lang, Number(n));
    if (!langs.includes(lang)) langs.push(lang);
    if (!views.includes(view)) views.push(view);
}
for (const [key, byView] of sizes) {
    console.log(`\n**${key}**\n`);
    console.log(`| view | ${langs.join(' | ')} |`);
    console.log(`|---|${langs.map(() => '---').join('|')}|`);
    for (const view of views) {
        const row = byView.get(view);
        if (!row) continue;
        console.log(`| ${view} | ${langs.map(l => (row.get(l) === undefined ? '' : row.get(l) === 0 ? 'ok' : String(row.get(l)))).join(' | ')} |`);
    }
}
