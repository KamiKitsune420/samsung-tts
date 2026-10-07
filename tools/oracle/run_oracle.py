"""Run Samsung's own engine over a corpus on an Android device or emulator, and pull the audio.

    python tools/oracle/run_oracle.py <corpus.txt> <outdir> [--pack DIR] [--verbose]

One utterance a line. Writes NNNNN.wav (24 kHz mono) and index.tsv (line, samples, result, callbacks, seconds, text)
to <outdir>. --pack is an unpacked voice pack APK (default work/l03 in this repository, which git ignores): its
lib/arm64-v8a/libsamsungtts.so and assets/ are pushed once to /data/local/tmp/smt. Build oracle first (build.ps1).
"""
import argparse, os, shutil, subprocess, sys, wave

HERE = os.path.dirname(os.path.abspath(__file__))
REMOTE = "/data/local/tmp/smt"
VOICE = "en_US_l03"


def adb(*args, capture=False):
    exe = shutil.which("adb") or os.path.expanduser("~/AppData/Local/Android/Sdk/platform-tools/adb.exe")
    r = subprocess.run([exe] + list(args), capture_output=capture, text=True)
    return r.stdout if capture else r.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus"); ap.add_argument("outdir")
    ap.add_argument("--pack", default=os.path.join(HERE, "..", "..", "work", "l03"))
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()

    vdir = "%s/v/%s" % (REMOTE, VOICE)
    if "regular.ivc" not in adb("shell", "ls %s 2>/dev/null" % vdir, capture=True):
        adb("shell", "mkdir -p " + vdir)
        adb("push", os.path.join(a.pack, "lib", "arm64-v8a", "libsamsungtts.so"), REMOTE + "/")
        for f in ("cfg", "lng", "regular.ivc", "cache.tsv"):
            adb("push", os.path.join(a.pack, "assets", f), vdir + "/")
    adb("push", os.path.join(HERE, "oracle"), REMOTE + "/")
    adb("push", a.corpus, REMOTE + "/corpus.txt")
    adb("shell", "cd %s && chmod 755 oracle && rm -rf out && mkdir out && ./oracle %s/libsamsungtts.so %s/v corpus.txt out %s"
        % (REMOTE, REMOTE, REMOTE, "-v 2>out/log.txt" if a.verbose else "2>out/log.txt"))
    os.makedirs(a.outdir, exist_ok=True)
    adb("pull", REMOTE + "/out/.", a.outdir)
    n = 0
    for name in sorted(os.listdir(a.outdir)):
        if name.endswith(".pcm"):
            p = os.path.join(a.outdir, name)
            with open(p, "rb") as f, wave.open(p[:-4] + ".wav", "wb") as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(24000); w.writeframes(f.read())
            os.remove(p); n += 1
    print("%d utterances in %s" % (n, a.outdir))


if __name__ == "__main__":
    sys.exit(main())
