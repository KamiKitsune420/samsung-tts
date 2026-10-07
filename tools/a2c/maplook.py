"""Name the host code in a profile: resolve "host code at image offset N" lines through the linker map.

    engine.exe ... --profile 2>&1 | python tools/a2c/maplook.py work/engine/engine.map
"""
import bisect, re, sys

syms = []
for l in open(sys.argv[1], errors='replace'):
    m = re.match(r'\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})', l)
    if m:
        syms.append((int(m.group(2), 16) - 0x140000000, m.group(1)))
syms.sort()
for line in sys.stdin:
    m = re.search(r'host code at image offset ([0-9a-f]+)', line)
    if m and syms:
        a = int(m.group(1), 16)
        i = bisect.bisect_right(syms, (a, '~')) - 1
        line = line.rstrip() + '  = %s+0x%x\n' % (syms[i][1], a - syms[i][0])
    sys.stdout.write(line)
