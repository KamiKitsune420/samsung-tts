/* The translated Samsung TTS engine behind a small C interface.
 *
 * The engine is not re-entrant: call everything from one thread (the SAPI layer keeps a worker thread for it).
 * The thread needs a large stack, 64 MB reserved: the translated code recurses on the host stack. */
#ifndef SSTTS_H
#define SSTTS_H
#ifdef __cplusplus
extern "C" {
#endif

/* Audio as it is produced: 16-bit mono at sstts_rate(). word_offset and word_length locate the word being spoken
 * in the UTF-8 text given to sstts_speak, in bytes; they are -1 when the engine gives no position. */
typedef void (*sstts_audio_fn)(void *user, const short *pcm, int samples, int word_offset, int word_length);

/* Once per process. Maps the engine's data, which is in the program beside its translated code, and runs the
 * engine's static initialisers. Returns 0, or a message in *error. */
int sstts_init(const char **error);

/* Make a voice the current one, loading it if it is not loaded. voice_root holds one folder per voice pack,
 * named <ll>_<CC>_<type><nn> as `voice` is, e.g. voice_root/en_US_l03 with the pack's cfg, lng and .ivc files.
 * A few voices stay loaded, so going back to one is immediate. Returns 0, or a message in *error. */
int sstts_select(const char *voice_root, const char *voice, const char **error);

/* The rest act on the current voice. */
int sstts_rate(void);
int sstts_version(void);

/* Percentages, 100 being the voice's own. */
void sstts_control(int pitch, int volume, int rate);

/* The silence the engine appends to an utterance, in milliseconds (500 by default). */
void sstts_ending_silence(int ms);

/* Speak one utterance, calling fn for each piece of audio, and return when it is finished or stopped.
 * Returns the engine's result: 0 for success, 7 when stopped; -2 if the engine faulted, in which case the
 * voice is dropped and loads again at the next sstts_select. */
int sstts_speak(const char *utf8, sstts_audio_fn fn, void *user);

/* After sstts_speak or sstts_select returned -2: what went wrong inside the engine. */
const char *sstts_last_fault(void);

/* How many voices could not be torn down after a fault and were abandoned as they were. Each is 100 to 300 MB
 * lost until the process ends. */
long sstts_abandoned(void);

/* Abandon the utterance being spoken. Call it from the audio function, or from another thread: it only sets a
 * flag, which the engine sees at its next audio callback or vocoder frame. */
void sstts_stop(void);

/* 1: treat denormal numbers as zero on the calling thread (MXCSR DAZ and FTZ). Much faster on x86, where the
 * acoustic model's many denormal values each cost a microcode assist; the audio has been identical in every
 * comparison made so far, but that is an observation, not a guarantee. 0 restores exact arithmetic. */
void sstts_fast_denormals(int on);

/* Ask Windows to run the calling thread on the fast cores of a hybrid CPU. */
void sstts_prefer_fast_cores(void);

#ifdef __cplusplus
}
#endif
#endif
