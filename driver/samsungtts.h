/* samsungtts.dll: Samsung's text-to-speech engine as a plain Windows DLL (64-bit), for programs that want the
 * audio themselves: a screen-reader add-on, a game, a file converter.
 *
 * The engine, code and data, is inside the DLL. A voice is a folder of files from one of Samsung's voice packs;
 * see driver/README.md for where they come from and for an example in Python.
 *
 * Every function may be called from any thread. The engine itself can only do one thing at a time, so calls to
 * samsungtts_speak wait for one another, in the order they were made. All strings are UTF-8, paths included.
 * The calling convention is the C one (cdecl): ctypes.CDLL in Python. */
#ifndef SAMSUNGTTS_H
#define SAMSUNGTTS_H
#ifdef __cplusplus
extern "C" {
#endif

#ifdef SAMSUNGTTS_BUILD
#define SAMSUNGTTS_API __declspec(dllexport)
#else
#define SAMSUNGTTS_API __declspec(dllimport)
#endif

/* Results of samsungtts_speak and samsungtts_sample_rate. */
#define SAMSUNGTTS_OK 0             /* spoken to the end */
#define SAMSUNGTTS_STOPPED 1        /* cut short by samsungtts_stop or by the audio function */
#define SAMSUNGTTS_NO_VOICE (-1)    /* the voice could not be loaded: wrong folder, files missing, a pack the engine cannot use */
#define SAMSUNGTTS_FAULT (-2)       /* the engine faulted on this text; the voice loads afresh at the next call */
#define SAMSUNGTTS_NO_ENGINE (-3)   /* the engine could not be started in this process */
#define SAMSUNGTTS_BAD_ARGUMENT (-4)

/* Audio as it is produced, in pieces of about 20 ms: 16-bit signed mono samples at sample_rate (24000 for the
 * voices seen so far). word_offset and word_length say which word of the text the piece belongs to, in bytes of
 * the UTF-8 text, or are -1 when the engine gives no position. Return 0 to go on, anything else to stop.
 * It is called on the engine's own thread, not the caller's, while samsungtts_speak waits; do not call
 * samsungtts_speak from inside it. */
typedef int (*samsungtts_audio_fn)(void *user, const short *pcm, int samples, int sample_rate, int word_offset, int word_length);

/* Speak text with a voice and return when it has all been delivered to `audio`, or stopped.
 *   voice_root   the folder that holds the voice folders
 *   voice        the voice folder's name: <ll>_<CC>_<type><nn>, for example en_US_l03
 *   rate, pitch  percentages, 100 being the voice's own (rate 200 is twice as fast)
 *   volume       0 to 100
 * The first call starts the engine (0.2 to 0.4 s); a voice not used before loads in 0.1 to 0.4 s, and the three
 * used most recently stay loaded. First audio for a sentence comes after 50 to 100 ms, more for a very long one. */
SAMSUNGTTS_API int samsungtts_speak(const char *voice_root, const char *voice, const char *text, int rate, int pitch, int volume,
                                    samsungtts_audio_fn audio, void *user);

/* Cut short the samsungtts_speak that is running, if one is: it returns SAMSUNGTTS_STOPPED within a few tens of
 * milliseconds once audio is flowing. One that has not produced its first audio yet stops when it gets there.
 * Callers that are waiting their turn are not affected. */
SAMSUNGTTS_API void samsungtts_stop(void);

/* Load a voice (as samsungtts_speak would) and return its sample rate in Hz, or a negative result. */
SAMSUNGTTS_API int samsungtts_sample_rate(const char *voice_root, const char *voice);

/* The silence the engine puts after the last word of a call, in milliseconds. The engine's own is 500; a screen
 * reader will want less. Applies to the calls that follow. */
SAMSUNGTTS_API void samsungtts_ending_silence(int milliseconds);

/* What went wrong in the last call that returned a negative result, in English. For SAMSUNGTTS_FAULT it names
 * the engine function and the call stack, which is what a bug report needs. */
SAMSUNGTTS_API const char *samsungtts_last_error(void);

/* The version of this DLL, "0.2.0". */
SAMSUNGTTS_API const char *samsungtts_version(void);

#ifdef __cplusplus
}
#endif
#endif
