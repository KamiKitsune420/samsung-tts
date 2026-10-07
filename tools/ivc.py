"""Read a Samsung TTS voice file (.ivc): undo its scrambling, list its sections, parse the LPCNet model.

    python tools/ivc.py <regular.ivc>                 list the sections
    python tools/ivc.py <regular.ivc> --lpcnet        list the LPCNet model's layers
    python tools/ivc.py <regular.ivc> --plain OUT     write the unscrambled payload

Layout (docs/notes.md): a 0x124-byte header with a 16-byte key at 0xac; the payload is XORed with the key,
repeating, phase counted from the payload's start. The payload is u32 1, then sections of u32 type, u32 size, data,
ended by type 0xffffffff.
"""
import struct, sys

HEADER = 0x124
KEY_AT, KEY_LEN = 0xAC, 16
SECTIONS = {1: "encoder", 2: "decoder", 3: "postnet", 5: "symbols", 6: "lpcnet", 10: "speakers", 16: "style tokens"}
LAYER_KINDS = {0: "embed", 1: "dense", 2: "conv1d", 3: "gru", 4: "sparse_gru", 5: "mdense"}


def load(path):
    """The unscrambled payload."""
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:3] != b"ivc":
        raise ValueError("not an ivc file")
    key = raw[KEY_AT:KEY_AT + KEY_LEN]
    body = raw[HEADER:]
    reps = len(body) // KEY_LEN + 1
    return (int.from_bytes(body, "little") ^ int.from_bytes((key * reps)[:len(body)], "little")).to_bytes(len(body), "little")


def sections(plain):
    """[(type, offset, size)] of the payload's sections."""
    out, o = [], 4
    while o + 4 <= len(plain):
        kind, = struct.unpack_from("<I", plain, o)
        if kind == 0xFFFFFFFF:
            break
        size, = struct.unpack_from("<I", plain, o + 4)
        out.append((kind, o + 8, size))
        o += 8 + size
    return out


class Reader:
    def __init__(self, data, pos=0):
        self.d, self.p = data, pos

    def u8(self):
        self.p += 1
        return self.d[self.p - 1]

    def u32(self):
        self.p += 4
        return struct.unpack_from("<I", self.d, self.p - 4)[0]

    def f32(self):
        self.p += 4
        return struct.unpack_from("<f", self.d, self.p - 4)[0]

    def raw(self, n):
        self.p += n
        return self.d[self.p - n:self.p]

    def floats(self, n):
        return (self.p, n, self.raw(4 * n))

    def name(self):
        return self.raw(self.u8()).decode()


def lpcnet(data):
    """The LPCNet section as {'header': {...}, 'layers': [{kind, name, dims..., arrays as (offset, count, bytes)}]}.

    Field meanings in the header are not all established yet; they are kept in file order.
    """
    r = Reader(data)
    magic = r.raw(10)
    if not magic.startswith(b"LPCNET_V"):
        raise ValueError("not an LPCNet model: %r" % magic)
    h = {"magic": magic.decode(), "f0": r.f32(), "i1": r.u32(), "i2": r.u32(), "f3": r.f32(), "f4": r.f32(),
         "f5": r.f32(), "f6": r.f32(), "pitch_embed_dim": r.u32(), "pitch_entries": r.u32()}
    h["table88"] = r.floats(88)
    layers = []
    while r.p < len(data):
        kind = r.u8()
        if kind not in LAYER_KINDS or r.p >= len(data):
            break
        L = {"kind": LAYER_KINDS[kind], "name": r.name()}
        if kind == 0:      # embedding: rows x dim
            L["rows"], L["dim"] = r.u32(), r.u32()
            L["weights"] = r.floats(L["rows"] * L["dim"])
        elif kind == 1:    # dense: activation, in, out, weights, bias
            L["activation"], L["nin"], L["nout"] = r.u8(), r.u32(), r.u32()
            L["weights"] = r.floats(L["nin"] * L["nout"]); L["bias"] = r.floats(L["nout"])
        elif kind == 2:    # conv1d: activation, kernel, in, out
            L["activation"], L["kernel"], L["nin"], L["nout"] = r.u8(), r.u32(), r.u32(), r.u32()
            L["weights"] = r.floats(L["kernel"] * L["nin"] * L["nout"]); L["bias"] = r.floats(L["nout"])
        elif kind == 3:    # gru: activation, reset_after, in, units; three gates
            L["activation"], L["reset_after"], L["nin"], L["n"] = r.u8(), r.u8(), r.u32(), r.u32()
            L["input_weights"] = r.floats(L["nin"] * 3 * L["n"])
            L["recurrent_weights"] = r.floats(L["n"] * 3 * L["n"])
            L["bias"] = r.floats(2 * 3 * L["n"])
        elif kind == 4:    # sparse gru: activation, reset_after, N, nonzeros, index count
            L["activation"], L["reset_after"], L["n"], L["nnz"], L["nidx"] = r.u8(), r.u8(), r.u32(), r.u32(), r.u32()
            L["diag"] = r.floats(3 * L["n"]); L["weights"] = r.floats(L["nnz"])
            L["idx"] = (r.p, L["nidx"], r.raw(4 * L["nidx"])); L["bias"] = r.floats(3 * L["n"])
        elif kind == 5:    # mdense: activation, in, channels, out
            L["activation"], L["nin"], L["channels"], L["nout"] = r.u8(), r.u32(), r.u32(), r.u32()
            L["weights"] = r.floats(L["nin"] * L["channels"] * L["nout"])
            L["bias"] = r.floats(L["channels"] * L["nout"]); L["factor"] = r.floats(L["channels"] * L["nout"])
        layers.append(L)
    return {"header": h, "layers": layers, "end": r.p}


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    plain = load(sys.argv[1])
    secs = sections(plain)
    if "--plain" in sys.argv:
        with open(sys.argv[sys.argv.index("--plain") + 1], "wb") as f:
            f.write(plain)
    if "--lpcnet" in sys.argv:
        kind, off, size = next(s for s in secs if s[0] == 6)
        m = lpcnet(plain[off:off + size])
        print({k: v for k, v in m["header"].items() if k != "table88"})
        for L in m["layers"]:
            dims = {k: v for k, v in L.items() if isinstance(v, int)}
            arrays = {k: v[1] for k, v in L.items() if isinstance(v, tuple)}
            print("%-11s %-20s %s  arrays %s" % (L["kind"], L["name"], dims, arrays))
        print("parsed %d of %d bytes" % (m["end"], size))
    else:
        for kind, off, size in secs:
            print("%2d %-13s offset %9d  size %9d" % (kind, SECTIONS.get(kind, "?"), off, size))


if __name__ == "__main__":
    main()
