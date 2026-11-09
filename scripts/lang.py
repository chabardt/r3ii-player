#!/usr/bin/env python3
"""Translations for the r3ii UI (gettext .po files in lang/, no gettext tools needed).

  python3 scripts/lang.py extract   # write lang/messages.pot from every _("...") / N_("...") in src/ui
  python3 scripts/lang.py update    # add new strings to each lang/<code>.po (empty), mark unused ones obsolete
  python3 scripts/lang.py check     # per language: missing / unused strings; fails on %-format mismatches
  python3 scripts/lang.py gen       # compile lang/*.po into src/ui/lang_tables.c (build.sh runs this)

A .po header needs "Language: <code>" and "X-Language-Name: <native name>" (e.g. Français).
See docs/TRANSLATING.md."""
import glob, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = sorted(glob.glob(os.path.join(ROOT, "src/ui/*.c")))
LANG = os.path.join(ROOT, "lang")
OUT = os.path.join(ROOT, "src/ui/lang_tables.c")

C_ESC = {"n": "\n", "t": "\t", "\\": "\\", '"': '"', "'": "'", "r": "\r", "0": "\0"}
def c_unescape(s):
    out, i = [], 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            n = s[i + 1]
            if n == "x":
                m = re.match(r"[0-9a-fA-F]{1,2}", s[i + 2:]); out.append(chr(int(m.group(0), 16))); i += 2 + len(m.group(0)); continue
            if n.isdigit():
                m = re.match(r"[0-7]{1,3}", s[i + 1:]); out.append(chr(int(m.group(0), 8))); i += 1 + len(m.group(0)); continue
            out.append(C_ESC.get(n, n)); i += 2
        else:
            out.append(s[i]); i += 1
    return "".join(out)

def c_escape(s):
    b = s.encode("utf-8"); out = []
    for ch in b:
        c = chr(ch)
        if c == "\\": out.append("\\\\")
        elif c == '"': out.append('\\"')
        elif c == "\n": out.append("\\n")
        elif c == "\t": out.append("\\t")
        elif ch < 0x20 or ch >= 0x7f: out.append("\\%03o" % ch)   # octal: never swallows the next char
        else: out.append(c)
    return "".join(out)

CALL = re.compile(r'\bN?_\(\s*((?:"(?:\\.|[^"\\\n])*"\s*)+)\)')
LIT = re.compile(r'"((?:\\.|[^"\\\n])*)"')
def extract():
    """{msgid: [file:line, ...]} in first-seen order"""
    found = {}
    for path in SRC:
        if path.endswith("lang_tables.c"): continue
        text = open(path, encoding="utf-8").read()
        for m in CALL.finditer(text):
            # source files are read as UTF-8 text; escapes are plain ASCII in our strings
            raw = "".join(c_unescape(x) for x in LIT.findall(m.group(1)))
            s = raw
            line = text.count("\n", 0, m.start()) + 1
            found.setdefault(s, []).append(f"{os.path.relpath(path, ROOT)}:{line}")
    return found

def po_quote(s):
    s = s.replace("\\", "\\\\").replace('"', '\\"').replace("\t", "\\t")
    if "\n" in s[:-1]:
        parts = s.split("\n")
        lines = [p + "\\n" for p in parts[:-1]] + ([parts[-1]] if parts[-1] else [])
        return '""\n' + "\n".join(f'"{l}"' for l in lines)
    return '"' + s.replace("\n", "\\n") + '"'

def po_unquote(s):
    return c_unescape(s)

def parse_po(path):
    """-> (header dict, [(msgid, msgstr, flags, obsolete)])"""
    entries, cur, key, flags = [], None, None, set()
    def flush():
        nonlocal cur
        if cur is not None: entries.append(cur)
        cur = None
    for raw in open(path, encoding="utf-8").read().splitlines():
        line = raw.strip(); obsolete = line.startswith("#~")
        if obsolete: line = line[2:].strip()
        if line.startswith("#,"):
            flush(); flags = {f.strip() for f in line[2:].split(",")}; continue
        if not line or (line.startswith("#") and not obsolete):
            if not line: flush()
            continue
        m = re.match(r'(msgid|msgstr)\s+"(.*)"$', line)
        if m:
            if m.group(1) == "msgid": flush(); cur = {"id": "", "str": "", "flags": flags, "obsolete": obsolete}; flags = set()
            key = m.group(1); cur["id" if key == "msgid" else "str"] += po_unquote(m.group(2)); continue
        m = re.match(r'"(.*)"$', line)
        if m and cur is not None: cur["id" if key == "msgid" else "str"] += po_unquote(m.group(1))
    flush()
    header = {}
    for e in entries:
        if e["id"] == "":
            for l in e["str"].split("\n"):
                if ":" in l: k, v = l.split(":", 1); header[k.strip()] = v.strip()
    return header, [e for e in entries if e["id"] != ""]

FMT = re.compile(r"%(?:(\d+)\$)?[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z)?([diouxXcsfgeEp%])")
def fmts(s):
    """conversion types in argument order; a translation may reorder with %2$s %1$d"""
    specs = [(m.group(1), m.group(2)) for m in FMT.finditer(s) if m.group(2) != "%"]
    if any(p for p, _ in specs):
        if not all(p for p, _ in specs): return ["mixed positional and plain %"]
        return [t for _, t in sorted(specs, key=lambda x: int(x[0]))]
    return [t for _, t in specs]

def write_po(path, header_text, entries, refs):
    out = [header_text.rstrip("\n"), ""]
    for e in entries:
        if e.get("obsolete"):
            out.append(f"#~ msgid {po_quote(e['id'])}".replace("\n", "\n#~ "))
            out.append(f"#~ msgstr {po_quote(e['str'])}".replace("\n", "\n#~ ")); out.append(""); continue
        for r in refs.get(e["id"], [])[:3]: out.append(f"#: {r}")
        if "c-format" in e.get("flags", set()) or fmts(e["id"]): out.append("#, c-format")
        out.append(f"msgid {po_quote(e['id'])}"); out.append(f"msgstr {po_quote(e['str'])}"); out.append("")
    open(path, "w", encoding="utf-8").write("\n".join(out))

POT_HEADER = '''# r3ii-player UI strings — template. Copy to lang/<code>.po to start a translation.
msgid ""
msgstr ""
"Content-Type: text/plain; charset=UTF-8\\n"
"Language: \\n"
"X-Language-Name: \\n"
'''

def header_text(path):
    lines, started = [], False
    for l in open(path, encoding="utf-8").read().splitlines():
        if l.startswith('msgid ""'): started = True
        if started and not l.strip(): break
        lines.append(l)
    return "\n".join(lines) + "\n"

def cmd_extract():
    found = extract(); os.makedirs(LANG, exist_ok=True)
    write_po(os.path.join(LANG, "messages.pot"), POT_HEADER, [{"id": k, "str": ""} for k in found], found)
    print(f"lang/messages.pot: {len(found)} strings")

def cmd_update():
    found = extract(); cmd_extract()
    for po in sorted(glob.glob(os.path.join(LANG, "*.po"))):
        _, ents = parse_po(po); have = {e["id"]: e for e in ents}
        merged = [{"id": k, "str": have[k]["str"] if k in have else "", "flags": have.get(k, {}).get("flags", set())} for k in found]
        merged += [{"id": e["id"], "str": e["str"], "obsolete": True} for e in ents if e["id"] not in found and e["str"]]
        write_po(po, header_text(po), merged, found)
        print(f"{os.path.basename(po)}: {sum(1 for k in found if k not in have)} new")

def cmd_check():
    found = extract(); bad = 0
    for po in sorted(glob.glob(os.path.join(LANG, "*.po"))):
        h, ents = parse_po(po); live = [e for e in ents if not e["obsolete"]]
        done = {e["id"] for e in live if e["str"] and "fuzzy" not in e["flags"]}
        missing = [k for k in found if k not in done]; unused = [e["id"] for e in live if e["id"] not in found]
        print(f"{os.path.basename(po)} ({h.get('X-Language-Name', '?')}): {len(done & set(found))}/{len(found)} translated, "
              f"{len(missing)} missing, {len(unused)} unused")
        for k in missing[:40]: print(f"   missing: {k!r}  ({found[k][0]})")
        for e in live:
            if e["str"] and fmts(e["id"]) != fmts(e["str"]):
                bad += 1; print(f"   ERROR %-format differs: {e['id']!r} -> {e['str']!r}")
            if not h.get("Language") or not h.get("X-Language-Name"):
                bad += 1; print("   ERROR header needs Language: and X-Language-Name:"); break
    return 1 if bad else 0

def cmd_gen():
    langs = []
    for po in sorted(glob.glob(os.path.join(LANG, "*.po"))):
        h, ents = parse_po(po); code = h.get("Language", "").strip(); name = h.get("X-Language-Name", code)
        if not code: continue
        pairs = {e["id"]: e["str"] for e in ents if e["str"] and not e["obsolete"] and "fuzzy" not in e["flags"]
                 and fmts(e["id"]) == fmts(e["str"])}            # never ship a format mismatch
        langs.append((code, name, sorted(pairs.items(), key=lambda kv: kv[0].encode("utf-8"))))
    out = ["/* Generated by scripts/lang.py gen from lang/*.po — do not edit. */", '#include "lang_tables.h"', ""]
    for code, name, pairs in langs:
        out.append(f"static const lang_entry_t L_{code.replace('-', '_')}[] = {{")
        out += [f'    {{ "{c_escape(k)}", "{c_escape(v)}" }},' for k, v in pairs]
        out.append("};")
    out.append("const lang_t LANGS[] = {")
    out += [f'    {{ "{c_escape(c)}", "{c_escape(n)}", {len(p)}, L_{c.replace("-", "_")} }},' for c, n, p in langs]
    if not langs: out.append("    { 0, 0, 0, 0 }")
    out += ["};", f"const int LANG_N = {len(langs)};", ""]
    new = "\n".join(out)
    if not os.path.exists(OUT) or open(OUT, encoding="utf-8").read() != new:
        open(OUT, "w", encoding="utf-8").write(new)
    print("lang_tables.c: " + (", ".join(f"{c} {len(p)}" for c, n, p in langs) or "no languages"))

if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "check"
    sys.exit({"extract": cmd_extract, "update": cmd_update, "check": cmd_check, "gen": cmd_gen}[cmd]() or 0)
