"""Translate the whole engine (everything reachable from the TTS_ENGINE_ API) to C.

    python tools/translate_engine.py [--so FILE] [--libm FILE] [--out DIR] [--dry]

--so is the arm64 libsamsungtts.so of voice pack en_us_l03 3.1.25.4 (default work/l03/lib/arm64-v8a/...: the
addresses below are that build's), --libm an arm64 Android libm.so (default work/sys/libm.so), --out where the
generated C and inits.h go (default work/engine). --dry reports the closure without writing anything.
The output is a derivative of Samsung's library: it stays in work/, which git ignores.
"""
import sys, os, re, collections, time
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'a2c'))
import a2c

opt = lambda name, default: sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default
OUT = opt('--out', os.path.join(ROOT, 'work', 'engine'))
main = a2c.Elf(opt('--so', os.path.join(ROOT, 'work', 'l03', 'lib', 'arm64-v8a', 'libsamsungtts.so')), 1, 0)
libm = a2c.Elf(opt('--libm', os.path.join(ROOT, 'work', 'sys', 'libm.so')), 2, 0)

THIRD = re.compile(r'onnx|Ort[A-Z]|ORT_|absl|protobuf|google|re2|flatbuffers|nsync|cpuinfo|Mlas|mlas')

OWN_SKIP = re.compile(r'^Ort|N3Ort|4onnx|11onnxruntime|^cpuinfo_')

def stub(img, addr, name):
    """ONNX Runtime and the libraries that came with it are not on this voice's path."""
    if img.index != 1:
        return False
    if addr < 0x1640000:
        # Samsung's own code: only its wrappers around ONNX Runtime and the cpuinfo library go. (Matching the
        # library names anywhere in a symbol also caught fsa::...FSAMultiStore22Function..., "re2" by accident,
        # which the German front end needs.)
        return bool(name and OWN_SKIP.search(name))
    if name:
        return bool(THIRD.search(name))
    return addr < 0x2108000

HOST = []           # symbols always bound to the host (everything not defined in an image is anyway)
# hot routines with a hand-written host version in front of the translation (engine/fast.c)
WRAPS = ['_Z4gemmPfPKfiiS1_i', '_ZN6lpcnet18compute_activationERNS_2FPES1_ii',
         '_Z28lpctron_is_stopping_callbackPv',     # engine/sstts.c: stopping
         # engine/guards.c: functions that run wild on some input
         '_ZN6CHindi10_SplitWordEPNS_8tLettersEPcj', '_ZN8CItalian28IntervocalicSybilantsPostG2PEv']
OVERRIDE = ['cpuinfo_initialize']     # see a2c_host.c

tr = a2c.Translator({1: main, 2: libm}, host=HOST, overrides=OVERRIDE, stubs=[(1, 0x2186980), (1, 0x2187d34),
                              # two very large functions of the Chinese front end: 72 KB and 491 KB of code, which
                              # take the compiler gigabytes; not needed for English
                              (1, 0x1476e80), (1, 0x149d3b4)], stub_pred=stub,
                    wraps=WRAPS)
roots = [n for n in main.exports if n.startswith('TTS_ENGINE_')]
keys = [(1, main.exports[r]) for r in roots]
# static initialisers that are not ONNX Runtime's
ia = main.sec['.init_array']
inits = []
for slot in range(ia[3], ia[3] + ia[5], 8):
    t = main.relmap.get(slot)
    # 0x161fbf4 only fetches ONNX Runtime's API table (Ort::Global), which is stubbed out
    if t and t != 0x161fbf4 and not stub(main, t, main.byaddr.get(t)):
        inits.append(t)
keys += [(1, t) for t in inits]
t0 = time.time()
tr.run(keys)
print('translated %d functions (%d stubbed) in %.0fs' % (len(tr.done), getattr(tr, 'stubbed', 0), time.time() - t0))
size = sum(main.funcs.get(k[1], 0) if k[0] == 1 else libm.funcs.get(k[1], 0) for k in tr.done)
print('code translated: %d bytes of arm64; C text: %d bytes' % (size, sum(len(v) for v in tr.done.values())))
print('host functions used (%d): %s' % (len(tr.host_used), ' '.join(sorted(tr.host_used))))
h = collections.Counter(); ex = {}
for key, u in tr.problems:
    k = (u[1], u[3]) if isinstance(u, tuple) else ('?', u)
    h[k] += 1
    if isinstance(u, tuple): ex.setdefault(k, '%x %s %s' % (u[0], u[1], u[2]))
print('%d problems; %d functions with unresolved computed jumps' % (len(tr.problems), tr.unresolved_br))
for k, n in h.most_common(40):
    print('  %5d %s   e.g. %s' % (n, k, ex.get(k, '')))
print('init functions:', ' '.join('%x' % t for t in inits))
if '--dry' not in sys.argv:
    os.makedirs(OUT, exist_ok=True)
    names = tr.write_split(OUT, 48)
    # The two files themselves go into the program too: the translated code works on their data (tables, strings,
    # vtables), mapped at run time, and this way that data is always the build the code came from.
    with a2c._KeepIfSame(os.path.join(OUT, 'gen_images.c')) as f:
        f.write('/* The images the code in this directory was translated from, for a2c_load. */\n')
        f.write('__asm__(".section .rdata,\\"dr\\"\\n"\n')
        for n, img in ((1, main), (2, libm)):
            f.write('        ".globl a2c_image%d\\n.p2align 6\\na2c_image%d:\\n.incbin \\"%s\\"\\n.globl a2c_image%d_end\\na2c_image%d_end:\\n"\n'
                    % (n, n, os.path.abspath(img.path).replace('\\', '/'), n, n))
        f.write('        ".text\\n");\n')
    open(os.path.join(OUT, 'inits.h'), 'w').write('static const uint64_t engine_inits[] = { %s };\n' % ', '.join('IMG1 + 0x%xULL' % t for t in inits))
    print('wrote', len(names), 'files')
