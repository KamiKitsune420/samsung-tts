"""Render a short sample of every voice the voice centre lists, for it to play before a voice is downloaded.

    python tools/make_samples.py [--out dist/SamsungTTS/samples]

Reads work/try_voices.tsv for the packs that speak and renders one sentence in each pack's language with the
engine (work/engine/engine.exe) from the packs unpacked under work/voice. Writes <code>.wav, 24 kHz mono.
The clips are Samsung's voices speaking: like the installer they go into, they are not for publishing.
"""
import os, subprocess, sys, wave
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import try_voices


def first_sentence(text):
    for stop in (". ", "。", "। "):
        i = text.find(stop)
        if i > 0:
            return text[:i + len(stop)].strip()
    return text


def main():
    out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else os.path.join("dist", "SamsungTTS", "samples")
    os.makedirs(out, exist_ok=True)
    tmp = os.path.join("work", "sample_tmp")
    os.makedirs(tmp, exist_ok=True)
    done, total = 0, 0.0
    for line in open(os.path.join("work", "try_voices.tsv"), encoding="utf8"):
        code, version, status = line.rstrip("\n").split("\t")[:3]
        if status != "speaks":
            continue
        ll, cc, kind = code.split("_")
        voice = "%s_%s_%s" % (ll, cc.upper(), kind)
        corpus = os.path.join(tmp, "text.txt")
        with open(corpus, "w", encoding="utf8", newline="\n") as f:
            f.write(first_sentence(try_voices.TEXT.get(ll, try_voices.TEXT["en"])) + "\n")
        r = subprocess.run([os.path.join("work", "engine", "engine.exe"), os.path.join("work", "voice"), corpus, tmp, "--fast", "--voice", voice,
                            "--end-silence", "100"], capture_output=True, text=True, encoding="utf8", errors="replace", timeout=300)
        pcm = os.path.join(tmp, "00000.pcm")
        data = open(pcm, "rb").read() if os.path.exists(pcm) else b""
        if r.returncode or len(data) < 4800:
            print("no sample for %s: %s" % (code, (r.stdout + r.stderr).strip()[-120:]))
            continue
        with wave.open(os.path.join(out, code + ".wav"), "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(24000); w.writeframes(data)
        os.remove(pcm)
        done += 1; total += len(data) / 48000
    print("%d samples in %s, %.0f s of audio in all" % (done, out, total))


if __name__ == "__main__":
    main()
