/* For tests/soak.c built as soak_guards_off.exe: the functions engine/guards.c guards, without the guards, so that
 * the engine faults the way it did (the Hindi voice on a word with two apostrophes, an Italian voice on a word
 * of thirty phonemes or more). What is under test is the fault handling in engine/sstts.c: the utterance is lost,
 * the voice is torn down and loads again, memory stays where it was, the other voices do not notice.
 * tests/corpus/faults.txt has lines that fault. */
#include "a2c_rt.h"
void O__ZN6CHindi10_SplitWordEPNS_8tLettersEPcj(cpu_t *c);
void H__ZN6CHindi10_SplitWordEPNS_8tLettersEPcj(cpu_t *c) { O__ZN6CHindi10_SplitWordEPNS_8tLettersEPcj(c); }
void O__ZN8CItalian28IntervocalicSybilantsPostG2PEv(cpu_t *c);
void H__ZN8CItalian28IntervocalicSybilantsPostG2PEv(cpu_t *c) { O__ZN8CItalian28IntervocalicSybilantsPostG2PEv(c); }
