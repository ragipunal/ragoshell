#!/usr/bin/env bash
# Every application icon of every platform, made from the one artwork assets/icon/ragoshell-icon.svg
# (docs/DesignSystem.md). Needs ImageMagick 7 (`magick`) with its SVG renderer; Windows (Git Bash),
# Linux and macOS all work. Run it from anywhere after the artwork changed and commit what it writes:
#
#   dev/icons/make-icons.sh            # everything
#   dev/icons/make-icons.sh --sheet    # also writes build/icons-sheet.png, a picture of all variants to look at
#
# What it writes
#   install/windows/ragoshell.ico, install/windows/icon-set/*.png      Windows: the squircle, edge to edge
#   install/macosx/ragoshell.iconset/*.png, ragoshell.icns             macOS: Big Sur grid (art 824 of 1024, shadow)
#   install/linux/icons/hicolor/<n>x<n>/apps/ragoshell.png, scalable/  Linux: freedesktop theme
#   install/android/res/mipmap-*/                                      Android: legacy + round + adaptive layers
#   install/windows/nsis-sidebar.bmp, nsis-topbar.bmp, wix-dialog.bmp  Windows installers: the wordmark (assets/icon/ragoshell-installer-*.svg)
#   install/android/play-store-icon-512.png                            Google Play listing
#   install/ios/Assets.xcassets/AppIcon.appiconset/                    iOS and iPadOS: 1024 light / dark / tinted
#   src/ragoshell-app/resources/app_icon.png                            the icon inside the app (window, About, top bar)
#
# Variants are derived from the SVG, not drawn again: the emblem (the flower without the squircle) is the SVG with
# its two background rectangles removed; the background is its gradient as a full square; the monochrome layer
# of Android's themed icons is the emblem's silhouette; the iOS tinted icon is the emblem in grey on black.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# Git Bash on Windows: the native ImageMagick does not know /tmp or /c/..., so paths are given as C:/...
if command -v cygpath >/dev/null; then root="$(cygpath -m "$root")"; fi
src="$root/assets/icon/ragoshell-icon.svg"
work="$(mktemp -d)"
if command -v cygpath >/dev/null; then work="$(cygpath -m "$work")"; fi
trap 'rm -rf "$work"' EXIT

command -v magick >/dev/null || { echo "ImageMagick 7 (magick) is required" >&2; exit 1; }

# --- the layers, as SVG ------------------------------------------------------------------------------------------
# emblem: the flower on a transparent canvas
sed -e '/<rect x="0" y="0" width="512" height="512"/d' -e '/<rect x="5" y="5" width="502"/d' "$src" > "$work/emblem.svg"
grep -q '<rect' "$work/emblem.svg" && { echo "emblem: a background rectangle is left in the artwork" >&2; exit 1; }
# background: the squircle's gradient as a full square (the system masks the corners)
cat > "$work/bg.svg" <<'SVG'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" width="512" height="512">
  <defs>
    <linearGradient id="bg" x1="0%" y1="0%" x2="0%" y2="100%">
      <stop offset="0%" stop-color="#0c2319"/>
      <stop offset="100%" stop-color="#05140e"/>
    </linearGradient>
  </defs>
  <rect width="512" height="512" fill="url(#bg)"/>
</svg>
SVG

# svg -> png at an exact size (rendered large, then reduced: the reduction is what keeps small sizes crisp)
render() { # svg size out
    magick -background none -density 576 "$1" -filter Lanczos -resize "${2}x${2}" -depth 8 "PNG32:$3"
}
# the artwork of a layer at 'art' pixels inside a canvas of 'size' (transparent margin)
inset() { # png art size out
    magick "$1" -resize "${2}x${2}" -background none -gravity center -extent "${3}x${3}" -depth 8 "PNG32:$4"
}

mkdir -p "$work/p"
for s in 16 20 22 24 32 40 44 48 64 72 96 128 144 192 256 512 1024; do
    render "$src" "$s" "$work/p/squircle-$s.png"
done
render "$work/emblem.svg" 1024 "$work/emblem-1024.png"
render "$work/bg.svg" 1024 "$work/bg-1024.png"
# the full-bleed icon (iOS, Android's round icon, Play): background and emblem, nothing else
magick "$work/bg-1024.png" "$work/emblem-1024.png" -compose over -composite "$work/full-1024.png"
# silhouette of the emblem: white where the emblem is, transparent elsewhere
magick "$work/emblem-1024.png" -alpha extract -threshold 5% -background white -alpha shape "PNG32:$work/mono-1024.png"

# --- the PNG sizes of the artwork (assets/icon/png), for anything that wants a plain picture ----------------------
mkdir -p "$root/assets/icon/png"
for s in 16 20 24 32 40 48 64 128 256 512 1024; do
    if [ -f "$work/p/squircle-$s.png" ]; then cp "$work/p/squircle-$s.png" "$root/assets/icon/png/ragoshell-icon-$s.png"
    else render "$src" "$s" "$root/assets/icon/png/ragoshell-icon-$s.png"; fi
done

# --- Windows -----------------------------------------------------------------------------------------------------
win="$root/install/windows"
mkdir -p "$win/icon-set"
for s in 16 20 24 32 40 48 64 128 256; do
    cp "$work/p/squircle-$s.png" "$win/icon-set/${s}x${s}.png"
done
magick "$win/icon-set/16x16.png" "$win/icon-set/20x20.png" "$win/icon-set/24x24.png" "$win/icon-set/32x32.png" \
       "$win/icon-set/40x40.png" "$win/icon-set/48x48.png" "$win/icon-set/64x64.png" "$win/icon-set/128x128.png" \
       "$win/icon-set/256x256.png" "$win/ragoshell.ico"
cp "$win/ragoshell.ico" "$root/assets/icon/ragoshell.ico"

# --- macOS: the Big Sur grid keeps the art inside 824 of 1024 pixels and puts a soft shadow under it ----------------
mac="$root/install/macosx"
set_icon() { # size out: the squircle at 80.5 % on a transparent canvas, with the Apple-style shadow
    local size=$1 out=$2
    local art=$(( size * 824 / 1024 ))
    render "$src" "$art" "$work/mac-art.png"
    magick "$work/mac-art.png" \( +clone -background black -shadow 45x$(( size / 64 + 1 ))+0+$(( size / 85 + 1 )) \) +swap \
        -background none -layers merge +repage -gravity center -extent "${size}x${size}" -depth 8 "PNG32:$out"
}
rm -rf "$mac/ragoshell.iconset"; mkdir -p "$mac/ragoshell.iconset"
for s in 16 32 128 256 512; do
    set_icon "$s" "$mac/ragoshell.iconset/icon_${s}x${s}.png"
    set_icon "$(( s * 2 ))" "$mac/ragoshell.iconset/icon_${s}x${s}@2x.png"
done
# .icns: iconutil is macOS only, so the container is written by a small script (the same entries iconutil makes)
node "$root/dev/icons/png2icns.mjs" "$mac/ragoshell.iconset" "$mac/ragoshell.icns"
cp "$mac/ragoshell.icns" "$root/assets/icon/ragoshell.icns" 2>/dev/null || true
rm -rf "$root/assets/icon/ragoshell.iconset"; cp -r "$mac/ragoshell.iconset" "$root/assets/icon/ragoshell.iconset"

# --- Linux: freedesktop icon theme --------------------------------------------------------------------------------
lin="$root/install/linux/icons/hicolor"
rm -rf "$lin"
for s in 16 22 24 32 48 64 128 256 512; do
    mkdir -p "$lin/${s}x${s}/apps"
    if [ -f "$work/p/squircle-$s.png" ]; then cp "$work/p/squircle-$s.png" "$lin/${s}x${s}/apps/ragoshell.png"
    else render "$src" "$s" "$lin/${s}x${s}/apps/ragoshell.png"; fi
done
mkdir -p "$lin/scalable/apps"; cp "$src" "$lin/scalable/apps/ragoshell.svg"

# --- Android -----------------------------------------------------------------------------------------------------
res="$root/install/android/res"
rm -rf "$res"/mipmap-*   # not "$res": it also holds hand-written files (xml/network_security_config.xml)
density_px() { case $1 in mdpi) echo 1;; hdpi) echo 1.5;; xhdpi) echo 2;; xxhdpi) echo 3;; xxxhdpi) echo 4;; esac; }
for d in mdpi hdpi xhdpi xxhdpi xxxhdpi; do
    f=$(density_px $d)
    legacy=$(awk "BEGIN{printf \"%d\", 48*$f}")   # launcher icon: 48 dp
    layer=$(awk "BEGIN{printf \"%d\", 108*$f}")   # adaptive layers: 108 dp, of which the middle 66 dp are always visible
    out="$res/mipmap-$d"; mkdir -p "$out"
    render "$src" "$legacy" "$out/ic_launcher.png"
    # round (Android 7.1): the full-bleed icon in a circle
    magick "$work/full-1024.png" -resize "${legacy}x${legacy}" \( -size "${legacy}x${legacy}" xc:none -fill white \
        -draw "circle $((legacy/2)),$((legacy/2)) $((legacy/2)),0" \) -compose DstIn -composite -depth 8 "PNG32:$out/ic_launcher_round.png"
    # adaptive layers: the flower 55 % of the layer wide (a circle of 61 % stays visible under every mask shape)
    render "$work/bg.svg" "$layer" "$out/ic_launcher_background.png"
    inset "$work/emblem-1024.png" $(( layer * 88 / 100 )) "$layer" "$out/ic_launcher_foreground.png"
    inset "$work/mono-1024.png"   $(( layer * 88 / 100 )) "$layer" "$out/ic_launcher_monochrome.png"
done
mkdir -p "$res/mipmap-anydpi-v26"
for name in ic_launcher ic_launcher_round; do
cat > "$res/mipmap-anydpi-v26/$name.xml" <<'XML'
<?xml version="1.0" encoding="utf-8"?>
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@mipmap/ic_launcher_background" />
    <foreground android:drawable="@mipmap/ic_launcher_foreground" />
    <monochrome android:drawable="@mipmap/ic_launcher_monochrome" />
</adaptive-icon>
XML
done
render "$work/full-1024.png" 512 "$root/install/android/play-store-icon-512.png" 2>/dev/null \
    || magick "$work/full-1024.png" -resize 512x512 -depth 8 "PNG32:$root/install/android/play-store-icon-512.png"

# --- iOS / iPadOS ------------------------------------------------------------------------------------------------
ios="$root/install/ios/Assets.xcassets/AppIcon.appiconset"
rm -rf "$ios"; mkdir -p "$ios"
magick "$work/full-1024.png" -background "#05140e" -alpha remove -alpha off -depth 8 "PNG24:$ios/icon-1024.png"
cp "$ios/icon-1024.png" "$ios/icon-1024-dark.png"
# tinted: a grey flower on black; the system colours it
magick -size 1024x1024 xc:black \( "$work/emblem-1024.png" -colorspace Gray \) -compose over -composite -alpha off -depth 8 "PNG24:$ios/icon-1024-tinted.png"
cat > "$ios/Contents.json" <<'JSON'
{
  "images" : [
    { "filename" : "icon-1024.png", "idiom" : "universal", "platform" : "ios", "size" : "1024x1024" },
    { "appearances" : [ { "appearance" : "luminosity", "value" : "dark" } ],
      "filename" : "icon-1024-dark.png", "idiom" : "universal", "platform" : "ios", "size" : "1024x1024" },
    { "appearances" : [ { "appearance" : "luminosity", "value" : "tinted" } ],
      "filename" : "icon-1024-tinted.png", "idiom" : "universal", "platform" : "ios", "size" : "1024x1024" }
  ],
  "info" : { "author" : "xcode", "version" : 1 }
}
JSON
cat > "$root/install/ios/Assets.xcassets/Contents.json" <<'JSON'
{ "info" : { "author" : "xcode", "version" : 1 } }
JSON

# --- Windows installer art: the pictures that carry the wordmark ---------------------------------------------------------
# The art SVGs list a CSS font stack; ImageMagick's SVG renderer (librsvg) reads only its first family, which is system-ui
# and comes out thin. A family installed here goes first, in a temporary copy (RAGO_ART_FONT overrides the default).
art_font="${RAGO_ART_FONT:-Segoe UI}"
render_art() { # svg width height out.bmp: rendered at 4x, reduced, flattened on white, 24-bit BMP as NSIS and WiX want
    sed "s/font-family=\"system-ui[^\"]*\"/font-family=\"$art_font\"/" "$1" > "$work/art.svg"
    magick -background none -density 384 "$work/art.svg" -filter Lanczos -resize "${2}x${3}!" \
        -background white -alpha remove -alpha off -depth 8 -type TrueColor "BMP3:$4"
}
render_art "$root/assets/icon/ragoshell-installer-side.svg" 164 314 "$win/nsis-sidebar.bmp"
render_art "$root/assets/icon/ragoshell-installer-top.svg" 150 57 "$win/nsis-topbar.bmp"
# the WiX dialog: the same panel, 2 px shorter, at the left of a white 493x312 page (wix-banner.bmp carries no wordmark)
magick "$win/nsis-sidebar.bmp" -gravity center -crop 164x312+0+0 +repage "$work/dialog-side.png"
magick -size 493x312 xc:white "$work/dialog-side.png" -geometry +0+0 -composite -depth 8 -type TrueColor "BMP3:$win/wix-dialog.bmp"

# --- inside the app ----------------------------------------------------------------------------------------------
size=$(magick identify -format '%w' "$root/src/ragoshell-app/resources/app_icon.png" 2>/dev/null || echo 256)
render "$src" "$size" "$root/src/ragoshell-app/resources/app_icon.png"

# --- a picture to look at ----------------------------------------------------------------------------------------
if [ "${1:-}" = "--sheet" ]; then
    mkdir -p "$root/build"
    a=$(ls "$res/mipmap-xxxhdpi")
    magick -size 2400x760 xc:'#7d7d7d' \
        \( "$work/p/squircle-256.png" \) -geometry +20+20 -composite \
        \( "$mac/ragoshell.iconset/icon_256x256.png" \) -geometry +300+20 -composite \
        \( "$res/mipmap-xxxhdpi/ic_launcher_round.png" -resize 192x192 \) -geometry +600+50 -composite \
        \( "$res/mipmap-xxxhdpi/ic_launcher.png" -resize 192x192 \) -geometry +850+50 -composite \
        \( "$ios/icon-1024.png" -resize 256x256 \) -geometry +1100+20 -composite \
        \( "$ios/icon-1024-tinted.png" -resize 256x256 \) -geometry +1400+20 -composite \
        \( "$res/mipmap-xxxhdpi/ic_launcher_background.png" "$res/mipmap-xxxhdpi/ic_launcher_foreground.png" -compose over -composite -resize 256x256 \) -geometry +1700+20 -composite \
        \( "$res/mipmap-xxxhdpi/ic_launcher_monochrome.png" -background '#444' -alpha remove -resize 256x256 \) -geometry +2000+20 -composite \
        -depth 8 "$root/build/icons-sheet.png"
    echo "sheet: build/icons-sheet.png ($a)"
fi
echo "icons written"
