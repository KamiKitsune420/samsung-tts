"""Write center/catalogue_more.inc from the results of tools/try_voices.py.

    python tools/make_catalogue.py [work/try_voices.tsv]

One line per pack that the engine spoke with, other than the five English ones voice_center.cpp lists itself:
its code, language, the voice's own name (from the pack's cfg, via info.txt) and a sample sentence in its
language. The packs must still be unpacked under work/voice, where their info.txt is.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import try_voices

BUILT_IN = {"en_us_l03", "en_us_g02", "en_us_l04", "en_us_l05", "en_gb_l02"}


def wide(s):
    """A C wide string literal, non-ASCII characters as universal character names."""
    out = []
    for ch in s:
        if ch in '"\\':
            out.append("\\" + ch)
        elif ord(ch) < 127:
            out.append(ch)
        else:
            out.append("\\u%04x" % ord(ch))
    return 'L"%s"' % "".join(out)


def main():
    tsv = sys.argv[1] if len(sys.argv) > 1 else os.path.join("work", "try_voices.tsv")
    rows = [l.rstrip("\n").split("\t") for l in open(tsv, encoding="utf8")]
    entries, skipped = [], []
    for code, version, status, *rest in rows:
        if code in BUILT_IN:
            continue
        if status != "speaks":
            skipped.append("%s (%s)" % (code, status))
            continue
        ll, cc, kind = code.split("_")
        info = dict(l.split("=", 1) for l in open(os.path.join("work", "voice", "%s_%s_%s" % (ll, cc.upper(), kind), "info.txt"),
                                                  encoding="utf8").read().splitlines() if "=" in l)
        entries.append((info["language"], code, info["name"], try_voices.TEXT.get(ll, try_voices.TEXT["en"])))
    entries.sort()
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "..", "center", "catalogue_more.inc"), "w", encoding="ascii", newline="\n") as f:
        f.write("// Written by tools/make_catalogue.py from tools/try_voices.py results: packs the engine spoke with.\n")
        if skipped:
            f.write("// Not listed, because the engine did not speak with them: %s\n" % ", ".join(skipped))
        for language, code, name, sample in entries:
            f.write("    {%s, %s, %s, %s},\n" % (wide(code), wide(language), wide(name), wide(sample)))
    print("%d packs listed, %d left out: %s" % (len(entries), len(skipped), ", ".join(skipped)))


if __name__ == "__main__":
    main()
