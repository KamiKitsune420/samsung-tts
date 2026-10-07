/* Soak test of the translated engine (engine/sstts.h): speak for a long time the way a screen reader does, and
 * report anything that changes with time.
 *
 *   soak <voice root> <corpus.txt> [options]
 *
 *   --voices A,B,..   voice folders to go through (default en_US_l03); the voice changes every --switch utterances
 *   --switch N        utterances between voice changes (default 7)
 *   --minutes M       how long to run (default 5);  --count N  stop after N utterances instead
 *   --stops P         percentage of utterances that are stopped part way, as a screen reader's purge does (default 60)
 *   --rates           also vary rate and pitch
 *   --rate N          speak everything at this rate, a percentage (300 gets through a long corpus sooner)
 *   --sweep           every line of the corpus in order with every voice, once, nothing stopped: which text a voice
 *                     cannot take
 *   --seed S          for the random choices (default 1)
 *   --stack KB        run on a thread with a stack of this size instead of the program's 64 MB: small enough, and
 *                     the engine runs out of stack, which must cost an utterance and not the process
 *
 * What is checked: every utterance spoken in full at the default rate must give the same samples each time it is
 * spoken with that voice (the first rendering is the reference), and some audio if it has a letter or a digit in
 * it; a stopped one must stop (result 7, little audio after the stop); nothing may fault. Memory, handles and speed are printed once a minute, so a leak or a slowdown
 * shows as a trend. Exit status 0 if nothing was wrong.
 *
 * Build: tests\build.ps1
 */
#include "../engine/sstts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include <psapi.h>

#pragma comment(lib, "psapi.lib")

#define MAX_LINES 4096
#define MAX_VOICES 64
static char *g_lines[MAX_LINES]; static int g_nlines;
static char g_voices[MAX_VOICES][32]; static int g_nvoices;
static uint64_t *g_ref;         /* per voice and line: hash of the full rendering, 0 if not rendered yet */
static long *g_ref_samples;

static uint64_t g_hash; static long g_samples, g_pieces, g_after_stop; static int g_stop_at;
static double g_stopped_at;       /* when the stop was asked for */
static double now(void);
static uint64_t g_rng = 1;
static unsigned rnd(void) { g_rng = g_rng * 6364136223846793005ULL + 1442695040888963407ULL; return (unsigned)(g_rng >> 33); }

static void on_audio(void *user, const short *pcm, int samples, int word_offset, int word_length) {
    (void)user; (void)word_offset; (void)word_length;
    g_pieces++;
    if (g_stop_at && g_pieces > g_stop_at) g_after_stop += samples;
    for (int i = 0; i < samples; i++) g_hash = (g_hash ^ (uint16_t)pcm[i]) * 1099511628211ULL;
    g_samples += samples;
    if (g_stop_at && g_pieces == g_stop_at) { g_stopped_at = now(); sstts_stop(); }
}

/* punctuation alone is rightly silent */
static int speakable(const char *s) {
    for (; *s; s++) if ((*s >= '0' && *s <= '9') || ((*s | 32) >= 'a' && (*s | 32) <= 'z')) return 1;
    return 0;
}

static double now(void) {
    static LARGE_INTEGER f; LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

void a2c_heap_stats(size_t *blocks, size_t *bytes, size_t *mapped);

/* the process's private memory and handles, and what the engine itself holds: allocated blocks and megabytes */
static char *resources(void) {
    static char text[200];
    PROCESS_MEMORY_COUNTERS_EX pmc; memset(&pmc, 0, sizeof pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc);
    DWORD h = 0; GetProcessHandleCount(GetCurrentProcess(), &h);
    size_t blocks, bytes, mapped;
    a2c_heap_stats(&blocks, &bytes, &mapped);
    snprintf(text, sizeof text, "%.0f MB private, %lu handles, engine holds %zu blocks and %.0f MB, %ld voices abandoned",
             (double)pmc.PrivateUsage / 1048576.0, (unsigned long)h, blocks, (double)(bytes + mapped) / 1048576.0, sstts_abandoned());
    return text;
}

static int soak(int argc, char **argv);
static int g_argc; static char **g_argv;
static DWORD WINAPI on_thread(LPVOID arg) { (void)arg; return (DWORD)soak(g_argc, g_argv); }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: soak voiceroot corpus [options]\n"); return 2; }
    for (int i = 3; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--stack")) {
            g_argc = argc; g_argv = argv;
            HANDLE t = CreateThread(NULL, (SIZE_T)atoi(argv[i + 1]) * 1024, on_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
            WaitForSingleObject(t, INFINITE);
            DWORD code = 1; GetExitCodeThread(t, &code);
            return (int)code;
        }
    return soak(argc, argv);
}

static int soak(int argc, char **argv) {
    const char *voices = "en_US_l03"; double minutes = 5; long count = 0; int every = 7, stops = 60, rates = 0, sweep = 0, fixed_rate = 100;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--voices") && i + 1 < argc) voices = argv[++i];
        else if (!strcmp(argv[i], "--switch") && i + 1 < argc) every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--minutes") && i + 1 < argc) minutes = atof(argv[++i]);
        else if (!strcmp(argv[i], "--count") && i + 1 < argc) count = atol(argv[++i]);
        else if (!strcmp(argv[i], "--stops") && i + 1 < argc) stops = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) g_rng = (uint64_t)atoll(argv[++i]);
        else if (!strcmp(argv[i], "--rates")) rates = 1;
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) fixed_rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sweep")) sweep = 1;
        else if (!strcmp(argv[i], "--stack") && i + 1 < argc) i++;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (every < 1) every = 1;
    for (const char *p = voices; *p && g_nvoices < MAX_VOICES;) {
        size_t n = strcspn(p, ",");
        snprintf(g_voices[g_nvoices++], sizeof g_voices[0], "%.*s", (int)n, p);
        p += n + (p[n] == ',');
    }
    FILE *in = fopen(argv[2], "rb"); if (!in) { perror(argv[2]); return 2; }
    static char line[16384];
    while (g_nlines < MAX_LINES && fgets(line, sizeof line, in)) {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
        if (L) g_lines[g_nlines++] = _strdup(line);
    }
    fclose(in);
    if (!g_nlines) { fprintf(stderr, "the corpus is empty\n"); return 2; }
    g_ref = calloc((size_t)g_nvoices * g_nlines, sizeof *g_ref);
    g_ref_samples = calloc((size_t)g_nvoices * g_nlines, sizeof *g_ref_samples);

    sstts_prefer_fast_cores();
    const char *err = "";
    if (sstts_init(&err)) { fprintf(stderr, "init: %s\n", err); return 2; }
    sstts_fast_denormals(1);

    long n = 0, wrong = 0, faults = 0, changed = 0, late = 0, silent = 0, load_failed = 0, stopped = 0;
    double t0 = now(), next_report = t0 + 60, audio = 0, busy = 0, window_audio = 0, window_busy = 0, worst_stop = 0;
    printf("start: %s; %d lines, %d voices\n", resources(), g_nlines, g_nvoices);
    fflush(stdout);
    int v = 0;
    for (;;) {
        if (count ? n >= count : !sweep && now() - t0 >= minutes * 60) break;
        int l;
        if (sweep) {
            if (n >= (long)g_nvoices * g_nlines) break;
            v = (int)(n / g_nlines); l = (int)(n % g_nlines);
        } else {
            if (n && n % every == 0) v = (v + 1) % g_nvoices;
            l = (int)(rnd() % (unsigned)g_nlines);
        }
        if (sstts_select(argv[1], g_voices[v], &err)) {
            printf("#%ld %s: cannot select the voice: %s\n", n, g_voices[v], err);
            load_failed++; wrong++; n++;
            if (sweep) n = (n / g_nlines + (n % g_nlines != 0)) * g_nlines;     /* on to the next voice */
            else if (load_failed > 50) { printf("giving up: the voices no longer load\n"); break; }
            continue;
        }
        int rate = fixed_rate, pitch = 100;
        if (rates && rnd() % 3 == 0) { rate = 40 + (int)(rnd() % 260); pitch = 70 + (int)(rnd() % 80); }
        int stop = !sweep && (int)(rnd() % 100) < stops;
        sstts_ending_silence(100);
        sstts_control(pitch, 100, rate);
        g_hash = 1469598103934665603ULL; g_samples = g_pieces = g_after_stop = 0;
        g_stop_at = stop ? 1 + (int)(rnd() % 40) : 0;
        double a = now();
        int r = sstts_speak(g_lines[l], on_audio, NULL);
        double sec = now() - a, secs_audio = (double)g_samples / 24000.0;
        busy += sec; audio += secs_audio; window_busy += sec; window_audio += secs_audio;
        int full = !(g_stop_at && g_pieces >= g_stop_at);       /* a short utterance can end before the stop point */
        if (r == -2) { printf("#%ld %s line %d \"%.40s\": FAULT: %s\n", n, g_voices[v], l, g_lines[l], sstts_last_fault()); faults++; wrong++; }
        else if (full) {
            if (r != 0) { printf("#%ld %s line %d: result %d\n", n, g_voices[v], l, r); wrong++; }
            else if (g_samples < 2400 && speakable(g_lines[l])) { printf("#%ld %s line %d: no audio (%ld samples)\n", n, g_voices[v], l, g_samples); silent++; wrong++; }
            else if (rate == fixed_rate && pitch == 100) {
                size_t k = (size_t)v * g_nlines + l;
                if (!g_ref[k]) { g_ref[k] = g_hash; g_ref_samples[k] = g_samples; }
                else if (g_ref[k] != g_hash) {
                    printf("#%ld %s line %d: the audio CHANGED: %ld samples, was %ld the first time\n", n, g_voices[v], l, g_samples, g_ref_samples[k]);
                    changed++; wrong++;
                }
            }
        } else {
            stopped++;
            if (now() - g_stopped_at > worst_stop) worst_stop = now() - g_stopped_at;
            if (r != 7 && r != 0) { printf("#%ld %s line %d: stopped, result %d\n", n, g_voices[v], l, r); wrong++; }
            if (g_after_stop > 24000) { printf("#%ld %s line %d: %ld samples arrived after the stop\n", n, g_voices[v], l, g_after_stop); late++; wrong++; }
        }
        n++;
        if (now() >= next_report) {
            printf("%4.0f min: %ld utterances (%ld stopped), %.1f x real time this minute, %ld wrong; %s\n",
                   (now() - t0) / 60, n, stopped, window_busy > 0 ? window_audio / window_busy : 0.0, wrong, resources());
            fflush(stdout);
            window_audio = window_busy = 0; next_report += 60;
        }
    }
    printf("end: %ld utterances (%ld stopped) in %.1f min, %.0f s of audio at %.1f x real time, slowest stop %.0f ms; %s\n",
           n, stopped, (now() - t0) / 60, audio, busy > 0 ? audio / busy : 0.0, worst_stop * 1000, resources());
    printf("wrong: %ld (faults %ld, audio changed %ld, no audio %ld, late after stop %ld, voice not loading %ld)\n",
           wrong, faults, changed, silent, late, load_failed);
    return wrong ? 1 : 0;
}
