/* The translated engine as a command-line program, the Windows counterpart of tools/oracle/oracle.c.
 *
 *   engine <voice root> <corpus.txt> <outdir> [options]
 *
 * One utterance a line. Writes NNNNN.pcm (16-bit mono) per line and index.tsv, like the oracle, so
 * tools/compare_runs.py can compare the two.
 *   --voice V       the voice folder under the voice root, e.g. en_GB_l02 (default en_US_l03)
 *   --fast          denormals as zero (see sstts_fast_denormals)
 *   --rate N, --pitch N    percentages
 *   --stop-after N  stop each utterance after N pieces of audio, and report how much more arrived
 *   --profile       sample where the time goes;  --watch  print where it is every 3 s;  --files  log file opens
 */
#include "sstts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

void a2c_profile_start(void);
void a2c_profile_report(int top);
void a2c_watch(unsigned period_ms);
void a2c_heap_stats(size_t *blocks, size_t *bytes, size_t *mapped);
extern int a2c_trace_files;

static FILE *g_pcm;
static long g_samples, g_pieces, g_after_stop;
static int g_stop_after;
static LARGE_INTEGER g_fq, g_t0;
static double g_first;

static void on_audio(void *user, const short *pcm, int samples, int word_offset, int word_length) {
    (void)user; (void)word_offset; (void)word_length;
    if (!g_pieces) { LARGE_INTEGER t; QueryPerformanceCounter(&t); g_first = (double)(t.QuadPart - g_t0.QuadPart) / (double)g_fq.QuadPart; }
    g_pieces++;
    if (g_stop_after && g_pieces > g_stop_after) g_after_stop += samples;
    if (g_pcm) fwrite(pcm, 2, (size_t)samples, g_pcm);
    g_samples += samples;
    if (g_stop_after && g_pieces == g_stop_after) sstts_stop();
}

static DWORD WINAPI run(LPVOID arg) {
    char **argv = arg; int argc = 0;
    while (argv[argc]) argc++;
    int fast = 0, profile = 0, rate = 100, pitch = 100, end_silence = -1;
    const char *voice = "en_US_l03";     /* the folder under the voice root: <ll>_<CC>_<type><nn> */
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--fast")) fast = 1;
        else if (!strcmp(argv[i], "--profile")) profile = 1;
        else if (!strcmp(argv[i], "--watch")) a2c_watch(3000);
        else if (!strcmp(argv[i], "--files")) a2c_trace_files = 1;
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pitch") && i + 1 < argc) pitch = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stop-after") && i + 1 < argc) g_stop_after = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--voice") && i + 1 < argc) voice = argv[++i];
        else if (!strcmp(argv[i], "--end-silence") && i + 1 < argc) end_silence = atoi(argv[++i]);
    }
    sstts_prefer_fast_cores();
    QueryPerformanceFrequency(&g_fq);
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    const char *err = "";
    /* --voice takes a comma-separated list; the lines of the corpus go through it in turn */
    static char voices[16][32]; int nvoices = 0;
    for (const char *p = voice; *p && nvoices < 16;) {
        size_t n = strcspn(p, ",");
        snprintf(voices[nvoices++], sizeof voices[0], "%.*s", (int)n, p);
        p += n + (p[n] == ',');
    }
    if (sstts_init(&err)) { fprintf(stderr, "open: %s\n", err); return 1; }
    if (sstts_select(argv[1], voices[0], &err)) { fprintf(stderr, "open: %s: %s\n", voices[0], err); return 1; }
    QueryPerformanceCounter(&b);
    fprintf(stderr, "engine version %d, %d Hz, opened in %.2f s\n", sstts_version(), sstts_rate(),
            (double)(b.QuadPart - a.QuadPart) / (double)g_fq.QuadPart);
    if (fast) sstts_fast_denormals(1);

    FILE *in = fopen(argv[2], "rb"); if (!in) { perror(argv[2]); return 1; }
    char path[1024]; snprintf(path, sizeof path, "%s/index.tsv", argv[3]);
    FILE *idx = fopen(path, "wb"); if (!idx) { perror(path); return 1; }
    static char line[8192]; int n = 0; double total_audio = 0, total_time = 0;
    if (profile) a2c_profile_start();
    while (fgets(line, sizeof line, in)) {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
        if (!L) continue;
        snprintf(path, sizeof path, "%s/%05d.pcm", argv[3], n);
        g_pcm = fopen(path, "wb"); g_samples = 0; g_pieces = 0; g_after_stop = 0;
        QueryPerformanceCounter(&a);
        if (sstts_select(argv[1], voices[n % nvoices], &err)) { fprintf(stderr, "open: %s: %s\n", voices[n % nvoices], err); return 1; }
        QueryPerformanceCounter(&b);
        if (nvoices > 1) {
            size_t blocks, bytes, mapped;
            a2c_heap_stats(&blocks, &bytes, &mapped);
            fprintf(stderr, "voice %s selected in %.0f ms; the engine holds %zu blocks, %.1f MB, and %.1f MB mapped\n", voices[n % nvoices],
                    1000.0 * (double)(b.QuadPart - a.QuadPart) / (double)g_fq.QuadPart, blocks, bytes / 1048576.0, mapped / 1048576.0);
        }
        int hz = sstts_rate();      /* before speaking: a fault drops the voice */
        if (end_silence >= 0) sstts_ending_silence(end_silence);
        sstts_control(pitch, 100, rate);
        QueryPerformanceCounter(&g_t0);
        int r = sstts_speak(line, on_audio, NULL);
        QueryPerformanceCounter(&b);
        double sec = (double)(b.QuadPart - g_t0.QuadPart) / (double)g_fq.QuadPart, audio = g_samples / (double)hz;
        if (r == -2) fprintf(stderr, "%d: the engine faulted: %s\n", n, sstts_last_fault());
        fclose(g_pcm); g_pcm = NULL;
        fprintf(idx, "%d\t%ld\t%d\t%ld\t%.3f\t%s\n", n, g_samples, r, g_pieces, sec, line);
        fprintf(stderr, "%d: result %d, %ld samples (%.2f s) in %.2f s: %.1f x real time, first audio after %.0f ms", n, r,
                g_samples, audio, sec, sec > 0 ? audio / sec : 0.0, g_first * 1000);
        if (g_stop_after) fprintf(stderr, "; %ld samples arrived after the stop", g_after_stop);
        fprintf(stderr, "\n");
        total_audio += audio; total_time += sec;
        n++;
    }
    fclose(idx);
    fprintf(stderr, "total: %.2f s of audio in %.2f s: %.2f x real time\n", total_audio, total_time, total_time > 0 ? total_audio / total_time : 0.0);
    if (profile) a2c_profile_report(28);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: engine voiceroot corpus outdir [options]\n"); return 2; }
    /* the engine wants a deep stack and, on hybrid CPUs, the fast cores */
    HANDLE t = CreateThread(NULL, 64u << 20, run, argv, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    SetThreadPriority(t, THREAD_PRIORITY_ABOVE_NORMAL);
    WaitForSingleObject(t, INFINITE);
    DWORD code = 1; GetExitCodeThread(t, &code);
    return (int)code;
}
