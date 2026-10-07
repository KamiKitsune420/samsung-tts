/* Samsung's own TTS engine as a command-line program, on an arm64 Android device or emulator.
 *
 *   oracle <libsamsungtts.so> <voice root> <corpus.txt> <outdir> [--lang en --country US --type l --num 3 --mode N]
 *
 * One utterance a line in the corpus. Writes NNNNN.pcm (16-bit mono) per line and index.tsv.
 * The engine library and the voice are the user's own; nothing of Samsung's is built into this.
 * TRACE=1 in the environment logs every file the engine opens.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static int g_trace;

/* The engine resolves libc through the global scope, where this executable comes first. */
FILE *fopen(const char *path, const char *mode) {
    static FILE *(*real)(const char *, const char *);
    if (!real) real = dlsym(RTLD_NEXT, "fopen");
    FILE *f = real(path, mode);
    if (g_trace) fprintf(stderr, "fopen %s %s -> %s\n", path, mode, f ? "ok" : "FAIL");
    return f;
}
int open(const char *path, int flags, ...) {
    static int (*real)(const char *, int, ...);
    va_list ap; va_start(ap, flags); int m = va_arg(ap, int); va_end(ap);
    if (!real) real = dlsym(RTLD_NEXT, "open");
    int r = real(path, flags, m);
    if (g_trace) fprintf(stderr, "open %s -> %d\n", path, r);
    return r;
}
int __open_2(const char *path, int flags) {
    static int (*real)(const char *, int, ...);
    if (!real) real = dlsym(RTLD_NEXT, "open");
    int r = real(path, flags);
    if (g_trace) fprintf(stderr, "open2 %s -> %d\n", path, r);
    return r;
}

/* The engine calls its own exported functions through its import table, so the same trick reaches inside it:
 * a function defined here under the engine's mangled name is called in its place, and forwards to the real one. */
static void *g_lib;
static FILE *g_voc;          /* VOCODER=<file>: every lpcnet_synthesize call, as nfeat, nsamp, features, samples */
static int g_voc_calls;

#define REAL(name) ({ static void *p; if (!p) p = dlsym(g_lib, name); p; })

void _ZN14TacotronLPCNet11set_speakerEi(void *self, int spk) {
    fprintf(stderr, "engine: TacotronLPCNet, speaker %d\n", spk);
    ((void (*)(void *, int))REAL("_ZN14TacotronLPCNet11set_speakerEi"))(self, spk);
}
/* ivc_stream::read(char*, long). IVC=<file> logs every read with the stream's fields, and each key once:
 * the object is {+0 memory base, +8 size, +0x10 key, +0x18 key length, +0x1c position, +0x20 key phase,
 * +0x24 file offset, +0x30 istream*, +0x38 encrypted}. */
static FILE *g_ivc;
long _ZN10ivc_stream4readEPcl(void *self, char *dst, long n) {
    unsigned char *s = self;
    void *base = *(void **)s, *key = *(void **)(s + 0x10), *is = *(void **)(s + 0x30);
    unsigned long size = *(unsigned long *)(s + 8);
    unsigned klen = *(unsigned *)(s + 0x18);
    int pos = *(int *)(s + 0x1c), phase = *(int *)(s + 0x20), foff = *(int *)(s + 0x24), enc = s[0x38];
    long r = ((long (*)(void *, char *, long))REAL("_ZN10ivc_stream4readEPcl"))(self, dst, n);
    if (g_ivc) {
        static void *seen[64]; static int nseen;
        int known = 0;
        for (int i = 0; i < nseen; i++) if (seen[i] == key) known = 1;
        if (key && !known && nseen < 64) {
            seen[nseen++] = key;
            fprintf(g_ivc, "KEY %p len %u:", key, klen);
            for (unsigned i = 0; i < klen && i < 4096; i++) fprintf(g_ivc, " %02x", ((unsigned char *)key)[i]);
            fprintf(g_ivc, "\n");
        }
        fprintf(g_ivc, "R %p base=%p is=%p size=%lu pos=%d phase=%d foff=%d enc=%d n=%ld ->%ld :", self, base, is, size,
                pos, phase, foff, enc, n, r);
        for (long i = 0; i < r && i < 16; i++) fprintf(g_ivc, " %02x", (unsigned char)dst[i]);
        fprintf(g_ivc, "\n");
    }
    return r;
}

void _ZN14TacotronLPCNetC2EP8Tacotron(void *self, void *taco) {
    fprintf(stderr, "engine: TacotronLPCNet\n");
    ((void (*)(void *, void *))REAL("_ZN14TacotronLPCNetC2EP8Tacotron"))(self, taco);
}
void _ZN16RVQVAESupervisorC1EP6RVQVAE(void *self, void *m) {
    fprintf(stderr, "engine: RVQVAESupervisor\n");
    ((void (*)(void *, void *))REAL("_ZN16RVQVAESupervisorC1EP6RVQVAE"))(self, m);
}
void _ZN16RVQVAESupervisor11set_speakerEi(void *self, int spk) {
    fprintf(stderr, "engine: RVQVAESupervisor, speaker %d\n", spk);
    ((void (*)(void *, int))REAL("_ZN16RVQVAESupervisor11set_speakerEi"))(self, spk);
}
/* COUNT=1: how often each vocoder stage runs. The generic forwarder passes six integer and four floating-point
 * argument registers through untouched, which covers every function counted here (none returns a float). */
static int g_count;
struct counter { const char *name; long n; };
static struct counter g_counters[32]; static int g_ncounters;
static long *counter(const char *name) {
    g_counters[g_ncounters].name = name;
    return &g_counters[g_ncounters++].n;
}
#define COUNTED(sym) \
    long sym(long a, long b, long c, long d, long e, long f, double v0, double v1, double v2, double v3) { \
        static long *n; if (!n) n = counter(#sym); (*n)++; \
        return ((long (*)(long, long, long, long, long, long, double, double, double, double))REAL(#sym))( \
            a, b, c, d, e, f, v0, v1, v2, v3); }
COUNTED(_ZN6lpcnet19run_FD1_LQ3_networkEPNS_11LPCNetModelEPNS_9NNetStateERNS_2FPES5_)
COUNTED(_ZN6lpcnet15sample_from_molEPNS_9NNetStateEPsfi)
COUNTED(__cxa_throw)
COUNTED(pthread_create)
COUNTED(__cxa_begin_catch)
COUNTED(_ZN6lpcnet18compute_sparse_gruEPNS_14SparseGRULayerEPNS_9NNetStateEi)
COUNTED(_ZN6lpcnet12compute_gru2EPNS_8GRULayerEPNS_9NNetStateERNS_2FPEi)
COUNTED(_ZN6lpcnet14compute_mdenseEPNS_11MDenseLayerERNS_2FPES3_S3_)
COUNTED(_ZN6lpcnet18compute_activationERNS_2FPES1_ii)
COUNTED(_ZN6lpcnet13compute_denseEPNS_10DenseLayerERNS_2FPES3_)
COUNTED(_ZN6lpcnet14compute_conv1dEPNS_11Conv1DLayerERNS_2FPES3_S3_S3_)
COUNTED(_ZN6lpcnet14compute_conv1dEPNS_11Conv1DLayerERNS_2FPES3_S3_)
COUNTED(_ZN6lpcnet17compute_embeddingEPNS_14EmbeddingLayerERNS_2FPEi)
COUNTED(_ZN6lpcnet15accum_embeddingEPNS_14EmbeddingLayerERNS_2FPEi)
COUNTED(_ZN6lpcnet15compute_highwayEPNS_11LPCNetModelEPNS_9NNetStateERNS_2FPEi)
COUNTED(_ZN6lpcnet17lpc_from_cepstrumEPfPKfPNS_11CommonStateE)
COUNTED(_ZN6lpcnet14lpc_from_bandsEPfPKfPNS_11CommonStateE)
COUNTED(_ZN6lpcnet22convert_feat_dual_rateEPfPNS_11CommonStateE)
COUNTED(_ZN6lpcnet9declickerEPfiS0_S0_S0_)
COUNTED(_ZN6lpcnet15hf_tone_removerEPfS0_PNS_8HTRStateEi)

/* TRACE=<file> [TRACE_N=<samples>]: the vocoder's whole state around each stage, for replaying against a port.
 * A record is a 4-byte tag, a u32 payload length, and the payload. A snapshot is the LPCNetState (0x1a60 bytes) and
 * the NNetState (0x6b0 bytes) raw, then every lpcnet::FP in the NNetState as {u32 slot offset, u32 count, floats};
 * an FP is {data, bytes per element, count, is_view}, 0x20 bytes. */
static FILE *g_tr; static long g_tr_n = 480, g_tr_samples;
static unsigned char *g_rec; static size_t g_rec_len, g_rec_cap;
static void rec_put(const void *p, size_t n) {
    if (g_rec_len + n > g_rec_cap) { g_rec_cap = (g_rec_len + n) * 2; g_rec = realloc(g_rec, g_rec_cap); }
    memcpy(g_rec + g_rec_len, p, n); g_rec_len += n;
}
static void rec_end(const char *tag) {
    unsigned n = (unsigned)g_rec_len;
    fwrite(tag, 1, 4, g_tr); fwrite(&n, 4, 1, g_tr); fwrite(g_rec, 1, g_rec_len, g_tr); g_rec_len = 0;
}
static void rec_snapshot(void *lst) {
    unsigned char *nn = *(unsigned char **)((char *)lst + 8);
    rec_put(lst, 0x1a60); rec_put(nn, 0x6b0);
    for (unsigned o = 0; o + 0x20 <= 0x6b0; o += 0x20) {
        void *data = *(void **)(nn + o); long prec = *(long *)(nn + o + 8), cnt = *(long *)(nn + o + 16);
        if (!data || prec != 4 || cnt <= 0 || cnt > (1 << 20) || nn[o + 24] > 1) continue;
        unsigned c = (unsigned)cnt; rec_put(&o, 4); rec_put(&c, 4); rec_put(data, 4 * (size_t)cnt);
    }
    unsigned end = 0xffffffff; rec_put(&end, 4);
}
void _ZN6lpcnet17run_frame_networkEPNS_11LPCNetStateERNS_2FPEi(void *st, void *fp, int pitch) {
    void (*real)(void *, void *, int) = REAL("_ZN6lpcnet17run_frame_networkEPNS_11LPCNetStateERNS_2FPEi");
    if (!g_tr) { real(st, fp, pitch); return; }
    unsigned cnt = (unsigned)*(long *)((char *)fp + 16);
    rec_put(&pitch, 4); rec_put(&cnt, 4); rec_put(*(void **)fp, 4 * (size_t)cnt); rec_snapshot(st); rec_end("FRM0");
    real(st, fp, pitch);
    rec_snapshot(st); rec_end("FRM1");
}
void _ZN6lpcnet18run_sample_networkEPNS_11LPCNetStateEPiS2_S2_(void *st, int *a, int *b, int *c) {
    void (*real)(void *, int *, int *, int *) = REAL("_ZN6lpcnet18run_sample_networkEPNS_11LPCNetStateEPiS2_S2_");
    int on = g_tr && g_tr_samples < g_tr_n;
    if (on) { rec_put(a, 4); rec_put(b, 4); rec_put(c, 4); rec_snapshot(st); rec_end("SMP0"); }
    real(st, a, b, c);
    if (on) { rec_put(a, 4); rec_put(b, 4); rec_put(c, 4); rec_snapshot(st); rec_end("SMP1"); g_tr_samples++; }
}
unsigned long _ZN6lpcnet15sample_from_pdfEPNS_9NNetStateEiffPs(void *nn, int n, float f0, float f1, short *out) {
    unsigned long (*real)(void *, int, float, float, short *) = REAL("_ZN6lpcnet15sample_from_pdfEPNS_9NNetStateEiffPs");
    int on = g_tr && g_tr_samples < g_tr_n;
    if (on) {
        unsigned char *p = nn; unsigned cnt = (unsigned)*(long *)(p + 0x1b0);
        rec_put(&n, 4); rec_put(&f0, 4); rec_put(&f1, 4); rec_put(&cnt, 4); rec_put(*(void **)(p + 0x1a0), 4 * (size_t)cnt);
        rec_put(nn, 0x6b0); rec_end("PDF0");
    }
    unsigned long r = real(nn, n, f0, f1, out);
    if (on) { short o = out ? *out : 0; rec_put(&r, 8); rec_put(&o, 2); rec_put(nn, 0x6b0); rec_end("PDF1"); }
    return r;
}

/* SETUP=1: the calls that configure the vocoder, with their arguments, in order. */
static int g_setup;
#define LOGGED(sym, fmt) \
    long sym(long a, long b, long c, long d, long e, long f, double v0, double v1, double v2, double v3) { \
        float fv; memcpy(&fv, &v0, 4); \
        long r = ((long (*)(long, long, long, long, long, long, double, double, double, double))REAL(#sym))( \
            a, b, c, d, e, f, v0, v1, v2, v3); \
        if (g_setup) fprintf(stderr, "setup %s " fmt " -> %lx\n", #sym, a, b, (double)fv, r); \
        return r; }
LOGGED(_ZN6lpcnet13lpcnet_createEPNS_11LPCNetModelE, "(%lx, %lx, %g)")
LOGGED(_ZN6lpcnet11lpcnet_initEPNS_11LPCNetStateE, "(%lx, %lx, %g)")
LOGGED(_ZN6lpcnet17set_lpcnet_spk_idEPNS_11LPCNetStateEi, "(%lx, %ld, %g)")
LOGGED(_ZN6lpcnet15set_lpcnet_rateEPNS_11LPCNetStateEf, "(%lx, %lx, %.9g)")
LOGGED(_ZN6lpcnet17set_lpcnet_volumeEPNS_11LPCNetStateEf, "(%lx, %lx, %.9g)")
LOGGED(_ZN6lpcnet21set_lpcnet_pitch_rateEPNS_11LPCNetStateEf, "(%lx, %lx, %.9g)")
LOGGED(_ZN6lpcnet25set_lpcnet_pitch_semitoneEPNS_11LPCNetStateEf, "(%lx, %lx, %.9g)")
LOGGED(_ZN6lpcnet26set_precision_lpcnet_modelEPNS_11LPCNetModelEm, "(%lx, %ld, %g)")
LOGGED(_ZN6lpcnet14lpcnet_destroyEPNS_11LPCNetStateE, "(%lx, %lx, %g)")
LOGGED(_ZN6lpcnet8htr_initEPNS_8HTRStateEiibifi, "(%lx, %ld, %.9g)")
long _ZN6lpcnet17lpcnet_model_loadER10ivc_streamPKc(void *stream, const char *name) {
    long r = ((long (*)(void *, const char *))REAL("_ZN6lpcnet17lpcnet_model_loadER10ivc_streamPKc"))(stream, name);
    if (g_setup) fprintf(stderr, "setup lpcnet_model_load(stream, \"%s\") -> %lx\n", name ? name : "(null)", r);
    return r;
}

/* lpcnet::lpcnet_synthesize(LPCNetState*, float* features, short* pcm) */
long _ZN6lpcnet17lpcnet_synthesizeEPNS_11LPCNetStateEPfPs(void *st, float *feat, short *pcm) {
    int nfeat = ((int (*)(void *))REAL("_ZN6lpcnet25get_lpcnet_nb_features_inEPNS_11LPCNetStateE"))(st);
    int nsamp = ((int (*)(void *))REAL("_ZN6lpcnet21get_lpcnet_frame_sizeEPNS_11LPCNetStateE"))(st);
    float in[256];
    if (g_tr) { rec_put(&nfeat, 4); rec_put(&nsamp, 4); rec_put(feat, 4 * (size_t)nfeat); rec_snapshot(st); rec_end("SYN0"); }
    if (g_voc && nfeat > 0 && nfeat <= 256) memcpy(in, feat, nfeat * sizeof(float));
    long r = ((long (*)(void *, float *, short *))REAL("_ZN6lpcnet17lpcnet_synthesizeEPNS_11LPCNetStateEPfPs"))(st, feat, pcm);
    if (g_tr) {
        int has = pcm != NULL;
        rec_put(&has, 4); rec_put(feat, 4 * (size_t)nfeat); if (pcm) rec_put(pcm, 2 * (size_t)nsamp);
        rec_snapshot(st); rec_end("SYN1");
    }
    if (g_voc_calls++ < 3) {
        long *model = *(long **)st;
        fprintf(stderr, "lpcnet_synthesize: %d features in, frame %d, pcm %p, returns %ld; precision %d, model type %d\n",
                nfeat, nsamp, (void *)pcm, r, *(int *)((char *)model + 0x60888), *(int *)((char *)model + 0x69c));
    }
    if (g_voc && pcm && nfeat > 0 && nfeat <= 256) {
        fwrite(&nfeat, 4, 1, g_voc); fwrite(&nsamp, 4, 1, g_voc);
        fwrite(in, sizeof(float), nfeat, g_voc); fwrite(pcm, 2, nsamp, g_voc);
    }
    return r;
}

typedef void (*audio_cb)(const char *, int, int, int, const char *, int, void *);

static FILE *g_pcm;
static long g_bytes;
static int g_calls, g_last_result, g_verbose;

static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static void on_audio(const char *buf, int a, int b, int c, const char *s, int result, void *user) {
    g_calls++; g_last_result = result;
    if (g_verbose)
        fprintf(stderr, "cb %d: buf=%p a=%d b=%d c=%d s=%s result=%d t=%.3f\n", g_calls, (void *)buf, a, b, c,
                s ? s : "(null)", result, now());
    if (buf && a > 0 && g_pcm) { fwrite(buf, 1, a, g_pcm); g_bytes += a; }
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: oracle lib voiceroot corpus outdir [options]\n"); return 2; }
    const char *lang = "en", *country = "US", *type = "l"; int num = 3, mode = 0;
    for (int i = 5; i < argc; i++) {
        if (!strcmp(argv[i], "--lang")) lang = argv[++i];
        else if (!strcmp(argv[i], "--country")) country = argv[++i];
        else if (!strcmp(argv[i], "--type")) type = argv[++i];
        else if (!strcmp(argv[i], "--num")) num = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mode")) mode = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-v")) g_verbose = 1;
    }
    g_trace = getenv("TRACE") != NULL;

    void *h = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    g_lib = h;
    if (getenv("VOCODER")) g_voc = fopen(getenv("VOCODER"), "wb");
    if (getenv("IVC")) g_ivc = fopen(getenv("IVC"), "w");
    g_count = getenv("COUNT") != NULL;
    g_setup = getenv("SETUP") != NULL;
    if (getenv("TRACE")) g_tr = fopen(getenv("TRACE"), "wb");
    if (getenv("TRACE_N")) g_tr_n = atol(getenv("TRACE_N"));
    void *(*New)(void) = dlsym(h, "TTS_ENGINE_New");
    int (*Initialize)(void *) = dlsym(h, "TTS_ENGINE_Initialize");
    int (*SetAudioCallback)(void *, audio_cb, void *) = dlsym(h, "TTS_ENGINE_SetAudioCallback");
    int (*SetVoice)(void *, int, const char *, const char *, const char *, const char *, const char *, unsigned) =
        dlsym(h, "TTS_ENGINE_SetVoice");
    int (*InputText)(void *, const char *) = dlsym(h, "TTS_ENGINE_InputText");
    int (*GetSamplingRate)(void *) = dlsym(h, "TTS_ENGINE_GetSamplingRate");
    int (*GetVersion)(void *) = dlsym(h, "TTS_ENGINE_GetVersion");
    if (!New || !Initialize || !SetAudioCallback || !SetVoice || !InputText || !GetSamplingRate || !GetVersion) {
        fprintf(stderr, "missing TTS_ENGINE_ symbol\n"); return 1;
    }

    void *e = New();
    fprintf(stderr, "engine %p version %d\n", e, GetVersion(e));
    /* The voice comes first: Initialize builds the synthesizer the voice's cfg asks for. */
    int r = SetVoice(e, mode, argv[2], NULL, lang, country, type, (unsigned)num);
    fprintf(stderr, "SetVoice(mode %d, %s, %s_%s %s%02d) -> %d\n", mode, argv[2], lang, country, type, num, r);
    if (r) return 1;
    r = Initialize(e); fprintf(stderr, "Initialize -> %d\n", r);
    if (r) return 1;
    r = SetAudioCallback(e, on_audio, NULL); fprintf(stderr, "SetAudioCallback -> %d\n", r);
    int rate = GetSamplingRate(e); fprintf(stderr, "rate %d\n", rate);

    FILE *in = fopen(argv[3], "r"); if (!in) { perror(argv[3]); return 1; }
    char path[1024]; snprintf(path, sizeof path, "%s/index.tsv", argv[4]);
    FILE *idx = fopen(path, "w"); if (!idx) { perror(path); return 1; }
    char line[8192]; int n = 0;
    while (fgets(line, sizeof line, in)) {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
        if (!L) continue;
        snprintf(path, sizeof path, "%s/%05d.pcm", argv[4], n);
        g_pcm = fopen(path, "wb"); g_bytes = 0; g_calls = 0;
        double t0 = now();
        r = InputText(e, line);
        double t1 = now();
        fclose(g_pcm); g_pcm = NULL;
        fprintf(idx, "%d\t%ld\t%d\t%d\t%.3f\t%s\n", n, g_bytes / 2, r, g_calls, t1 - t0, line);
        fprintf(stderr, "%d: InputText -> %d, %ld samples, %d callbacks, last result %d, %.2fs\n", n, r, g_bytes / 2,
                g_calls, g_last_result, t1 - t0);
        n++;
    }
    fclose(idx);
    if (g_count)
        for (int i = 0; i < g_ncounters; i++)
            fprintf(stderr, "count %8ld  %s (%.2f a frame)\n", g_counters[i].n, g_counters[i].name,
                    g_voc_calls ? (double)g_counters[i].n / g_voc_calls : 0.0);
    fflush(NULL);
    _exit(0);
}
