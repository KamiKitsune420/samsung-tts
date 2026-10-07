/* Guards in front of engine functions that run wild on some input. The translation is exact, so it also has the
 * original's bugs; on a phone such a bug takes down the TTS service, which Android starts again, but here the
 * engine lives inside the program that is speaking, and a voice that faults is lost with its memory
 * (engine/sstts.c). Each guard puts right what is wrong in the function's input and calls the translation,
 * O_<name>; for every call the original gets through, nothing changes. The functions are listed in
 * tools/translate_engine.py (WRAPS), which is what puts these in front. Found by tests/soak.c --sweep, which
 * names the function and the call stack of a fault. Addresses are those of docs/notes.md. */
#include "a2c_rt.h"

/* unsigned CHindi::_SplitWord(tLetters *out, char *word, unsigned length)                          0x13c4b0c
 *
 * CHindi::G2P cuts a word at its apostrophes and gives the pieces to this one at a time, with the length that is
 * left. For the piece after a second apostrophe it subtracts everything consumed so far from what was left
 * after the first, and the length comes out negative: "O'Neil's" is given as "O" (8), "Neil" (6), "s"
 * (4294967295). This function then reads four thousand million letters, writing six bytes for each into G2P's
 * 1024 entries, and runs off the end of the stack. The word buffer is 1024 bytes, so no real length is more. */
void O__ZN6CHindi10_SplitWordEPNS_8tLettersEPcj(cpu_t *c);
void H__ZN6CHindi10_SplitWordEPNS_8tLettersEPcj(cpu_t *c) {
    if ((uint32_t)c->x[3] > 1023) c->x[3] = (uint64_t)strnlen((const char *)c->x[2], 1023);
    O__ZN6CHindi10_SplitWordEPNS_8tLettersEPcj(c);
}

/* int CItalian::IntervocalicSybilantsPostG2P()                                                     0x13d33e0
 *
 * For each word it writes the names of the word's phonemes, a space between them, into 100 bytes on its stack
 * and never looks at how many there are: a word of some thirty phonemes or more ("AAAA..." sixty-four times)
 * goes over the end, and the stack protector stops the engine.
 *
 * The utterance is at this + 0x32010 and its first word at utterance + 0x2f0; a word has the next word at 0x68,
 * its first phoneme at 0x148 and the phoneme it stops before at 0x150; a phoneme has the next one at 0x68 and
 * its code at 0x88. Names come from the table at [[this + 0x32018] + 0x30]: a count, then (code, offset) pairs
 * in code order, the name at table + 2 + offset, three characters at most being used.
 *
 * So: a word whose names do not fit is made to stop, for the length of this call, before the first phoneme that
 * does not fit. The function may change a phoneme to one of three others before it takes the name (0x4022,
 * 0x4025, 0x8001), so those count with the longest of the names it could end up with. */
void O__ZN8CItalian28IntervocalicSybilantsPostG2PEv(cpu_t *c);

static int phoneme_name_length(uint64_t table, uint32_t code) {
    if (!table) return 3;
    uint32_t lo = 0, hi = ld16(table);
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2, at = ld16(table + 2 + 4 * (uint64_t)mid);
        if (at < code) lo = mid + 1;
        else if (at > code) hi = mid;
        else {
            uint32_t offset = ld16(table + 4 + 4 * (uint64_t)mid);
            if (!offset) return 3;
            int n = (int)strnlen((const char *)(table + 2 + offset), 3);
            return n ? n : 1;       /* the function writes the first character whatever it is */
        }
    }
    return 3;
}

void H__ZN8CItalian28IntervocalicSybilantsPostG2PEv(cpu_t *c) {
    enum { ROOM = 99, MAX_CUT = 256 };      /* 100 bytes, one of them for the end of the string */
    uint64_t self = c->x[0], cut_word[MAX_CUT], cut_end[MAX_CUT];
    int ncut = 0;
    uint64_t utterance = ld64(self + 0x32010), data = ld64(self + 0x32018);
    uint64_t table = data ? ld64(data + 0x30) : 0;
    int changed = phoneme_name_length(table, 0x4022), n;
    if ((n = phoneme_name_length(table, 0x4025)) > changed) changed = n;
    if ((n = phoneme_name_length(table, 0x8001)) > changed) changed = n;
    for (uint64_t word = utterance ? ld64(utterance + 0x2f0) : 0; word; word = ld64(word + 0x68)) {
        uint64_t end = ld64(word + 0x150);
        int used = 0;
        for (uint64_t p = ld64(word + 0x148); p && p != end; p = ld64(p + 0x68)) {
            uint32_t code = ld16(p + 0x88);
            int len = phoneme_name_length(table, code);
            if ((code == 0x4022 || code == 0x4025 || code == 0x2001) && changed > len) len = changed;
            used += len + (used > 0);
            if (used > ROOM) {
                if (ncut == MAX_CUT) {      /* more over-long words than can be put back afterwards: leave the sibilants alone */
                    for (int i = 0; i < ncut; i++) st64(cut_word[i] + 0x150, cut_end[i]);
                    c->x[0] = 0;
                    return;
                }
                cut_word[ncut] = word; cut_end[ncut++] = end;
                st64(word + 0x150, p);
                break;
            }
        }
    }
    O__ZN8CItalian28IntervocalicSybilantsPostG2PEv(c);
    for (int i = 0; i < ncut; i++) st64(cut_word[i] + 0x150, cut_end[i]);
}
