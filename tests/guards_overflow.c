/* For tests/soak.c built as soak_overflow.exe: the Italian post-G2P function replaced by a recursion that never
 * ends, so that every utterance of an Italian voice runs the host stack out. What is under test: that costs the
 * utterance and not the process (engine/sstts.c keeps stack in hand and puts the guard page back). */
#include "a2c_rt.h"
void O__ZN6CHindi10_SplitWordEPNS_8tLettersEPcj(cpu_t *c);
void H__ZN6CHindi10_SplitWordEPNS_8tLettersEPcj(cpu_t *c) { O__ZN6CHindi10_SplitWordEPNS_8tLettersEPcj(c); }
static int deep(volatile int n) {
    volatile char pad[2048];
    pad[0] = (char)n;
    return deep(n + 1) + pad[0];
}
void H__ZN8CItalian28IntervocalicSybilantsPostG2PEv(cpu_t *c) { c->x[0] = (uint64_t)deep(0); }
