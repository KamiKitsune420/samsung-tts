"""For every voice, find how long a run of letters or digits its front end takes before it faults.

    python tests/token_limits.py [--raw] [--voices work/voice] [voice ...]

Each voice is given runs of one character (w, x, a, q, W, 7), of the alphabet and of "h7", 33 to 1100 long, and
stopped at the first piece of audio, so only the text front end works. Prints, per voice, how many of the 48
lines fault and the shortest faulting run of each kind. This is where the limits in engine/sstts.c
(token_limit) come from: with --raw the engine is run with that guard off (SSTTS_TOKEN_LIMIT=0) to see what the
front ends do on their own; without it, to see that the guard holds. Results also go to
work/token_limits.txt. Needs work/engine/engine.exe.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UNITS = ("w", "x", "a", "q", "W", "7", "abcdefghijklmnopqrstuvwxyz", "h7")
LENGTHS = (33, 64, 128, 256, 400, 1100)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    voices_dir = os.path.join(ROOT, "work", "voice")
    if "--voices" in sys.argv:
        voices_dir = os.path.abspath(sys.argv[sys.argv.index("--voices") + 1])
        args.remove(sys.argv[sys.argv.index("--voices") + 1])
    lines = [(u[:3], n, (u * (n // len(u) + 1))[:n]) for u in UNITS for n in LENGTHS]
    tmp = os.path.join(ROOT, "work", "token_limits")
    os.makedirs(tmp, exist_ok=True)
    corpus = os.path.join(tmp, "runs.txt")
    with open(corpus, "w", newline="\n") as f:
        f.write("\n".join(l[2] for l in lines) + "\n")
    env = dict(os.environ)
    if "--raw" in sys.argv:
        env["SSTTS_TOKEN_LIMIT"] = "0"
    total = 0
    with open(os.path.join(ROOT, "work", "token_limits.txt"), "w") as out:
        for v in args or sorted(os.listdir(voices_dir)):
            try:
                r = subprocess.run([os.path.join(ROOT, "work", "engine", "engine.exe"), voices_dir, corpus, tmp, "--fast",
                                    "--stop-after", "1", "--voice", v], capture_output=True, text=True, errors="replace",
                                   timeout=1200, env=env)
                if "open:" in r.stderr and "result" not in r.stderr:
                    msg = "%s: does not load" % v
                else:
                    bad = sorted({int(m.group(1)) for m in re.finditer(r"^(\d+): the engine faulted", r.stderr, re.M)})
                    total += len(bad)
                    low = {}
                    for i in bad:
                        low[lines[i][0]] = min(low.get(lines[i][0], 1 << 30), lines[i][1])
                    msg = "%s: %d of %d fault%s" % (v, len(bad), len(lines),
                                                    "; shortest: " + " ".join("%s=%d" % kv for kv in low.items()) if low else "")
            except subprocess.TimeoutExpired:
                msg = "%s: TIMEOUT" % v
                total += 1
            print(msg, flush=True)
            out.write(msg + "\n")
            out.flush()
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
