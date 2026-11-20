#!/usr/bin/env python3
"""Moves strings between Qt .ts files and plain JSON, for translators that are
not Qt Linguist (people with a spreadsheet, scripts, LLM drafts).

    # 1. List the strings once (all languages share the same source strings).
    python ci/ts-exchange.py export src/ragoshell-app/i18n/ragoshell_tr.ts work/ --chunks 4
        work/index.json     id -> context / source / comment (do not edit)
        work/chunk_1.json   [{"id": 0, "context": ..., "source": ..., "note": ..., "forms": 1}, ...]

    # 2. The translator writes work/<lang>_1.json ... : {"0": "translation", "7": ["one", "other"]}
    #    (a plural message gets a list with one text per plural form of that language)

    # 3. Put the translations into a language's .ts file.
    python ci/ts-exchange.py import src/ragoshell-app/i18n/ragoshell_de.ts work/index.json work/de_*.json

    # Check a translator's files before importing (coverage, placeholders, line breaks).
    python ci/ts-exchange.py check work/index.json work/de_*.json

    # Optional: how many plural forms does a language need?
    python ci/ts-exchange.py forms src/ragoshell-app/i18n/ragoshell_ru.ts

After an import run `cmake --build <dir> --target update_translations` so lupdate
re-normalises the file, then `node ci/check-i18n.mjs`.
See docs/Localization.md.
"""
import json
import re
import sys

MESSAGE = re.compile(r"<message(?P<attrs>[^>]*)>(?P<body>.*?)</message>", re.S)
CONTEXT = re.compile(r"<context>\s*<name>(?P<name>.*?)</name>(?P<body>.*?)</context>", re.S)
TRANSLATION = re.compile(r"<translation(?P<attrs>[^>]*?)(?:/>|>(?P<inner>.*?)</translation>)", re.S)


def unescape(text):
    return (text.replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", '"')
                .replace("&apos;", "'").replace("&amp;", "&"))


def escape(text):
    return (text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


def tag(body, name):
    m = re.search(rf"<{name}>(.*?)</{name}>", body, re.S)
    return unescape(m.group(1)) if m else ""


def read(path):
    with open(path, encoding="utf-8", newline="") as f:
        return f.read()


def messages(text):
    """Yields (context, message match) in file order."""
    for c in CONTEXT.finditer(text):
        for m in MESSAGE.finditer(c.group("body")):
            yield unescape(c.group("name")), m


def forms_of(body):
    tr = TRANSLATION.search(body)
    if not tr:
        return 1
    return max(1, len(re.findall(r"<numerusform", tr.group("inner") or "")))


def cmd_export(ts, outdir, chunks):
    import os
    text = read(ts)
    os.makedirs(outdir, exist_ok=True)
    items, index = [], {}
    for i, (ctx, m) in enumerate(messages(text)):
        body = m.group("body")
        numerus = 'numerus="yes"' in m.group("attrs")
        note = " ".join(x for x in (tag(body, "extracomment"), tag(body, "comment")) if x)
        item = {"id": i, "context": ctx, "source": tag(body, "source")}
        if note:
            item["note"] = note
        if numerus:
            item["plural"] = True
        items.append(item)
        index[str(i)] = {"context": ctx, "source": item["source"], "comment": tag(body, "comment")}
    with open(os.path.join(outdir, "index.json"), "w", encoding="utf-8") as f:
        json.dump(index, f, ensure_ascii=False, indent=1)
    size = -(-len(items) // chunks)
    for n in range(chunks):
        part = items[n * size:(n + 1) * size]
        with open(os.path.join(outdir, f"chunk_{n + 1}.json"), "w", encoding="utf-8") as f:
            json.dump(part, f, ensure_ascii=False, indent=1)
        print(f"chunk_{n + 1}.json: {len(part)} strings (ids {part[0]['id']}..{part[-1]['id']})")
    return 0


def cmd_import(ts, index_path, files):
    with open(index_path, encoding="utf-8") as f:
        index = json.load(f)
    by_key = {}
    for i, e in index.items():
        crlf = lambda t: t.replace(chr(13) + chr(10), chr(10))
        by_key[(crlf(e["context"]), crlf(e["source"]), crlf(e["comment"]))] = i
    translations = {}
    for path in files:
        with open(path, encoding="utf-8") as f:
            translations.update(json.load(f))

    text = read(ts)
    crlf = "\r\n" in text
    text = text.replace("\r\n", "\n")
    applied, skipped, errors = 0, 0, []

    def fix_message(ctx):
        def run(m):
            nonlocal applied, skipped
            body = m.group("body")
            key = (ctx, tag(body, "source"), tag(body, "comment"))
            i = by_key.get(key)
            value = translations.get(i) if i is not None else None
            if value is None or value == "" or value == []:
                skipped += 1
                errors.append(f"no translation for ({ctx}) {key[1][:70]!r}")
                return m.group(0)
            numerus = 'numerus="yes"' in m.group("attrs")
            tr = TRANSLATION.search(body)
            if numerus:
                need = forms_of(body)
                forms = value if isinstance(value, list) else [value]
                if len(forms) != need:
                    errors.append(f"id {i}: {need} plural forms needed, got {len(forms)}: {key[1][:60]}")
                    skipped += 1
                    return m.group(0)
                inner = "\n" + "".join(f"            <numerusform>{escape(f)}</numerusform>\n" for f in forms) + "        "
            else:
                if isinstance(value, list):
                    errors.append(f"id {i}: a list was given for a message without plural forms")
                    skipped += 1
                    return m.group(0)
                inner = escape(value)
            new_tr = f"<translation>{inner}</translation>"
            applied += 1
            return m.group(0)[:m.start("body") - m.start(0)] + body[:tr.start()] + new_tr + body[tr.end():] + "</message>"
        return run

    def fix_context(c):
        ctx = unescape(c.group("name"))
        body = MESSAGE.sub(fix_message(ctx), c.group("body"))
        return c.group(0)[:c.start("body") - c.start(0)] + body + "</context>"

    text = CONTEXT.sub(fix_context, text)
    if crlf:
        text = text.replace("\n", "\r\n")
    with open(ts, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    print(f"{ts}: {applied} translations applied, {skipped} left open")
    for e in errors:
        print("  " + e)
    return 1 if errors else 0


def cmd_check(index_path, files):
    with open(index_path, encoding="utf-8") as f:
        index = json.load(f)
    translations = {}
    for path in files:
        with open(path, encoding="utf-8") as f:
            translations.update(json.load(f))
    token = re.compile(r"%(?:\d{1,2}|n)")
    problems = 0
    for i, e in index.items():
        value = translations.get(i)
        if value is None or value == "" or value == []:
            print(f"id {i}: missing: {e['source'][:70]}")
            problems += 1
            continue
        want = sorted(t for t in token.findall(e["source"]) if t != "%n")
        for form in (value if isinstance(value, list) else [value]):
            if not isinstance(form, str) or not form.strip():
                print(f"id {i}: empty form")
                problems += 1
                continue
            got = sorted(t for t in token.findall(form) if t != "%n")
            if got != want:
                print(f"id {i}: placeholders {want} expected, found {got}: {form[:70]}")
                problems += 1
            if form.count(chr(10)) != e["source"].count(chr(10)):
                print(f"id {i}: line breaks differ: {form[:70]}")
                problems += 1
    extra = [i for i in translations if i not in index]
    for i in extra:
        print(f"id {i}: not in the index")
        problems += 1
    print(f"{len(translations)} of {len(index)} strings translated, {problems} problem(s)")
    return 1 if problems else 0


def cmd_forms(ts):
    text = read(ts)
    counts = {}
    for _, m in messages(text):
        if 'numerus="yes"' in m.group("attrs"):
            n = forms_of(m.group("body"))
            counts[n] = counts.get(n, 0) + 1
    print(counts)
    return 0


if __name__ == "__main__":
    a = sys.argv[1:]
    if len(a) >= 3 and a[0] == "export":
        sys.exit(cmd_export(a[1], a[2], int(a[a.index("--chunks") + 1]) if "--chunks" in a else 4))
    if len(a) >= 4 and a[0] == "import":
        sys.exit(cmd_import(a[1], a[2], a[3:]))
    if len(a) >= 3 and a[0] == "check":
        sys.exit(cmd_check(a[1], a[2:]))
    if len(a) == 2 and a[0] == "forms":
        sys.exit(cmd_forms(a[1]))
    sys.exit(__doc__)
