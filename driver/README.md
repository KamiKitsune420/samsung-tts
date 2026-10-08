# samsungtts.dll: the driver

Samsung's text-to-speech engine as one plain 64-bit Windows DLL with a C interface, for anyone who wants to build
something of their own on it: a screen-reader add-on, a game, a tool that turns text into audio files. You give
it a voice folder and a text; it calls you back with 16-bit audio as it is produced. The SAPI voices in this
project are one such program built on the same engine.

The engine, its code and its data, is inside the DLL: no other file of the engine's is needed. Voices are
separate (below).

## What is in the package

| | |
|---|---|
| `samsungtts.dll` | the driver, 64-bit, about 47 MB. No dependencies beyond Windows itself |
| `samsungtts.h` | the interface, with the details of every function |
| `samsungtts.lib` | import library, for C and C++ |
| `example.py` | speaks a text into a WAV file from Python with `ctypes` |
| `README.md` | this file |
| `bionic-libm-NOTICE.txt` | the licence notices of Android's maths library, which is inside the DLL |
| `src.zip` | the source of everything, as in the repository |

## The interface

Seven functions, C calling convention, UTF-8 strings (paths too). `samsungtts.h` is the reference.

```c
typedef int (*samsungtts_audio_fn)(void *user, const short *pcm, int samples, int sample_rate,
                                   int word_offset, int word_length);

int samsungtts_speak(const char *voice_root, const char *voice, const char *text,
                     int rate, int pitch, int volume, samsungtts_audio_fn audio, void *user);
void samsungtts_stop(void);
int samsungtts_sample_rate(const char *voice_root, const char *voice);
void samsungtts_ending_silence(int milliseconds);
const char *samsungtts_last_error(void);
const char *samsungtts_version(void);
```

- `samsungtts_speak` returns when the whole text has been delivered to your audio function, or it was stopped.
  Results: 0 spoken to the end, 1 stopped, -1 the voice could not be loaded, -2 the engine faulted on this text,
  -3 the engine could not start, -4 a bad argument. After a negative result `samsungtts_last_error` says why.
- Rate and pitch are percentages, 100 being the voice's own. Volume is 0 to 100.
- The audio function gets pieces of about 20 ms, mono, 16-bit, at `sample_rate` (24000 Hz for every voice tried).
  `word_offset` and `word_length` locate the word being spoken, in bytes of the UTF-8 text, or are -1. Return 0
  to go on, anything else to stop. **It runs on the engine's thread**, not yours, while your call to
  `samsungtts_speak` waits. Copy the samples before you return: the memory is the engine's.
- `samsungtts_stop` cuts short the call that is running; call it from any thread.
- Calls from several threads wait for one another, in the order they came. There is one engine per process.

## In Python

```python
import example                      # the file in this package
dll = example.load()                # samsungtts.dll beside it
result, rate, pcm, words = example.speak(dll, r"C:\voices", "en_US_l03", "Hello there.")
```

`example.py` is 80 lines and shows the `ctypes` declarations. To play as it speaks, hand each piece to your audio
output inside the audio function instead of collecting them.

For an NVDA add-on: the DLL is 64-bit, so it loads into 64-bit NVDA (the 2026 releases) and not into the older,
32-bit ones. A synth driver's `speak` would run `samsungtts_speak` on a thread of its own, feed the pieces
to `nvwave.WavePlayer`, raise index and word callbacks from `word_offset`, and call `samsungtts_stop` from
`cancel`. Put the voice folders somewhere under the add-on or the user's configuration: paths with any
characters work.

## Voices

A voice is a folder named `<ll>_<CC>_<type><nn>` (for example `en_US_l03`) holding the files of one of Samsung's
voice packs: `cfg`, `lng`, the voice model (`regular.ivc` or `tiny.ivc`) and `cache.tsv`. The folder that holds
those folders is the voice root you pass in.

The packs are Android packages (`com.samsung.SMT.lang_<code>`) in Samsung's Galaxy Store; the files are the
contents of the package's `assets` folder. Three ways to get them:

- The voice centre of this project's SAPI release downloads and unpacks them; its folder
  `C:\Program Files\SamsungTTS\data\voice` is a voice root you can point the driver at.
- `tools/fetch_voice.py <code>` in the source does the same from a command line, and `tools/catalogue.py` lists
  what the store offers. Both are short and use only Python's standard library, so an add-on can carry its own
  copy: the store answers a plain HTTPS request with the download address.
- By hand: get the package and unzip its `assets` folder.

54 of the 57 packs speak with this engine; `es_US_l01`, `es_US_g01` and `nl_NL_f00` do not. The engine is the
build that came with `en_us_l03` 3.1.25.4; the other packs were made for other builds, and only `en_us_l03` has
been compared sample for sample with Samsung's own engine. `tools/try_voices.py` is how the 57 were tried.

## How fast, and what to expect

On an i7-1355U laptop, one thread: 4 to 6 times real time for the large English voices, up to 30 times for the
small ones. First audio 50 to 100 ms after the call for an ordinary sentence; a sentence of several hundred
characters takes longer (2 s for 485 characters of numbers), and a stop that arrives in that time takes effect
when the first audio would have come. Loading the DLL costs nothing until the first call; the engine starts in
0.2 to 0.4 s; a voice loads in 0.1 to 0.4 s and takes 100 to 300 MB while loaded (three stay loaded).

The engine is Samsung's, translated; its bugs came with it. A fault inside it is caught: the call returns -2, the
text is lost, the voice loads again at the next call, and your program goes on. Two inputs that crashed it are
corrected (see `docs/notes.md` in the source). If you meet another, `samsungtts_last_error` names the function
and the call stack: that is what a report needs.

## Building it yourself

`build.ps1` in the source builds this DLL into `dist\SamsungTTS-Driver`. It needs the voice pack
`en_us_l03` 3.1.25.4 and an arm64 Android `libm.so` to translate from, Python 3 with `capstone`, and Visual
Studio's clang-cl; the README in the source has the steps.

## Whose it is

The interface, the translator and the code around the engine are this project's. **The engine inside the DLL is
Samsung's**: their library, translated mechanically to native code, with its data. The voices are Samsung's too.
Nobody here has Samsung's permission to pass either on; whether you may use or redistribute them is for you to
judge under Samsung's terms and your own law. The maths library inside is Android's (bionic `libm`), translated
the same way, under the BSD-style licences in `bionic-libm-NOTICE.txt`: pass that file on with the DLL.
