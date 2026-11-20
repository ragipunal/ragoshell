#!/usr/bin/env python3
"""Builds a pseudo-locale Qt translation file from a populated .ts file.

Every translatable string becomes accented, about 40 % longer and wrapped in
brackets, e.g. "Save" -> "[Šàvé ~]". Placeholders (%1, %n) and the & mnemonic
marker survive. Used to find text that is not translatable (it stays plain
English) and layouts that break when text grows (a cut-off "]").

    python ci/pseudo-translate.py src/ragoshell-app/i18n/ragoshell_tr.ts pseudo.ts
    lrelease pseudo.ts -qm pseudo.qm
    ragoshell --qm-file pseudo.qm

See docs/Localization.md.
"""
import math
import re
import sys
import xml.etree.ElementTree as ET

ACCENTS = str.maketrans(
    "AEIOUYaeiouycCnNsSzZ",
    "ÀÉÎÕÜÝàéîõûÿčČñÑšŠžŽ",
)
# %1 .. %99, %n and Qt's literal %% pass through untouched.
TOKEN = re.compile(r"(%\d{1,2}|%n|%%|<[^>]+>|&&)")


def pseudo(text: str) -> str:
    if not text.strip():
        return text
    parts = TOKEN.split(text)
    out = []
    letters = 0
    for part in parts:
        if TOKEN.fullmatch(part):
            out.append(part)
        else:
            letters += sum(c.isalpha() for c in part)
            out.append(part.translate(ACCENTS))
    filler = "~" * max(1, math.ceil(letters * 0.4))
    return "[" + "".join(out) + " " + filler + "]"


def main(src: str, dst: str) -> int:
    tree = ET.parse(src)
    root = tree.getroot()
    root.set("language", "en_XA")
    count = 0
    for message in root.iter("message"):
        source = message.findtext("source") or ""
        translation = message.find("translation")
        if translation is None:
            translation = ET.SubElement(message, "translation")
        for attr in ("type",):
            translation.attrib.pop(attr, None)
        if message.get("numerus") == "yes":
            for form in list(translation):
                translation.remove(form)
            for _ in range(2):
                ET.SubElement(translation, "numerusform").text = pseudo(source)
        else:
            translation.text = pseudo(source)
        count += 1
    ET.indent(tree)
    with open(dst, "wb") as f:
        f.write(b'<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE TS>\n')
        tree.write(f, encoding="utf-8", xml_declaration=False)
    print(f"{dst}: {count} strings")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1], sys.argv[2]))
