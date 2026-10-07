"""Compare two renderings of the same corpus, sample by sample.

    python tools/compare_runs.py <oracle dir> <other dir>

Each directory holds NNNNN.pcm (or .wav) per line and index.tsv, as tools/oracle/run_oracle.py and the translated
engine write them. Prints one line per utterance and the count of identical ones. Exit status 0 only if all match.
"""
import os, sys, wave
import numpy as np


def load(d, stem):
    p = os.path.join(d, stem + ".pcm")
    if os.path.exists(p):
        return np.fromfile(p, "<i2")
    p = os.path.join(d, stem + ".wav")
    if os.path.exists(p):
        with wave.open(p) as w:
            return np.frombuffer(w.readframes(w.getnframes()), "<i2")
    return None


def main():
    a_dir, b_dir = sys.argv[1], sys.argv[2]
    stems = sorted({os.path.splitext(f)[0] for f in os.listdir(a_dir) if f[:5].isdigit()})
    same = 0
    for s in stems:
        a, b = load(a_dir, s), load(b_dir, s)
        if a is None or b is None:
            print("%s: missing in %s" % (s, a_dir if a is None else b_dir))
            continue
        if len(a) == len(b) and np.array_equal(a, b):
            same += 1
            print("%s: identical, %d samples" % (s, len(a)))
        else:
            n = min(len(a), len(b))
            diff = np.flatnonzero(a[:n] != b[:n])
            first = int(diff[0]) if len(diff) else n
            print("%s: DIFFERENT: %d vs %d samples, first difference at sample %d (%.3f s), %d of the common %d differ"
                  % (s, len(a), len(b), first, first / 24000.0, len(diff), n))
    print("%d of %d utterances identical" % (same, len(stems)))
    return 0 if same == len(stems) and stems else 1


if __name__ == "__main__":
    sys.exit(main())
