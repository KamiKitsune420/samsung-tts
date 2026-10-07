"""Test samsungtts.dll (driver/) through ctypes, the way an add-on would use it.

    python tests/driver_test.py [--dll dist/SamsungTTS-Driver/samsungtts.dll] [--voices work/voice]

Checks: the audio is what the engine's command-line driver gives for the same voice and text; word positions lie
inside the text; a stop from the audio function and a stop from another thread both end the call at once; a voice
that is not there is an error with a message, not a crash; a voice root with non-ASCII characters in its path
works; two threads speaking at once both get their own audio. Needs en_US_l03 and en_AU_f00 under the voice root
and work/engine/engine.exe. Exit status 0 if everything held.
"""
import os
import shutil
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "driver"))
import example

TEXT = "The quick brown fox jumps over the lazy dog. It costs $12.50, or so Dr. Smith said."
failures = []


def check(what, ok, detail=""):
    print("%s: %s%s" % ("ok  " if ok else "FAIL", what, (" (" + detail + ")") if detail and not ok else ""))
    if not ok:
        failures.append(what)


def engine_pcm(voices, voice, text, tmp):
    os.makedirs(tmp, exist_ok=True)
    corpus = os.path.join(tmp, "text.txt")
    with open(corpus, "w", encoding="utf8", newline="\n") as f:
        f.write(text + "\n")
    subprocess.run([os.path.join(ROOT, "work", "engine", "engine.exe"), voices, corpus, tmp, "--fast", "--voice", voice],
                   capture_output=True, timeout=300)
    with open(os.path.join(tmp, "00000.pcm"), "rb") as f:
        return f.read()


def main():
    opt = lambda name, default: sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default
    dll_path = os.path.abspath(opt("--dll", os.path.join(ROOT, "dist", "SamsungTTS-Driver", "samsungtts.dll")))
    voices = os.path.abspath(opt("--voices", os.path.join(ROOT, "work", "voice")))
    tmp = os.path.join(ROOT, "work", "driver_test")
    dll = example.load(dll_path)
    print("samsungtts", dll.samsungtts_version().decode())

    hz = dll.samsungtts_sample_rate(voices.encode("utf8"), b"en_US_l03")
    check("sample rate of en_US_l03 is 24000", hz == 24000, str(hz))

    for voice in ("en_US_l03", "en_AU_f00"):
        result, rate, pcm, words = example.speak(dll, voices, voice, TEXT)
        ref = engine_pcm(voices, voice, TEXT, tmp)
        check("%s: spoken to the end" % voice, result == 0 and len(pcm) > 48000, "result %d, %d bytes" % (result, len(pcm)))
        check("%s: the same samples as engine.exe" % voice, pcm == ref, "%d bytes against %d" % (len(pcm), len(ref)))
        n = len(TEXT.encode("utf8"))
        check("%s: word positions inside the text" % voice, len(words) > 5 and all(0 <= o and o + l <= n and l > 0 for _, o, l in words),
              "%d words" % len(words))

    result, rate, half, _ = example.speak(dll, voices, "en_US_l03", TEXT, volume=50)
    full = example.speak(dll, voices, "en_US_l03", TEXT)[2]
    check("volume 50 gives the same length, quieter", result == 0 and len(half) == len(full) and max(half) <= max(full) and half != full)

    # a stop from the audio function
    count = {"n": 0}

    def stop_after_five(user, pcm, samples, sample_rate, word_offset, word_length):
        count["n"] += 1
        return 1 if count["n"] == 5 else 0

    cb = example.AUDIO(stop_after_five)
    t = time.perf_counter()
    result = dll.samsungtts_speak(voices.encode("utf8"), b"en_US_l03", (TEXT * 4).encode("utf8"), 100, 100, 100, cb, None)
    check("stop from the audio function: result 1, no more audio", result == 1 and count["n"] == 5,
          "result %d after %d pieces, %.2f s" % (result, count["n"], time.perf_counter() - t))

    # a stop from another thread, once audio is flowing
    got = {"n": 0, "at": 0.0}

    def count_pieces(user, pcm, samples, sample_rate, word_offset, word_length):
        got["n"] += 1
        if got["n"] == 10:
            threading.Thread(target=lambda: (got.__setitem__("at", time.perf_counter()), dll.samsungtts_stop())).start()
        return 0

    cb2 = example.AUDIO(count_pieces)
    result = dll.samsungtts_speak(voices.encode("utf8"), b"en_US_l03", (TEXT * 4).encode("utf8"), 100, 100, 100, cb2, None)
    took = time.perf_counter() - got["at"]
    check("stop from another thread: result 1 within half a second", result == 1 and took < 0.5, "result %d, %.3f s" % (result, took))

    # and the engine is none the worse for being stopped
    again = example.speak(dll, voices, "en_US_l03", TEXT)[2]
    check("after the stops, the same samples as before", again == full)

    result = example.speak(dll, voices, "xx_XX_l99", TEXT)[0]
    err = dll.samsungtts_last_error().decode("utf8", "replace")
    check("a voice that is not there: result -1 and a message", result == -1 and "xx_XX_l99" in err, "result %d, %r" % (result, err))
    result = example.speak(dll, voices, "nonsense", TEXT)[0]
    check("a voice name that is not one: result -1", result == -1, str(result))

    # a path that is not ASCII (an add-on lives under the user's profile, whatever the user is called)
    odd = os.path.join(tmp, "Zoë Müller 声")
    if not os.path.exists(os.path.join(odd, "en_AU_f00", "cfg")):
        shutil.copytree(os.path.join(voices, "en_AU_f00"), os.path.join(odd, "en_AU_f00"), dirs_exist_ok=True)
    result, rate, pcm, _ = example.speak(dll, odd, "en_AU_f00", TEXT)
    check("a voice root with non-ASCII characters", result == 0 and pcm == engine_pcm(voices, "en_AU_f00", TEXT, tmp),
          "result %d: %s" % (result, dll.samsungtts_last_error().decode("utf8", "replace")))

    # two callers at once
    out = {}

    def caller(name, voice):
        out[name] = example.speak(dll, voices, voice, TEXT)

    threads = [threading.Thread(target=caller, args=("a", "en_US_l03")), threading.Thread(target=caller, args=("b", "en_AU_f00"))]
    for th in threads:
        th.start()
    for th in threads:
        th.join()
    check("two threads at once each get their own voice's audio",
          out["a"][0] == 0 and out["b"][0] == 0 and out["a"][2] == full and out["b"][2] == engine_pcm(voices, "en_AU_f00", TEXT, tmp))

    # the engine's two known crashes, which the guards in engine/guards.c put right
    r1 = example.speak(dll, voices, "hi_IN_f00", "O'Neil's")
    r2 = example.speak(dll, voices, "it_IT_l01", "A" * 64)
    check("Hindi, a word with two apostrophes: speaks", r1[0] == 0 and len(r1[2]) > 9600, "result %d" % r1[0])
    check("Italian, a word of sixty-four phonemes: speaks", r2[0] == 0 and len(r2[2]) > 9600, "result %d" % r2[0])

    # a run of Latin letters longer than the Chinese and Thai front ends take: given to the engine in pieces
    long_text = "hello " + "w" * 200 + " world"
    for voice in ("zh_CN_l02", "th_TH_f00"):
        if not os.path.exists(os.path.join(voices, voice, "cfg")):
            print("skip: %s is not under the voice root" % voice)
            continue
        r = example.speak(dll, voices, voice, long_text, rate=300)
        n = len(long_text)
        check("%s, two hundred letters in a row: speaks, word positions inside the text" % voice,
              r[0] == 0 and len(r[2]) > 9600 and all(0 <= o and o + l <= n for _, o, l in r[3]),
              "result %d: %s" % (r[0], dll.samsungtts_last_error().decode("utf8", "replace")[:200]))

    print("%d failed" % len(failures) if failures else "all passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
