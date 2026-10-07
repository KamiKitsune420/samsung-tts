"""Download voice packs and see whether the translated engine can speak with them.

    python tools/try_voices.py <code> [<code> ...] [--all] [--out work/try_voices.tsv]

For each pack: fetch it (tools/fetch_voice.py), run the engine on a sentence in that language, and record whether
it spoke, how fast, and what stopped it if it did not. A trap names the original function that is missing or
stubbed. Results are appended to the TSV: code, version, status, seconds of audio, times real time, detail.
"""
import os, re, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import catalogue

TEXT = {
    "en": "Hello, this is a test of the voice. The quick brown fox jumps over the lazy dog, 42 times.",
    "ko": "안녕하세요, 음성 테스트입니다. 오늘 날씨가 참 좋네요.",
    "es": "Hola, esta es una prueba de la voz. El zorro marrón salta sobre el perro perezoso, 42 veces.",
    "fr": "Bonjour, ceci est un test de la voix. Le renard brun saute par-dessus le chien paresseux, 42 fois.",
    "de": "Hallo, dies ist ein Test der Stimme. Der schnelle braune Fuchs springt 42 Mal über den faulen Hund.",
    "it": "Ciao, questa è una prova della voce. La volpe marrone salta sopra il cane pigro, 42 volte.",
    "pt": "Olá, este é um teste da voz. A raposa marrom pula sobre o cão preguiçoso, 42 vezes.",
    "zh": "你好，这是语音测试。今天天气很好。",
    "ja": "こんにちは、これは音声のテストです。今日はいい天気ですね。",
    "ru": "Здравствуйте, это проверка голоса. Сегодня хорошая погода, 42 раза.",
    "hi": "नमस्ते, यह आवाज़ का परीक्षण है। आज मौसम अच्छा है।",
    "pl": "Dzień dobry, to jest test głosu. Dziś jest ładna pogoda, 42 razy.",
    "tr": "Merhaba, bu bir ses testidir. Bugün hava çok güzel, 42 kez.",
    "th": "สวัสดี นี่คือการทดสอบเสียง วันนี้อากาศดีมาก",
    "vi": "Xin chào, đây là bài kiểm tra giọng nói. Hôm nay thời tiết đẹp.",
    "id": "Halo, ini adalah tes suara. Cuaca hari ini sangat bagus, 42 kali.",
    "sv": "Hej, det här är ett test av rösten. Vädret är fint idag, 42 gånger.",
    "ro": "Bună ziua, acesta este un test al vocii. Vremea este frumoasă astăzi.",
    "nl": "Hallo, dit is een test van de stem. Het weer is mooi vandaag, 42 keer.",
    "cs": "Dobrý den, toto je test hlasu. Dnes je hezké počasí.",
    "da": "Hej, dette er en test af stemmen. Vejret er fint i dag.",
    "el": "Γεια σας, αυτή είναι μια δοκιμή φωνής. Ο καιρός είναι καλός σήμερα.",
    "fi": "Hei, tämä on äänen testi. Sää on tänään hyvä.",
    "hu": "Jó napot, ez egy hangteszt. Ma szép az idő.",
    "nb": "Hei, dette er en test av stemmen. Været er fint i dag.",
    "sk": "Dobrý deň, toto je test hlasu. Dnes je pekné počasie.",
}


def main():
    codes = [a for a in sys.argv[1:] if not a.startswith("--") and "/" not in a and "\\" not in a]
    if "--all" in sys.argv:
        codes = list(catalogue.CODES)
    out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else os.path.join("work", "try_voices.tsv")
    here = os.path.dirname(os.path.abspath(__file__))
    for code in codes:
        ll, cc, rest = code.split("_")
        voice = "%s_%s_%s" % (ll, cc.upper(), rest)
        if not os.path.exists(os.path.join("work", "voice", voice, "cfg")):
            r = subprocess.run([sys.executable, os.path.join(here, "fetch_voice.py"), code], capture_output=True, text=True, encoding="utf8", errors="replace")
            if r.returncode:
                row = [code, "", "download failed", "", "", (r.stdout + r.stderr).strip()[-200:]]
                open(out, "a", encoding="utf8").write("\t".join(row) + "\n"); print(row); continue
        info = open(os.path.join("work", "voice", voice, "info.txt"), encoding="utf8").read()
        version = (re.search(r"version=(.*)", info) or [None, ""])[1]
        corpus = os.path.join("work", "try_%s.txt" % ll)
        with open(corpus, "w", encoding="utf8", newline="\n") as f:
            f.write(TEXT.get(ll, TEXT["en"]) + "\n")
        outdir = os.path.join("work", "try_out", voice)
        os.makedirs(outdir, exist_ok=True)
        try:
            r = subprocess.run([os.path.join("work", "engine", "engine.exe"), os.path.join("work", "voice"), corpus, outdir, "--fast", "--voice", voice],
                               capture_output=True, text=True, encoding="utf8", errors="replace", timeout=300)
            log = r.stdout + r.stderr
        except subprocess.TimeoutExpired:
            log = "timeout after 300 s"
        m = re.search(r"result (-?\d+), (\d+) samples \(([\d.]+) s\) in ([\d.]+) s: ([\d.]+) x", log)
        trap = re.search(r"a2c: (trap[^\n]*|call to unresolved[^\n]*)|open: [^\n]*|timeout[^\n]*", log)
        if m and int(m.group(2)) > 2400 and m.group(1) == "0":
            row = [code, version, "speaks", m.group(3), m.group(5), ""]
        elif m:
            row = [code, version, "no audio" if int(m.group(2)) <= 2400 else "result " + m.group(1), m.group(3), m.group(5), ""]
        else:
            row = [code, version, "fails", "", "", trap.group(0) if trap else log.strip()[-160:]]
        open(out, "a", encoding="utf8").write("\t".join(row) + "\n")
        print("\t".join(row).encode("ascii", "replace").decode(), flush=True)


if __name__ == "__main__":
    main()
