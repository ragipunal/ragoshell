#!/usr/bin/env python3
"""Fills the English plural forms in src/ragoshell-app/i18n/ragoshell_en.ts.

English is the source language, so ragoshell_en.ts only exists to give "%n" its
two forms: "1 document" / "2 documents". lupdate adds the new plural messages
with empty forms; run this after `update_translations` to fill them.

    python ci/fill-english-plurals.py [path/to/ragoshell_en.ts]

The rule: "noun(s)" gives "noun" / "nouns" (first occurrence); a few sources
that spell the plural out are listed in SPECIAL.
"""
import re
import sys

SPECIAL = {
    "Replica Set (%n nodes)": ("Replica Set (%n node)", "Replica Set (%n nodes)"),
    "Delete %n Documents…": ("Delete %n Document…", "Delete %n Documents…"),
    "Delete %n Documents": ("Delete %n Document", "Delete %n Documents"),
    "Permanently delete %n selected documents from '%1.%2'?": (
        "Permanently delete %n selected document from '%1.%2'?",
        "Permanently delete %n selected documents from '%1.%2'?",
    ),
}

MESSAGE = re.compile(
    r'(<message numerus="yes">.*?<source>)(.*?)(</source>.*?)<translation[^>]*>.*?</translation>',
    re.S,
)


def unescape(text: str) -> str:
    return (text.replace("&lt;", "<").replace("&gt;", ">")
                .replace("&quot;", '"').replace("&apos;", "'").replace("&amp;", "&"))


def escape(text: str) -> str:
    # The same escaping lupdate writes, so a refresh leaves the file unchanged.
    return (text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
                .replace('"', "&quot;").replace("'", "&apos;"))


def forms(source: str):
    if source in SPECIAL:
        return SPECIAL[source]
    if "(s)" in source:
        return source.replace("(s)", "", 1), source.replace("(s)", "s", 1)
    raise SystemExit(f"no English plural rule for: {source}")


def fill(match: re.Match) -> str:
    source = unescape(match.group(2))
    one, many = forms(source)
    return (f'{match.group(1)}{match.group(2)}{match.group(3)}<translation>\n'
            f'            <numerusform>{escape(one)}</numerusform>\n'
            f'            <numerusform>{escape(many)}</numerusform>\n'
            f'        </translation>')


def main(path: str) -> int:
    with open(path, encoding="utf-8", newline="") as f:
        text = f.read()
    crlf = "\r\n" in text
    text = text.replace("\r\n", "\n")
    filled, count = MESSAGE.subn(fill, text)
    if crlf:
        filled = filled.replace("\n", "\r\n")
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(filled)
    print(f"{path}: {count} plural messages")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "src/ragoshell-app/i18n/ragoshell_en.ts"))
