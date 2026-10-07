# Samsung TTS: engine notes

What has been established about Samsung's engine, with addresses. Addresses are virtual addresses in the arm64
`libsamsungtts.so` of voice pack `com.samsung.SMT.lang_en_us_l03` 3.1.25.4 (engine version 512508201). Nothing of
Samsung's is to be committed; the pack is unpacked in `work/l03`, which git ignores.

## The pack

| file | size | what |
|---|---|---|
| `lib/arm64-v8a/libsamsungtts.so` | 37 MB | the whole engine; 13 MB of code, of which about 2 MB is Samsung's and the rest a static ONNX Runtime, frugally-deep, fastText, RE2, protobuf |
| `lib/armeabi-v7a/libsamsungtts.so` | 10.5 MB | the same engine without ONNX Runtime (2.5 MB of code), with the same Tacotron and LPCNet classes |
| `assets/cfg` | 5.5 KB | binary settings (24000 Hz at offset 0x0c), then key/value strings: voice `Stephanie`, `en-US`, `neural="1"`, `_synthesis_type UC_I`, `_DB_name regular` |
| `assets/lng` | 33 MB | language data: part-of-speech tags, lexicon, models for the text front end |
| `assets/regular.ivc` | 45 MB | the voice. Magic `ivc\x06`, header size 0xd0, then high-entropy data read through `ivc_stream`; an `ORTM` (ONNX Runtime format) marker at 0x11e6b03 |
| `assets/cache.tsv` | 1.3 KB | phoneme strings with rewrite targets |

The library imports only libc, libm, libdl and liblog, and exports all its symbols (32,000 functions, C++ names intact).

## Calling it

`TTS_ENGINE_*` are thin C wrappers over the `IEngine` vtable (`_ZTV7IEngine` at 0x21cb4a0). The order that works:

```
e = TTS_ENGINE_New()
TTS_ENGINE_SetVoice(e, mode=0, root, NULL, "en", "US", "l", 3)   opens root/en_US_l03/{cfg,lng}; root needs no trailing slash
TTS_ENGINE_Initialize(e)                                           builds the synthesizer the cfg asks for, opens regular.ivc
TTS_ENGINE_SetAudioCallback(e, cb, user)                           must come after Initialize: it is forwarded to the synthesizer
TTS_ENGINE_InputText(e, utf8)                                      blocks; the callback runs on the calling thread
```

Initialize before SetVoice returns 0 and then InputText produces nothing. Modes 0, 2, 3, 7 and 8 find `en_US_l03`;
1 looks for `en_US_dict_l03`, 4 for `en_US_r00`, 5 for `en_US_t00`, 6 for `xx_XX_l03`.

The callback is `void cb(const char *pcm, int bytes, int word_offset, int word_length, const char *text_from_word,
int result, void *user)`. Audio comes 960 bytes (480 samples, 20 ms, 16-bit mono 24 kHz) at a time with result 0;
result 8 opens an utterance, 2 then 1 close it. `word_offset` and `word_length` index the input text.

## The oracle

`tools/oracle/oracle.c` is that sequence as a program; `run_oracle.py` pushes it, the library and the voice to
`/data/local/tmp/smt` and pulls WAVs. It runs on the `skype` AVD (x86_64, Android 15), whose arm64 translation
executes the library unmodified, at 0.4 to 0.7 times real time.

**Deterministic**: two runs of two sentences gave identical samples (50,400 and 78,240). Not yet checked over a
larger or harder corpus, or against a real arm64 device.

## The pipeline, from symbol names (not yet traced at run time)

```
IEngine -> ISynthesizer::CREATE_NEURAL (0x157b4d8) -> CDiphoneTTS / TPort::processNeural (0x15ab780)
text front end   CSuperviser (text-to-text), CLexer_1, CTokenPattern, CEnglish / ILanguage (PoS, G2P, phrase break, labels)
acoustic model   Tacotron (initialize_lazy(ivc_stream&) 0x158fc9c): Encoder (forward_nat 0x1609574: duration
                 predictor, Gaussian upsampler, style encoders), DecoderCell (forward_nat 0x1603da4), Postnet (0x1611f6c)
vocoder          lpcnet:: (lpcnet_synthesize 0x15d6aa0, run_sample_network 0x15d5dc0, sparse GRU, sample_from_pdf,
                 declicker, hf_tone_remover); model versions LPCNET_V01..V07
driver           TacotronLPCNet::synthesize (0x15b26cc)
```

A second neural engine is present, `RVQVAESupervisor` / `RVQVAE` / `PriorEncoder` / `PhonemeEncoder` (0x1612f20
onward), which is where ONNX Runtime would be used. `TPort::processNeural` chooses between the two. **This voice
runs `TacotronLPCNet`** (hooked: its constructor is called once an utterance, the other never), so ONNX Runtime is
not on the path, whatever the `ORTM` marker in the voice file belongs to.

## Hooking

The library calls 12,615 of its own exported functions through its import table, and the dynamic linker resolves
those against the executable first. So a function defined in `oracle.c` under the engine's mangled name replaces
it, and forwards to the real one with `dlsym(handle, name)`. No patching. Reachable this way, among others:
`lpcnet::lpcnet_synthesize`, `lpcnet::run_frame_network`, `ivc_stream::read`, `TacotronLPCNet::set_speaker`. Not
reachable: functions only called directly, such as `Postnet::forward`.

## The vocoder's boundary

`lpcnet::lpcnet_synthesize(LPCNetState*, float *features, short *pcm)`: **22 features in, 240 samples (10 ms)
out**. The first calls of an utterance pass `pcm == NULL` (warm-up). `VOCODER=<file>` makes the oracle record every
call. For the two test sentences, all 436 recorded frames occur verbatim, in order and back to back, in the audio
the callback delivers; the remaining 12,000 samples of each utterance are trailing silence (0.5 s). So the
delivered audio is the vocoder's output and nothing else.

The code uses fused multiply-add (`fmadd` in `lpcnet_synthesize`), which a bit-exact x86 port has to reproduce.

Unit-selection (`UnitSel`, `TPackBinDiphoneDB`, `PSOLA`) and HMM (`CModel`, `CParametricTTS`, STRAIGHT vocoders)
code is also in the library and is not what a `neural="1"` voice uses.

## The voice file (`tools/ivc.py`)

`regular.ivc` is a 0x124-byte header followed by a payload XORed with a repeating 16-byte key. **The key is in the
file's own header, at 0xac.** `ivc_stream::read` (0x1590d60) is the whole of it: the object is {+0 memory base,
+8 size, +0x10 key, +0x18 key length, +0x1c position, +0x20 key phase, +0x24 file offset, +0x30 istream*,
+0x38 scrambled}, and a read copies bytes then XORs them with key[(phase + position) mod length]. Sub-streams keep the
phase of their offset in the payload, so one XOR over the payload unscrambles everything. Checked: all 512 reads the
engine makes while loading match the payload unscrambled that way.

The payload is `u32 1`, then sections of `u32 type, u32 size, data`, ended by type 0xffffffff:

| type | size | what |
|---|---|---|
| 1 | 24,637,908 | encoder; starts with a 0x4c header `cmc`, `1.4.1`, `stephanie john lisa nat_feat_extactor`, `2024-10-03`, `encoder` |
| 2 | 14,070,284 | decoder, same header |
| 3 | 1,106,640 | postnet, same header |
| 6 | 4,908,867 | LPCNet model, `LPCNET_V06` |
| 5 | 758 | symbol table as text: `"_" 0`, `"~" 1`, punctuation, then phonemes with stress digits (`"ae1" 11` ...), 87 or so |
| 10 | 39 | speakers as text: `john 1`, `lisa 2`, `lisa_vivid 2`, `stephanie 0` |
| 16 | 6,152 | style tokens: `u32 3, u32 512`, then 3 x 512 floats |

So the pack holds three speakers (Stephanie, John, Lisa) in one multi-speaker model.

### The LPCNet model

Header: `LPCNET_V06`, then f32 32768, u32 1, u32 0, f32 1, f32 -5.5, f32 1, f32 0.3, u32 64 (pitch embedding width),
u32 380 (pitch embedding rows), 88 floats (meanings not established). Then layers, each `u8 kind, u8 name length,
name`, dimensions and float arrays. `tools/ivc.py --lpcnet` parses all 4,908,867 bytes:

| kind | name | shape |
|---|---|---|
| embed | `gru_a_embed_sig_0`, `gru_a_embed_pred_0`, `gru_a_embed_exc_0` | 256 x 1152 each (already multiplied into GRU A's input space, 3 x 384) |
| dense | `gru_a_dense_feature` | 128 -> 1152 |
| dense | `gru_b_dense_feature` | 128 -> 48 |
| sparse_gru | `sparse_gru_a` | 384 units, 17,696 non-zero weights, 1,178 index entries |
| embed | `embed_bunch_bit_0` | 128 x 16 |
| embed | `embed_pitch` | 380 x 64 |
| conv1d | `feature_conv1_0`, `feature_conv2_0` | kernel 3, 86 -> 128 -> 128 (86 = 22 features + 64 pitch embedding) |
| dense | `feature_dense1`, `feature_dense2` | 128 -> 128 |
| gru | `gru_b` | 384 in, 16 units |
| mdense | `dual_fc_0_0`, `dual_fc_0_1` | 16 -> 2 x 128, and 16 -> 2 x 16 |

This is Xiph's LPCNet with Samsung's changes: 24 kHz, a small GRU B, and an output split into two stages ("bunch").
The model can run in float32 or float16 (`lpcnet::FP`, precision 4 or 2 at model+0x60888); which one this voice uses
is not yet read out.

## Decompilation

The arm64 library is imported in the Ghidra project as `/libsamsungtts.so` (image base 0x100000, so Ghidra addresses
are these plus 0x100000). All 122 `lpcnet::` functions are decompiled to `work/decomp/lpcnet.c`. Of them 78 are
called through the import table and can be hooked from `oracle.c`, including `run_frame_network`,
`run_sample_network`, `sample_from_pdf`, `compute_sparse_gru`, `compute_gru2`, `compute_mdense`,
`compute_activation`, `declicker` and `hf_tone_remover`.

## Why the vocoder is translated, not rewritten

Reading the decompilation showed that the vocoder's results depend on arm64 details a rewrite would have to
reproduce one by one: the sigmoid and tanh approximations use `frecpe` (the 8-bit reciprocal estimate) in their
4-wide loops and a true division, partly in double precision, for the leftover elements; matrix products use fused
`fmla`; float-to-int conversions saturate. The same would hold for the acoustic model, and the text front end is
about a million instructions. So the code is **translated mechanically from arm64 to C** (`tools/a2c/`), which is
exact by construction, and checked against the oracle like any port.

`tools/a2c/a2c.py` turns each original function into one C function over a register file (`cpu_t`). Registers are
C locals inside a function; only the argument registers cross a call. The library's data is mapped at run time at a
fixed address, so addresses in the generated code are constants and pointers are real pointers. (At first it was
mapped from a `libsamsungtts.so` beside the program; since 2026-10-07 the file is built into the program, see
below.) Calls through the import table are bound at translation time, to another translated function
or to a host function `H_<name>` (`a2c_host.c`). `a2c_rt.c` has the loader (segments, RELATIVE / ABS64 / GLOB_DAT /
JUMP_SLOT / RELR relocations), indirect-call dispatch, and `frecpe`/`frsqrte` and half-float conversion as the
architecture defines them. The maths library is translated too, from the emulator's arm64 `libm.so`, so `expf`,
`logf`, `cos` and the rest are the device's own.

**The generated C is a derivative of Samsung's library and is never committed**: it is produced locally in `work/`
from the user's own voice pack.

First result (a spike, since set aside): the vocoder alone, 167 functions, model loaded from the unscrambled voice file, fed
the 48 feature frames the oracle recorded for "Hi.": **44 of 44 audio frames identical to the engine's**. Not
translated: the half-precision (float16) variant of each kernel, which this voice does not use and which traps if
reached. Speed: 0.4 times real time at first, **3 times real time** (on a loaded i7-1355U laptop, one thread) after two
changes to the translator, with the output still identical: jump tables are recovered, so a computed jump no
longer makes every instruction a label; and whole-register vector operations are emitted as SSE/FMA intrinsics
(`Func.sse`) instead of per-lane C, which the compiler turned into stack traffic. `frecpe` on four floats is a
table gather for normal inputs and falls back to the general routine otherwise.

On the real engine, a hard five-line text (numbers, dates, abbreviations, nonsense words) raised **no C++
exception and created no thread** (`COUNT=1` with `__cxa_throw`, `__cxa_begin_catch`, `pthread_create` hooked).
So the translation need not implement unwinding or threads: both trap if reached.

### The whole engine

`tools/translate_engine.py` translates everything reachable from the `TTS_ENGINE_*` functions and the library's
own static initialisers: about 6,000 functions, 3.8 MB of arm64. Reachability follows direct calls, and data: an
address the code materialises (`adrp`+`add`, or a load from the GOT) makes the functions it points to roots, the
whole object if a symbol covers it, which is how virtual functions are found. ONNX Runtime, protobuf, abseil,
RE2 and the rest that came with it (0x1640000 to 0x2108000, and by name) are replaced by traps, and their static
initialisers are not run. The generated C goes to `work/engine/` (`gen_*.c`, and `inits.h` with the addresses of
the initialisers to run); `tools/a2c/build.py` compiles the pieces in parallel; `engine/main.c` is the oracle's
sequence of calls. `build.ps1` in the root does all of it. Host functions: about 135 of bionic's, in `tools/a2c/a2c_host.c` (LP64 `long`, 32-bit
`wchar_t`, UTF-8 multibyte functions, arm64 `va_list`, `struct stat` and `dirent` layouts).

### Result: the whole engine on Windows

`work/engine/engine.exe` (generated C + `tools/a2c` runtime + `engine/main.c` + `engine/fast.c`) takes text and
writes PCM. Against the oracle's recordings (`tools/compare_runs.py`): **7 of 7 utterances identical, sample for
sample** - the two plain sentences (`work/run_a`) and the five hard lines (`work/run_hard`: money, dates, times,
phone number, URL, abbreviations, Roman numerals, ordinals, nonsense words, an e-mail address). That is the
complete path: text normalisation, lexicon and letter-to-sound, the acoustic model, the vocoder.

What it took beyond the translator:

- Two functions of the Chinese front end are stubbed (0x1476e80, 72 KB, and 0x149d3b4, 491 KB of code): one clang
  process went to 4.9 GB on them and the machine ran out of memory. Needed again for Chinese, at lower optimisation.
- The static initialiser 0x161fbf4 is skipped: it only fetches ONNX Runtime's API table.
- `cpuinfo_initialize` is a host function that does nothing. Its one caller is `is_support_fp16()`, which chooses
  between float16 and float32 models; with cpuinfo's tables empty the answer is "no", the float32 path, which is
  what the emulator oracle runs too. **A real phone with half-precision arithmetic would take the float16 path
  and may not produce the same samples as this oracle.** Not checked.
- A bug of mine worth remembering: the printf stand-in's `PUT(*fmt++)` macro did not evaluate its argument when
  the output buffer was full, so a length-only pass looped forever.

### Speed

Measured on an i7-1355U laptop that throttles, so figures move by a factor of two between runs. Plain translation:
0.3 to 0.7 times real time on long sentences. Findings, in order of size:

- **Denormal numbers.** About 7% of the values entering the acoustic model's matrix products are denormal, and
  15% of the weight rows contain denormal weights (dead units). arm64 computes with them at full speed; an x86
  core takes a microcode assist of a hundred cycles or more per operation, and the laptop's efficiency cores are
  five times slower again (52 s against 10 s for one sentence when pinned to them). With denormals treated as
  zero (`--daz`: MXCSR DAZ and FTZ) the five hard utterances are **still identical** and take about half the time.
  That is an observation on five utterances, not a proof: a denormal can in principle be amplified later (a log,
  a division). `fast.c` also has an exact shortcut for the provable case (denormal input, accumulator at least
  2^-50, weight below 2^40: the sum cannot change).
- `gemm` (0x15e9c1c), the acoustic model's matrix-vector product: 2.3 G multiply-adds for 10.8 s of speech, and
  the model's 40 MB of weights do not fit any cache. `fast.c` runs four blocks of eight outputs at a time (eight
  streams alias in the cache: the blocks are a multiple of 4 KB apart), each output accumulated in the original
  order, so the result is identical.
- `lpcnet::compute_activation`: sigmoid and tanh eight wide in `fast.c`, same arithmetic per element.
- Left: the vocoder's dense and sparse-GRU kernels (0x15da6c0, 0x15dc540), about a quarter of the time; threads
  (the engine is single-threaded: the acoustic model and the vocoder could run side by side, and `gemm` could be
  split over cores without changing a result).

`fast.c` routines stand in front of the translation through `WRAPS` in `tools/translate_engine.py`; the
translated original stays as `O_<name>` and takes every case the fast version does not handle. A wrap changes
only the text of the function it wraps, so adding one recompiles one or two generated files, not all of them.
`--profile` on the engine samples the instruction pointer every millisecond and names translated functions;
host code is listed by image offset, which `tools/a2c/maplook.py` resolves through the linker map.

### An existing project

github.com/OnjLouis/samsungGalaxyVoices (GPL-2.0) is an NVDA add-on that runs the same Samsung libraries on
Windows under the Dynarmic JIT, with a voice manager for many voice packs. It is the reference for package names
and for how rate, pitch and stopping are driven. This project's aim, set by the user on 2026-10-06, is a SAPI voice
and speed.

### The SAPI voice

`engine/sstts.c` is the oracle's call sequence behind `sstts.h` (open, speak with an audio callback, stop,
rate/pitch). `TTS_ENGINE_VoiceControl(engine, pitch, volume, rate)` takes percentages (from the NVDA add-on's
host). Stopping: a flag makes `lpctron_is_stopping_callback` answer yes (wrapped in `sstts.c`) and the audio
callback calls `TTS_ENGINE_Stop`; `InputText` then returns 7 and no further audio arrives.

`sapi/samsung_sapi.cpp` is a raw COM `ISpTTSEngine`. One worker thread owns the engine for the life of the process
(64 MB stack, fast cores preferred, fast denormals on); `Speak` stays on SAPI's thread, feeds it one text fragment
at a time and takes audio from a queue bounded at two seconds. Rate -10..10 maps to 3^(r/10), pitch to 2^(p/24),
volume is applied to the samples. Word boundary events come from the engine's own word positions.

SAPI lists only voices registered under HKLM, which needs elevation: `sapi/install.ps1`. Registered per user
(plain `regsvr32`), the voice can still be opened by token id, which is how `sapi/test_sapi.ps1` tests it.

Measured (fast mode, `--fast`, with `sstts_prefer_fast_cores`): 50.6 s of speech in 8.75 s, 5.8 times real time;
first audio after 45 to 100 ms; engine opened in 0.2 s; through SAPI 7 of 7 utterances identical to the oracle;
stop in 212 ms through a SAPI purge with live audio (one measurement).

Known gaps: 32-bit hosts; only the pack's default speaker; one fragment is one engine utterance, so XML tags
split sentences; a stop that arrives before an utterance's first audio waits for that audio (see the stress
tests below).

### Other voice packs

`tools/catalogue.py` asks Samsung's store (`vas.samsungapps.com/stub/stubDownload.as`, with the device profiles
the NVDA add-on uses: SM-S921B / Android 34 and SM-G970F / 29) for all 57 known packs: 25 neural voices (`l`, `g`,
and the newer builds of some `f`/`m` codes) in 12 or so languages and the small "Classic" ones. All are offered.
Pack versions range over 3.1.23.51, 3.1.23.56, 3.1.25.0 to 3.1.25.4 and 3.1.27.1, and **each pack carries its own
build of `libsamsungtts.so`**: en_US_l03 (3.1.25.4), en_GB_l02 (3.1.25.0) and en_US_l04 (3.1.23.56) have three
different libraries.

**The one translated engine (3.1.25.4) runs the other packs' data.** `tools/fetch_voice.py` unpacks a pack into
the voice root; `engine --voice en_GB_l02` and `--voice en_US_l04` both speak, at 12 and 7.5 times real time. So a
voice centre only has to download data. Not checked: whether the 3.1.25.4 engine gives the same samples as each
pack's own build (the oracle would have to run that pack's library), non-English languages, the Classic packs.

The ending silence is `TTS_ENGINE_SetEndingSilence(engine, milliseconds)`, 500 by default; the SAPI voice sets
100 (`EndingSilenceMs` on the token overrides it).

### All of Samsung's packs, the voice centre and the installer (2026-10-06)

`tools/try_voices.py --all` fetched all 57 packs and ran the one translated engine (3.1.25.4) on a sentence in
each pack's language: **54 produce audio**, at 4.5 to 31 times real time. Not speaking: `es_us_l01` and
`es_us_g01` (`Initialize` fails) and `nl_nl_f00` (no audio, no error). "Speaks" means result 0 and audio came out;
nobody has listened to most of them, and none but en_US_l03 has been compared with an oracle. Results are in
`work/try_voices.tsv`; `tools/make_catalogue.py` turns them into `center/catalogue_more.inc`.

Things that had to change for the other packs:

- The stub filter matched library names anywhere in a symbol and so removed `fsa::...FSAMultiStore22Function...`
  ("re2"), which German and French need. In Samsung's own address range only `Ort*`, `onnx*` and `cpuinfo_*`
  symbols are stubbed now.
- `SetVoice` mode 3 instead of 0: mode 0 wants `smt_<voice>.cfg` files for the `f` and `m` packs, mode 3 takes the
  folder layout for every type. en_US_l03 gives the same samples in both (7 of 7 against the oracle).
- Several voices in one process: `sstts_select` keeps up to three engine objects and drops the least recently
  used (`TTS_ENGINE_Finalize`, `TTS_ENGINE_Delete`); loading a voice takes 0.1 to 0.25 s.
- A fault in the engine no longer ends the process: `a2c_trap` raises a structured exception when
  `a2c_trap_raises` is set, and `sstts_speak` / `sstts_select` catch it (and access violations), return -2 and
  abandon that engine object.

A voice's own name is in its `cfg` after the key `name` (Stephanie, John Lano, Julia, Lisa, Amy Green, Chris,
Marie, Jan, Louise, Valentin, Sandra, David, Angela, Andrea, Meghana); `tools/fetch_voice.py` writes it to
`info.txt` in the voice folder, which the SAPI registration reads. `regsvr32 samsungtts_sapi.dll` makes one token
`SamsungTTS_<code>` per folder under `data/voice` and removes tokens of packs that are gone.

`center/voice_center.cpp` is the standalone voice centre (`SamsungVoices.exe`, plain Win32 controls): list,
download from Samsung over WinHTTP (only hosts under samsungapps.com), unpack with Windows' `tar`, write
`info.txt`, re-register through an elevated `regsvr32`, remove, play a sample through SAPI. The user has run it
for real (see Next).

`installer/samsungtts.iss` (Inno Setup 6, at `D:/Program Files/Inno Setup 6`) builds
`dist/SamsungTTS-Setup-<version>.exe`: the DLL (registered), the voice centre, `libsamsungtts.so` and `libm.so`, with
`data` writable by users. The user has installed with it. It contains Samsung's library and its translation, so it
is for the builder's own machines.

The reference add-on blocks en_us_l03 and en_us_g02 in their 3.1.25.4 builds as unable to generate audio in real
time; here they run at about 5 times real time (in fast mode).

### The engine's data is built in, and the driver (2026-10-07)

The translated code needs the library's data at run time: tables, strings, vtables, the relocation and symbol
tables the loader works from. Until now that came from `libsamsungtts.so` and `libm.so` in `data\` beside the
DLL, which had to be exactly the files the code was translated from. Now `tools/translate_engine.py` writes
`gen_images.c` with the rest of the generated C: an assembler `.incbin` of the two files, whole, into the
program's read-only data, and `a2c_load` maps from memory. So the code and its data cannot be two different
builds, nothing has to be found on disk, and a DLL is complete in itself: 47 MB instead of 10 plus a 37 MB file.
The files are still needed for building. (Putting only the data segments in would be smaller; the whole file
is in because the translated code may read constants out of the code segment, and nobody has checked that it
never does.) `sstts_init` takes no paths any more, the programs under `work\engine` lost their first two
arguments, and voice tokens no longer carry `EngineLib` and `LibM`.

`driver/samsungtts.c` is the engine as a DLL for other people's programs (`samsungtts.dll`, interface in
`driver/samsungtts.h`, documentation in `driver/README.md`): one thread owns the engine, `samsungtts_speak`
hands it a text and waits while the audio function is called on that thread, callers take turns in order,
`samsungtts_stop` works from any thread. `tests/driver_test.py` drives it through `ctypes`: the samples are
those of `engine.exe` for two voices, both kinds of stop end the call (0.1 s from another thread), a missing
voice is an error with a message, two threads at once each get their own voice's audio.

The engine opens its files with the C library, which on Windows means the system's code page; the host
functions (`H_fopen`, `H_open`, `H_stat`, `H_access`, `H_opendir`) now take the guest's paths as UTF-8 and use
the wide functions, so a voice folder under a profile with any characters in its name works (tested with
`Zoë Müller 声`). An add-on lives in such a place.

## Stress tests and what they found (2026-10-07)

Two programs under `tests/`. `soak.c` drives the engine through `sstts.h` the way a screen reader does: random
lines of a corpus, most of them stopped part way, the voice changing every few utterances, rate and pitch
varying. It checks that a line spoken in full gives the same samples as the first time it was spoken with that
voice, that a stop stops, and it prints the process's memory, its handles and what the engine holds, once a
minute. `--sweep` instead speaks every line once with every voice. `sapi_stress.cpp` loads the SAPI DLL without
registering it (a stand-in token and site take SAPI's place), and speaks from several threads with aborts timed
to the millisecond. Corpora are in `tests/corpus/` (`make_soak_corpus.py` writes the two generated ones).

A fault report names the place and the guest's call stack, which is what made the rest possible:
`access violation writing 204c681f004 in function 1245af8 (image 1) +80 STRNCPY; in TTS_ENGINE_InputText, called
from 1245af8 < 13c4b0c < 13c6a18 < ...`. The guest's registers are C locals, so its stack is recovered by
unwinding the host stack and keeping the frames that are translated functions (`a2c_backtrace`).

### What broke

**One word could take every voice down.** The first long run (twelve voices, twelve minutes) had three faults,
all the Hindi voice on the same line, and the process grew from 99 to 848 MB. Followed up:

1. `CHindi::G2P` (0x13c6a18) cuts a word at its apostrophes and passes the pieces to `CHindi::_SplitWord`
   (0x13c4b0c) with the length that is left. After a second apostrophe it subtracts all that has been consumed
   from what was left after the first: for "O'Neil's" the pieces are "O" (length 8), "Neil" (6) and "s" (-1).
   `_SplitWord` then reads four thousand million letters and writes six bytes for each into a 1024-entry array
   on G2P's stack. The function is the same, instruction for instruction, in the Hindi pack's own engine build
   (3.1.27.1), so this is Samsung's bug and not the translation's or a mismatch of versions. Not confirmed on a
   real engine: the oracle was not run on it.
2. The runaway write went up the guest's stack, **off its end and on into whatever the system had mapped next
   to it**, which was often the heap. What happened next depended on what it hit: the voice not loading again
   (`TTS_ENGINE_New` faulting inside the heap), another voice not loading (`voice_info::voice_info` reading a
   pointer made of text), every utterance of every voice faulting in `languageIDToString`. That is "the voices
   break after a while": one such word, and nothing speaks until the program is restarted.
3. Each fault abandoned the voice with its memory, 100 to 300 MB a time. The installed DLL, given a corpus of
   seven lines of which three fault, in three minutes: 35 utterances silent, **5.6 GB** of private memory, and
   a first audio that took 28 s while the machine paged.

`CItalian::IntervocalicSybilantsPostG2P` (0x13d33e0) is the second: it writes the names of a word's phonemes
into 100 bytes on its stack without counting them, so a word of some thirty phonemes ("A" sixty-four times) runs
over, and the stack protector stops the engine. A clean stop, but the voice was lost with its memory each time.

Nothing else faulted in a sweep of all 57 packs over `tokens.txt` (39 lines of apostrophes, addresses, numbers,
other scripts, emoji, long runs of one letter).

**Long runs of Latin letters** are the third thing, found by `torture.txt`: `"x"` 1100 times made the Mandarin
voice fault in `CCNChinese::_Normalize_AddNewWords_LATIN` (0x13a4dd0), which spells such a word out, letter by
letter, into 1024 bytes on its stack and goes on into its caller's frame. `tests/token_limits.py --raw` then
gave every voice runs of 33 to 1100 of several characters, stopped at the first audio so that only the front
end ran:

| voices | shortest run that faults |
|---|---|
| Thai (`th_TH_f00`) | 64 of "w" or "W", 128 of "x" or "q"; 18 of the 48 lines fault |
| Mandarin (`zh_CN_l02`, `zh_CN_f00`), tried by hand with "x" and "w" only | 64 of "w" (the stack protector), 200 of "x" |
| `zh_TW_f00`, `zh_HK_f00`, the same way | 64 of "w", 128 of "x" |
| the 46 other packs that speak, and `nl_NL_f00` | none: runs of 1100 go through |

The survey was stopped by the system, which was short of memory, after 51 packs in alphabetical order: it did
not reach the six Chinese ones, and `zh_CN_g02` and `zh_CN_m00` have not been tried at all without the cut
described below. Thai faults in another function (`UTF8_to_UNICODE`, from a pointer that was overwritten).

### What was done

- **A fence round the guest's stack** (`a2c_cpu_new`): a megabyte that cannot be touched on either side, so
  code that runs off the stack faults there and then. The same Hindi fault is now clean: the voice is reloaded
  and every other voice goes on working.
- **Guards** (`engine/guards.c`, put in front by `WRAPS`): `_SplitWord` is given the length of the string when
  the length it gets is more than the 1024-byte word buffer can hold; the Italian function has a word that
  would not fit cut short for the length of the call. For every call the original gets through they change
  nothing, and the seven oracle utterances are still identical.
- **Over-long runs are cut before the engine sees them** (`cut_long_tokens` in `sstts.c`): a run of ASCII
  letters, digits and signs longer than the voice's language can take is given to the engine with a space every
  so many characters, and word positions are mapped back to the caller's text. The limit is per language, from
  the survey above (32 for Chinese and Thai, half of the shortest run that faulted; 1000 for the rest, which took 1100). Text in other scripts is not touched. This one does change what is
  spoken for such a run, in the languages it applies to: pieces instead of a crash.
- **A fault no longer costs the voice's memory.** After a fault the engine object is torn down like any other
  (`drop` in `sstts.c`), if the guest's heap passes `HeapValidate`, and abandoned only if the teardown faults
  too. Before the fence this had seemed to corrupt tables shared by all voices; that was the stack overrun,
  not the teardown. With the two engine bugs put back on purpose (`tests/guards_off.c`, `tests/corpus/faults.txt`):
  abandoning, 4 faults cost 1.4 GB; tearing down, 37 faults in 400 utterances and memory stayed where it was.
  `SSTTS_ON_FAULT=abandon` gives the old behaviour.
- **The guest's memory comes from a heap of its own** (`a2c_host.c`), not the host program's, and a free of
  something that is not an allocated block is ignored instead of reaching the heap.
- **The fortified C library functions check** (`__strcpy_chk` and the rest): they trap where the phone's would
  abort, instead of copying past the end.
- **A stack overflow on the host stack is a fault like any other** (128 KB kept in hand for dealing with it,
  `_resetstkoflw` afterwards), not the end of the process. Tested with a guard that recurses for ever.
- In the SAPI DLL: a fault or a voice that does not load is written to the debugger output
  (`OutputDebugString`, with the function and call stack); `Speak` looks for an abort every 20 ms even when no
  audio is coming; a stop that arrives while the voice is loading is no longer forgotten; callers take turns in
  the order they came (a plain mutex let one caller starve another for half a minute in the two-thread test).

### What the tests say now

Measured on the build of 5:34 pm, which has everything above except the cut of over-long runs, and still took
the engine's data from a file:

| | |
|---|---|
| `soak --sweep`, all 57 packs, `tokens.txt` | no fault. `es_US_g01` and `es_US_l01` do not load and `nl_NL_f00` gives no audio, as before |
| `soak`, 20 minutes, 12 voices changing every 5 utterances, 70% stopped, rate and pitch varying | 1500 utterances, 4435 s of audio at 4.0 times real time, nothing wrong, 567 MB private at the end, 56 handles throughout, no voice abandoned |
| `soak_guards_off`, 5 minutes, `faults.txt`, 5 voices | 774 utterances, 57 faults, each torn down and reloaded: no voice abandoned, 595 MB, no line's audio changed |
| `soak_overflow`, 60 utterances | 40 host stack overflows, the process went on, 322 MB |
| `sapi_stress`, 3 callers, 10.6 minutes, 6 voices | 465 calls of which 299 aborted, nothing wrong, 440 MB, slowest abort 479 ms |
| `sapi_stress`, 1 caller, 4.4 minutes, 80% aborted | 318 calls, slowest abort once audio was flowing 186 ms; slowest that came before the first audio 1989 ms |
| `sapi_stress`, 2 callers, 3 minutes, `faults.txt` | 506 calls, none silent, 539 MB. The DLL as it was before, same command: 35 silent, 5574 MB |
| `soak --sweep --rate 300`, `torture.txt`, 6 voices, 9 minutes each | between 9 and 32 of the 42 lines per voice in that time; the only faults were the Mandarin voice on long runs of Latin letters |

On the final build: the seven oracle utterances are identical; `tests/driver_test.py` passes its 19 checks,
among them two hundred "w" with a Mandarin and the Thai voice; `soak --sweep` spoke 300 "x" and 200 "w" with
`zh_CN_l02`, `th_TH_f00` and `zh_HK_f00`; `sapi_stress` with 2 callers for 2 minutes on `faults.txt` with four
voices, one of them Mandarin: 525 calls, nothing wrong, 570 MB. **The long runs in the table were not repeated on
the final build.**

Limits of this: the corpora are English with a few lines in other scripts, so the front ends of the other
languages have seen little of their own languages' odd cases; the longest run is twenty minutes; the reference
for "the same audio" is the engine's own first rendering, not Samsung's engine.

A stop that arrives **before the first audio** of an utterance waits for it: the engine looks for a stop only
between pieces of audio and between vocoder frames, and computes the acoustic model for a whole sentence first.
For a sentence of 485 characters of numbers that is 2.5 s, 81% of it in `gemm`. Not fixed.

## Next

1. Done on 2026-10-06: the installer and the voice centre were run for real by the user (one voice installed,
   en_AU_f00) and work. A voice that came out as Stephanie until NVDA was restarted was NVDA still holding the
   first, single-voice build of the DLL from the night before; `sapi/test_live_install.ps1` shows the current
   build speaks a voice installed while it is loaded. The voice centre also plays a recorded sample of each
   voice before download (`tools/make_samples.py`, bundled by the installer).
2. The NVDA add-on is shelved by the user for now. If it is picked up: NVDA 2026.2 here is 64-bit, so the engine
   can load in-process from a plain DLL exporting `sstts.h`.
3. A larger corpus: exact mode against the oracle, fast mode against exact mode; other voices against their
   own engine builds; and the Hindi word with two apostrophes on the real engine, to see it fail there.
4. The long stress runs again on the final build, and `tests/token_limits.py` for the six Chinese packs it did
   not reach, with and without `--raw`.
   Then sweeps with text in each voice's own language: the crashes found were all in language front ends, on
   input an English corpus happened to contain.
5. A stop during the acoustic model (see above).
6. Why es_us_l01/g01 and nl_nl_f00 do not speak; the two stubbed Chinese functions (Chinese spoke without them
   on every line tried).
7. More speed: the vocoder kernels in AVX2, threads.
8. 32-bit SAPI clients need an out-of-process host, which would also put the engine's faults in a process of
   their own.
