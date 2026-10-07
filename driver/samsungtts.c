/* samsungtts.dll: driver/samsungtts.h over engine/sstts.h.
 *
 * The engine is single-threaded and not re-entrant, so one thread owns it for the life of the process (with the
 * deep stack the translated code is given everywhere else); samsungtts_speak hands it a job and waits. */
#define SAMSUNGTTS_BUILD
#include "samsungtts.h"
#include "../engine/sstts.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define VERSION "0.2.0"

/* callers take turns in the order they came */
static SRWLOCK g_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_changed = CONDITION_VARIABLE_INIT;
static unsigned long long g_next_ticket, g_serving;

static HANDLE g_thread, g_job_ready, g_job_done;
static int g_engine_state;      /* 0 not started, 1 running, -1 could not start */
static char g_error[1100];
static int g_end_silence = -1;
static volatile LONG g_stop;

/* the job: set by the caller whose turn it is, read by the engine thread */
static struct {
    const char *root, *voice, *text;        /* text NULL: only load the voice */
    int rate, pitch, volume;
    samsungtts_audio_fn audio; void *user;
    int result, sample_rate;
} g_job;

static void on_audio(void *user, const short *pcm, int samples, int word_offset, int word_length) {
    (void)user;
    /* a stop asked for while the voice was loading: the engine forgets a stop when it starts on a text */
    if (g_stop) { sstts_stop(); return; }
    if (g_job.volume >= 100) {
        if (g_job.audio(g_job.user, pcm, samples, g_job.sample_rate, word_offset, word_length)) { g_stop = 1; sstts_stop(); }
        return;
    }
    short quiet[960];
    for (int at = 0; at < samples;) {       /* the engine's pieces are 480 samples; take any size */
        int n = samples - at < 960 ? samples - at : 960;
        for (int i = 0; i < n; i++) quiet[i] = (short)(pcm[at + i] * g_job.volume / 100);
        if (g_job.audio(g_job.user, quiet, n, g_job.sample_rate, word_offset, word_length)) { g_stop = 1; sstts_stop(); return; }
        at += n;
    }
}

static DWORD WINAPI engine_thread(LPVOID arg) {
    (void)arg;
    const char *err = "";
    sstts_prefer_fast_cores();
    if (sstts_init(&err)) {
        snprintf(g_error, sizeof g_error, "the engine could not be started: %s", err);
        g_engine_state = -1;
        SetEvent(g_job_done);
        return 1;
    }
    sstts_fast_denormals(1);
    g_engine_state = 1;
    SetEvent(g_job_done);
    for (;;) {
        WaitForSingleObject(g_job_ready, INFINITE);
        err = "";
        int r = sstts_select(g_job.root, g_job.voice, &err);
        if (r) {
            snprintf(g_error, sizeof g_error, "voice %s: %s", g_job.voice, err);
            g_job.result = r == -2 ? SAMSUNGTTS_FAULT : SAMSUNGTTS_NO_VOICE;
        } else {
            g_job.sample_rate = sstts_rate();
            g_job.result = SAMSUNGTTS_OK;
            if (g_job.text && !g_stop) {
                if (g_end_silence >= 0) sstts_ending_silence(g_end_silence);
                sstts_control(g_job.pitch, 100, g_job.rate);
                r = sstts_speak(g_job.text, on_audio, NULL);
                if (r == -2) { snprintf(g_error, sizeof g_error, "%s", sstts_last_fault()); g_job.result = SAMSUNGTTS_FAULT; }
                else if (r == -1) { snprintf(g_error, sizeof g_error, "voice %s was lost to a fault while it was being set up: %s", g_job.voice, sstts_last_fault()); g_job.result = SAMSUNGTTS_FAULT; }
            }
            if (g_job.result == SAMSUNGTTS_OK && g_stop) g_job.result = SAMSUNGTTS_STOPPED;
        }
        SetEvent(g_job_done);
    }
}

/* Wait for this caller's turn; the engine is started by whoever comes first. Returns 0 with the turn held. */
static int take_turn(void) {
    AcquireSRWLockExclusive(&g_lock);
    unsigned long long mine = g_next_ticket++;
    while (g_serving != mine) SleepConditionVariableSRW(&g_changed, &g_lock, INFINITE, 0);
    ReleaseSRWLockExclusive(&g_lock);
    if (!g_engine_state) {
        g_job_ready = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_job_done = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_thread = CreateThread(NULL, 64u << 20, engine_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (!g_thread) { snprintf(g_error, sizeof g_error, "the engine thread could not be created"); g_engine_state = -1; }
        else WaitForSingleObject(g_job_done, INFINITE);
    }
    return g_engine_state == 1 ? 0 : SAMSUNGTTS_NO_ENGINE;
}

static void give_turn(void) {
    AcquireSRWLockExclusive(&g_lock);
    g_serving++;
    ReleaseSRWLockExclusive(&g_lock);
    WakeAllConditionVariable(&g_changed);
}

static int run(const char *root, const char *voice, const char *text, int rate, int pitch, int volume, samsungtts_audio_fn audio, void *user) {
    int r = take_turn();
    if (!r) {
        if (!root || !voice || !*voice || (text && !audio)) { snprintf(g_error, sizeof g_error, "a voice root, a voice name and an audio function are needed"); r = SAMSUNGTTS_BAD_ARGUMENT; }
        else {
            g_job.root = root; g_job.voice = voice; g_job.text = text;
            g_job.rate = rate < 10 ? 10 : rate > 1000 ? 1000 : rate;
            g_job.pitch = pitch < 10 ? 10 : pitch > 1000 ? 1000 : pitch;
            g_job.volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
            g_job.audio = audio; g_job.user = user; g_job.sample_rate = 0;
            g_stop = 0;         /* a stop is for the call that is running, not for this one that is about to */
            SetEvent(g_job_ready);
            WaitForSingleObject(g_job_done, INFINITE);
            r = text ? g_job.result : g_job.result ? g_job.result : g_job.sample_rate;
        }
    }
    give_turn();
    return r;
}

SAMSUNGTTS_API int samsungtts_speak(const char *voice_root, const char *voice, const char *text, int rate, int pitch, int volume,
                                    samsungtts_audio_fn audio, void *user) {
    return run(voice_root, voice, text ? text : "", rate, pitch, volume, audio, user);
}

SAMSUNGTTS_API int samsungtts_sample_rate(const char *voice_root, const char *voice) {
    return run(voice_root, voice, NULL, 100, 100, 100, NULL, NULL);
}

SAMSUNGTTS_API void samsungtts_stop(void) {
    g_stop = 1;
    if (g_engine_state == 1) sstts_stop();
}

SAMSUNGTTS_API void samsungtts_ending_silence(int milliseconds) { g_end_silence = milliseconds < 0 ? 0 : milliseconds; }
SAMSUNGTTS_API const char *samsungtts_last_error(void) { return g_error; }
SAMSUNGTTS_API const char *samsungtts_version(void) { return VERSION; }
