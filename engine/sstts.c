/* sstts.h over the translated engine: the calls tools/oracle/oracle.c makes on the device, in the same order. */
#include "a2c_rt.h"
#include "sstts.h"
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <windows.h>
#include "inits.h"

int a2c_load(int index, const uint8_t *d, size_t n, uint64_t base);
cpu_t *a2c_cpu_new(void);
void (*a2c_find(const char *name))(cpu_t *);
uint64_t a2c_callback(void (*fn)(cpu_t *));
void a2c_where(uint64_t host_address, char *out, size_t cap);
void a2c_backtrace(const void *context, char *out, size_t cap);
int a2c_heap_ok(void);
extern int a2c_trap_raises;
extern char a2c_last_trap[200];

/* A loaded voice is one engine object. Each holds its models in memory (some 100 MB), so only a few are kept:
 * the least recently used one is dropped to make room. */
#define MAX_VOICES 3
typedef struct { uint64_t engine; int rate, token_limit; char key[600]; unsigned long used; } voice_t;
static voice_t g_voices[MAX_VOICES];
static voice_t *g_cur;
static unsigned long g_clock;

static cpu_t *C;
static uint64_t g_callback;
static sstts_audio_fn g_fn;
static void *g_user;
static volatile long g_stop;
static int g_stop_sent;
static char g_fault[1000];

/* void cb(const char *pcm, int bytes, int word_offset, int word_length, const char *text, int result, void *user):
 * audio arrives with result 0; 8 opens an utterance, 2 and 1 close it */
static int g_cuts[256], g_ncuts;        /* where sstts_speak put a space into the text, as offsets in what the engine got */
static void on_audio(cpu_t *c) {
    const short *pcm = (const short *)c->x[0];
    int bytes = (int)c->x[1], off = (int)c->x[2], len = (int)c->x[3];
    int has_text = c->x[4] != 0;
    if (g_ncuts && has_text) {      /* a word's place in the caller's text, not in the one with spaces added */
        int end = off + len, before = 0, within = 0;
        for (int i = 0; i < g_ncuts; i++) { before += g_cuts[i] < off; within += g_cuts[i] >= off && g_cuts[i] < end; }
        off -= before; len -= within;
    }
    if (pcm && bytes > 0 && g_fn && !g_stop) g_fn(g_user, pcm, bytes / 2, has_text ? off : -1, has_text ? len : -1);
    if (g_stop && !g_stop_sent && g_cur) {
        /* the engine's own stop; calling into it from its callback is an ordinary nested call */
        g_stop_sent = 1;
        void (*stop)(cpu_t *) = a2c_find("TTS_ENGINE_Stop");
        c->x[0] = g_cur->engine;
        stop(c);
    }
}

/* lpctron_is_stopping_callback(void *): the vocoder asks between frames whether to give up */
void O__Z28lpctron_is_stopping_callbackPv(cpu_t *c);
void H__Z28lpctron_is_stopping_callbackPv(cpu_t *c) {
    if (g_stop) { c->x[0] = 1; return; }
    O__Z28lpctron_is_stopping_callbackPv(c);
}

int sstts_init(const char **error) {
    static const char *none = "";
    if (!error) error = &none;
    if (C) return 0;
    if (a2c_load(2, a2c_image2, (size_t)(a2c_image2_end - a2c_image2), IMG2)) { *error = "cannot map the maths library"; return -1; }
    if (a2c_load(1, a2c_image1, (size_t)(a2c_image1_end - a2c_image1), IMG1)) { *error = "cannot map the engine's data"; return -1; }
    C = a2c_cpu_new();
    for (size_t i = 0; i < sizeof engine_inits / sizeof engine_inits[0]; i++) a2c_call(C, engine_inits[i]);
    g_callback = a2c_callback(on_audio);
    return 0;
}

/* ---- calls into the engine
 *
 * A fault inside the engine (a trap in the translated code, an access violation) must cost one utterance and not
 * the process, which may be a screen reader. So every call is made with the fault caught. After a fault the voice
 * is dropped and loads afresh at the next sstts_select; sstts_last_fault says what happened. */
const char *sstts_last_fault(void) { return g_fault; }

/* The exception filter: note where it happened while the faulting frames still exist. */
static uint64_t g_fault_at, g_fault_access; static int g_fault_write;
static char g_fault_stack[600];
static int note_fault(EXCEPTION_POINTERS *x) {
    const EXCEPTION_RECORD *r = x->ExceptionRecord;
    g_fault_at = (uint64_t)r->ExceptionAddress;
    g_fault_access = r->NumberParameters >= 2 ? (uint64_t)r->ExceptionInformation[1] : 0;
    g_fault_write = r->NumberParameters >= 1 && r->ExceptionInformation[0] == 1;
    a2c_backtrace(x->ContextRecord, g_fault_stack, sizeof g_fault_stack);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void describe_fault(unsigned code, const char *name) {
    if (code == 0xE0A2C001u) snprintf(g_fault, sizeof g_fault, "%s", a2c_last_trap);
    else {
        char where[120];
        a2c_where(g_fault_at, where, sizeof where);
        if (code == EXCEPTION_ACCESS_VIOLATION)
            snprintf(g_fault, sizeof g_fault, "access violation %s %llx in %s", g_fault_write ? "writing" : "reading",
                     (unsigned long long)g_fault_access, where);
        else if (code == EXCEPTION_STACK_OVERFLOW) snprintf(g_fault, sizeof g_fault, "stack overflow (recursion too deep) in %s", where);
        else snprintf(g_fault, sizeof g_fault, "exception %08x in %s", code, where);
    }
    size_t n = strlen(g_fault);
    snprintf(g_fault + n, sizeof g_fault - n, "; in %s, called from %s", name, g_fault_stack);
}

/* Call an engine function for voice v. 1 with its result in *result, or 0 if it faulted. */
static int enter(voice_t *v, uint64_t *result, const char *name, int nargs, const uint64_t *args) {
    void (*f)(cpu_t *) = a2c_find(name);
    if (!f) { snprintf(g_fault, sizeof g_fault, "no translated function %s", name); return 0; }
    uint64_t sp = C->sp;
    int ok = 1;
    unsigned code = 0;
    (void)v;
    a2c_trap_raises = 1;
    /* The translated code recurses on this thread's own stack. Should it run out, the fault has to be dealt
     * with on what is left: keep enough in hand for that. */
    static DWORD guaranteed_on;
    if (guaranteed_on != GetCurrentThreadId()) { ULONG keep = 128 * 1024; SetThreadStackGuarantee(&keep); guaranteed_on = GetCurrentThreadId(); }
    __try {
        for (int i = 0; i < nargs; i++) C->x[i] = args[i];
        f(C);
        if (result) *result = C->x[0];
    } __except (note_fault(GetExceptionInformation())) {
        code = GetExceptionCode();
        describe_fault(code, name);
        C->sp = sp;             /* the guest stack as it was before the call */
        ok = 0;
    }
    if (code == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();      /* put the stack's guard page back */
    return ok;
}
#define ENTER(v, result, name, ...) enter(v, result, name, (int)(sizeof((uint64_t[]){__VA_ARGS__}) / 8), (uint64_t[]){__VA_ARGS__})

/* Forget a voice. Torn down, the engine gives back what it holds for it (100 to 300 MB).
 * After a fault the engine object was left in the middle of something. It is torn down all the same: abandoned
 * as it is, its memory would be lost until the process ends, and a text that faults tends to come again (with
 * the faults the stress tests provoked, four of them cost 1.4 GB abandoned and nothing torn down, see
 * docs/notes.md). The teardown runs the engine's destructors over that state, so it is made only if the guest's
 * heap is intact, and if it faults in turn the voice is abandoned after all; sstts_abandoned counts those.
 * SSTTS_ON_FAULT=abandon in the environment never tears down a faulted voice. */
enum { TORN_DOWN, NEVER_INITIALIZED, FAULTED };
static long g_abandoned;
long sstts_abandoned(void) { return g_abandoned; }
static void drop(voice_t *v, int how) {
    if (v->engine) {
        static int abandon_after_fault = -1;
        if (abandon_after_fault < 0) {
            const char *e = getenv("SSTTS_ON_FAULT");
            abandon_after_fault = e && !strcmp(e, "abandon");
        }
        char keep[sizeof g_fault];      /* a fault while tearing down is not the caller's news */
        memcpy(keep, g_fault, sizeof keep);
        int tear = how != FAULTED || (!abandon_after_fault && a2c_heap_ok());
        if (tear && how != NEVER_INITIALIZED) tear = ENTER(v, NULL, "TTS_ENGINE_Finalize", v->engine);
        if (tear) tear = ENTER(v, NULL, "TTS_ENGINE_Delete", v->engine);
        if (!tear) g_abandoned++;
        memcpy(g_fault, keep, sizeof keep);
    }
    if (g_cur == v) g_cur = NULL;
    memset(v, 0, sizeof *v);
}

static int token_limit(const char *lang);

int sstts_select(const char *voice_root, const char *voice, const char **error) {
    static const char *none = "";
    if (!error) error = &none;
    if (!C) { *error = "sstts_init has not been called"; return -1; }
    char key[600];
    snprintf(key, sizeof key, "%s|%s", voice_root, voice);
    voice_t *slot = NULL;
    for (int i = 0; i < MAX_VOICES; i++)
        if (g_voices[i].engine && !strcmp(g_voices[i].key, key)) { g_cur = &g_voices[i]; g_cur->used = ++g_clock; return 0; }
    char lang[8] = "", country[8] = "", type[8] = ""; int num = 0;
    if (sscanf(voice, "%7[^_]_%7[^_]_%1[a-z]%d", lang, country, type, &num) != 4) { *error = "voice name is not <ll>_<CC>_<type><nn>"; return -1; }
    for (int i = 0; i < MAX_VOICES; i++)
        if (!g_voices[i].engine) { slot = &g_voices[i]; break; }
    if (!slot) {        /* drop the one used longest ago */
        slot = &g_voices[0];
        for (int i = 1; i < MAX_VOICES; i++) if (g_voices[i].used < slot->used) slot = &g_voices[i];
        drop(slot, TORN_DOWN);
    }
    uint64_t e = 0, r = 0, hz = 0;
    if (!ENTER(slot, &e, "TTS_ENGINE_New")) { *error = g_fault; return -2; }
    if (!e) { *error = "the engine could not be created"; return -1; }
    slot->engine = e;
    /* The voice comes first: Initialize builds the synthesizer the voice's cfg asks for.
     * Mode 3 looks for the folder layout whatever the pack's type letter; mode 0 does so only for the l and g
     * packs and wants smt_<voice>.cfg files for the f and m ones. The reference voice gives the same samples
     * in both. SSTTS_MODE overrides it, for finding out what a new kind of pack needs. */
    int mode = getenv("SSTTS_MODE") ? atoi(getenv("SSTTS_MODE")) : 3;
    if (!ENTER(slot, &r, "TTS_ENGINE_SetVoice", e, (uint64_t)mode, (uint64_t)voice_root, 0, (uint64_t)lang, (uint64_t)country, (uint64_t)type, (uint64_t)num)) goto faulted;
    if ((int)r) { drop(slot, NEVER_INITIALIZED); *error = "SetVoice failed: voice files not found under the voice root"; return (int)r; }
    if (!ENTER(slot, &r, "TTS_ENGINE_Initialize", e)) goto faulted;
    if ((int)r) { drop(slot, NEVER_INITIALIZED); *error = "Initialize failed"; return (int)r; }
    if (!ENTER(slot, NULL, "TTS_ENGINE_SetAudioCallback", e, g_callback, 0)) goto faulted;
    if (!ENTER(slot, &hz, "TTS_ENGINE_GetSamplingRate", e)) goto faulted;
    slot->rate = (int)hz;
    slot->token_limit = token_limit(lang);
    snprintf(slot->key, sizeof slot->key, "%s", key);
    slot->used = ++g_clock;
    g_cur = slot;
    return 0;
faulted:
    drop(slot, FAULTED);
    *error = g_fault;
    return -2;
}

int sstts_rate(void) { return g_cur ? g_cur->rate : 0; }

int sstts_version(void) {
    uint64_t v = 0;
    if (g_cur && !ENTER(g_cur, &v, "TTS_ENGINE_GetVersion", g_cur->engine)) drop(g_cur, FAULTED);
    return (int)v;
}

void sstts_control(int pitch, int volume, int rate) {
    if (g_cur && !ENTER(g_cur, NULL, "TTS_ENGINE_VoiceControl", g_cur->engine, (uint64_t)pitch, (uint64_t)volume, (uint64_t)rate)) drop(g_cur, FAULTED);
}

void sstts_ending_silence(int ms) {
    if (g_cur && !ENTER(g_cur, NULL, "TTS_ENGINE_SetEndingSilence", g_cur->engine, (uint64_t)ms)) drop(g_cur, FAULTED);
}

/* ---- words too long for the engine
 *
 * The language front ends keep a word, and what they expand it to, in buffers of fixed size on the stack, and
 * several never look at its length. Chinese and Thai spell out a word in Latin letters, and a run of sixty-four
 * "w" is already more than their buffer holds; other languages go wrong much later or not at all
 * (tests/token_limits.py --raw is the survey, docs/notes.md has its results). So a run of ASCII letters, digits
 * and signs that is longer than the language can take is given to the engine in pieces, a space between them.
 * Nothing that is a word in any language is that long, and text in other scripts is never touched.
 * SSTTS_TOKEN_LIMIT in the environment sets the limit for every language; 0 turns this off. */
#define TOKEN_LIMITS { "zh", 32 }, { "th", 32 }
#define TOKEN_LIMIT_OTHERS 1000
static int token_limit(const char *lang) {
    const char *e = getenv("SSTTS_TOKEN_LIMIT");
    if (e) return atoi(e);
    static const struct { const char *lang; int limit; } table[] = { TOKEN_LIMITS };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (!strcmp(table[i].lang, lang)) return table[i].limit;
    return TOKEN_LIMIT_OTHERS;
}

/* The text with over-long runs cut (a new block, for the caller to free), or NULL if it has none. */
static char *cut_long_tokens(const char *text, int limit) {
    g_ncuts = 0;
    if (limit <= 0) return NULL;
    int run = 0, need = 0;
    size_t n = 0;
    for (; text[n]; n++) {
        run = (text[n] > ' ' && text[n] < 127) ? run + 1 : 0;
        if (run > limit) { need = 1; run = 1; }
    }
    if (!need) return NULL;
    char *out = malloc(n + n / (size_t)limit + 2);
    if (!out) return NULL;
    size_t o = 0;
    run = 0;
    for (size_t i = 0; i < n; i++) {
        run = (text[i] > ' ' && text[i] < 127) ? run + 1 : 0;
        if (run > limit) {
            if (g_ncuts < (int)(sizeof g_cuts / sizeof g_cuts[0])) g_cuts[g_ncuts++] = (int)o;
            out[o++] = ' ';
            run = 1;
        }
        out[o++] = text[i];
    }
    out[o] = 0;
    return out;
}

int sstts_speak(const char *utf8, sstts_audio_fn fn, void *user) {
    if (!g_cur) return -1;
    char *cut = cut_long_tokens(utf8, g_cur->token_limit);
    g_fn = fn; g_user = user; g_stop = 0; g_stop_sent = 0;
    uint64_t r = 0;
    int ok = ENTER(g_cur, &r, "TTS_ENGINE_InputText", g_cur->engine, (uint64_t)(cut ? cut : utf8));
    g_fn = NULL;
    g_ncuts = 0;
    free(cut);
    if (!ok) { drop(g_cur, FAULTED); return -2; }
    return (int)r;
}

void sstts_stop(void) { g_stop = 1; }

void sstts_prefer_fast_cores(void) {
    /* Tell Windows this thread is latency-sensitive work: on a hybrid CPU it then prefers the performance
     * cores, where the engine is several times faster than on the efficiency cores. */
    THREAD_POWER_THROTTLING_STATE st;
    memset(&st, 0, sizeof st);
    st.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    st.StateMask = 0;
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &st, sizeof st);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
}

void sstts_fast_denormals(int on) {
    unsigned csr = _mm_getcsr();
    _mm_setcsr(on ? (csr | 0x8040u) : (csr & ~0x8040u));
}
