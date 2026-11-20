// Writes a .icns from the PNG files of an .iconset folder, without macOS' iconutil (so the icons can be made on
// any machine): node dev/icons/png2icns.mjs <folder.iconset> <out.icns>
// An .icns is the tag 'icns', its total length, then entries of (4-letter type, length, PNG data); macOS 10.7 and
// later read PNG entries for every size.
import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const [dir, out] = process.argv.slice(2);
if (!dir || !out) {
    console.error('usage: node png2icns.mjs <folder.iconset> <out.icns>');
    process.exit(2);
}

// iconset file -> icns entry type
const entries = [
    ['icon_16x16.png', 'icp4'],
    ['icon_32x32.png', 'icp5'],
    ['icon_16x16@2x.png', 'ic11'],
    ['icon_32x32@2x.png', 'ic12'],
    ['icon_128x128.png', 'ic07'],
    ['icon_128x128@2x.png', 'ic13'],
    ['icon_256x256.png', 'ic08'],
    ['icon_256x256@2x.png', 'ic14'],
    ['icon_512x512.png', 'ic09'],
    ['icon_512x512@2x.png', 'ic10'],
];

const parts = [];
for (const [file, type] of entries) {
    const png = readFileSync(join(dir, file));
    const head = Buffer.alloc(8);
    head.write(type, 0, 'ascii');
    head.writeUInt32BE(png.length + 8, 4);
    parts.push(head, png);
}
const body = Buffer.concat(parts);
const header = Buffer.alloc(8);
header.write('icns', 0, 'ascii');
header.writeUInt32BE(body.length + 8, 4);
writeFileSync(out, Buffer.concat([header, body]));
