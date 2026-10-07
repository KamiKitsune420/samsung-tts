"""Download a Samsung TTS voice pack from Samsung's store and unpack it for the engine.

    python tools/fetch_voice.py <code> [--profile s24|legacy] [--root work/voice] [--keep-apk]

<code> is e.g. en_us_l04 (python tools/catalogue.py lists them). Writes <root>/<ll>_<CC>_<type><nn>/ with the
pack's cfg, lng, voice model (.ivc) and cache.tsv, which is the layout the engine looks for, and prints the
SHA-256 of the pack's own arm64 engine library so that packs can be grouped by engine build.
"""
import hashlib, io, os, re, sys, urllib.request, zipfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import catalogue


LANGUAGES = {
    "cs_CZ": "Czech", "da_DK": "Danish", "de_DE": "German", "el_GR": "Greek", "en_AU": "Australian English",
    "en_GB": "UK English", "en_IN": "Indian English", "en_US": "US English", "es_ES": "European Spanish",
    "es_MX": "Mexican Spanish", "es_US": "US Spanish", "fi_FI": "Finnish", "fr_CA": "Canadian French",
    "fr_FR": "French", "hi_IN": "Hindi", "hu_HU": "Hungarian", "id_ID": "Indonesian", "it_IT": "Italian",
    "ja_JP": "Japanese", "ko_KR": "Korean", "nb_NO": "Norwegian", "nl_NL": "Dutch", "pl_PL": "Polish",
    "pt_BR": "Brazilian Portuguese", "pt_PT": "European Portuguese", "ro_RO": "Romanian", "ru_RU": "Russian",
    "sk_SK": "Slovak", "sv_SE": "Swedish", "th_TH": "Thai", "tr_TR": "Turkish", "vi_VN": "Vietnamese",
    "zh_CN": "Simplified Chinese", "zh_HK": "Cantonese", "zh_TW": "Taiwanese Mandarin"}


def model_name(folder):
    """The voice's own name from its cfg: the string after the key `name` (Stephanie, Julia ...), or None."""
    try:
        data = open(os.path.join(folder, "cfg"), "rb").read()
    except OSError:
        return None
    strings = [m.group().decode("ascii") for m in re.finditer(rb"[ -~]{2,}", data)]
    for a, b in zip(strings, strings[1:]):
        if a == "name" and re.fullmatch(r"[A-Za-z][A-Za-z .'-]{1,30}", b):
            return b
    return None


def write_info(folder, code, meta):
    """info.txt: what the SAPI registration and the voice centre show for this pack."""
    ll, cc, rest = code.split("_")
    locale = "%s_%s" % (ll, cc.upper())
    # Many packs name their voice (Stephanie, Marie, Jan); some say only Male or Female, and some nothing usable
    # (the key that follows, or a name in a script this does not read). Those are labelled by their code:
    # f and m packs are a female and a male voice, l and g packs are numbered.
    name = model_name(folder)
    if name in ("Male", "Female"):
        name += " voice"
    elif not name or name in ("version", "name"):
        name = {"f": "Female voice", "m": "Male voice"}.get(rest[0], "Voice " + rest)
    with open(os.path.join(folder, "info.txt"), "w", encoding="utf8", newline="\n") as f:
        f.write("name=%s\nlanguage=%s\nversion=%s\nproduct=%s\n"
                % (name, LANGUAGES.get(locale, locale), meta["version"], meta["name"]))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opt = lambda name, default: sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default
    code = args[0]
    profile, root = opt("--profile", "s24"), opt("--root", os.path.join("work", "voice"))
    meta = catalogue.query(code, profile)
    if not meta["url"]:
        sys.exit("not offered: %s (%s)" % (code, meta["message"]))
    print("%s %s, %.1f MB, %s" % (code, meta["version"], meta["size"] / 1e6, meta["name"].encode("ascii", "replace").decode()))
    os.makedirs(os.path.join("work", "apk"), exist_ok=True)
    apk = os.path.join("work", "apk", "lang_%s-%s.apk" % (code, meta["version"]))
    if not os.path.exists(apk) or os.path.getsize(apk) != meta["size"]:
        with urllib.request.urlopen(meta["url"], timeout=60) as r, open(apk + ".part", "wb") as f:
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
        os.replace(apk + ".part", apk)
    ll, cc, rest = code.split("_")
    folder = os.path.join(root, "%s_%s_%s" % (ll, cc.upper(), rest))
    os.makedirs(folder, exist_ok=True)
    with zipfile.ZipFile(apk) as z:
        names = z.namelist()
        for n in names:
            if n.startswith("assets/") and not n.endswith("/"):
                with z.open(n) as src, open(os.path.join(folder, os.path.basename(n)), "wb") as dst:
                    dst.write(src.read())
        lib = "lib/arm64-v8a/libsamsungtts.so"
        digest = hashlib.sha256(z.read(lib)).hexdigest() if lib in names else None
    write_info(folder, code, meta)
    print("unpacked to %s: %s" % (folder, " ".join(sorted(os.listdir(folder)))))
    print("engine library sha256: %s" % digest)
    if "--keep-apk" not in sys.argv:
        os.remove(apk)


if __name__ == "__main__":
    main()
