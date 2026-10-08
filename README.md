# Samsung TTS on Windows

**Samsung's text-to-speech voices on Windows, running as native code: as 64-bit SAPI 5 voices, and as a plain DLL
to build your own things on.** Samsung's arm64 engine library is translated to C ahead of time on the builder's
machine and compiled, so nothing is emulated while a voice speaks. The voices themselves are downloaded from
Samsung's store.

This repository holds the tools and the glue. It holds nothing of Samsung's: no engine, no voice, no translated
code (see [What is not here](#what-is-not-here)).

## Download

Both packages are on the [releases page](https://github.com/KamiKitsune420/samsung-tts/releases): the installer
for the SAPI voices (`SamsungTTS-Setup-<version>.exe`) and the driver (`SamsungTTS-Driver-<version>.zip`). They
need 64-bit Windows 10 or 11 and a processor with AVX2 (2013 or later); voices are downloaded separately, by the
voice centre or as [the driver's documentation](driver/README.md) describes. Read
[What is not here](#what-is-not-here) first: the DLLs in them contain Samsung's engine.

## The two packages

`build.ps1` makes both.

**The SAPI voices** (`dist\SamsungTTS`, and an installer made with Inno Setup):

- `samsungtts_sapi.dll`: a SAPI 5 engine. One voice token per installed voice pack, word events, rate, pitch,
  volume, stop.
- `SamsungVoices.exe`: the voice centre. Lists Samsung's packs, downloads one from Samsung, registers it,
  removes it, plays a sample. Plain Win32 controls, so screen readers read it without help.

**The driver** (`dist\SamsungTTS-Driver`, zipped by `build.ps1 -Package` together with the source):

- `samsungtts.dll`: the engine behind seven C functions. Give it a voice folder and a text; it calls back with
  16-bit audio as it is produced. For an add-on, a game, a converter: whatever you like.
- `samsungtts.h`, [driver/README.md](driver/README.md) (the driver's documentation) and `example.py`, which
  speaks into a WAV file from Python with `ctypes`.

In both, the engine is inside the DLL, code and data: no other file of the engine's goes with it. 54 of
Samsung's 57 voice packs speak with it (not `es_US_l01`, `es_US_g01`, `nl_NL_f00`).

## What is measured

Against Samsung's real engine running on an Android emulator ([the oracle](docs/notes.md)), voice pack
`en_us_l03` 3.1.25.4:

| | |
|---|---|
| Translated engine, exact arithmetic | 7 of 7 test utterances identical, sample for sample (two plain sentences; five hard lines with money, dates, a phone number, a URL, abbreviations, Roman numerals, nonsense words) |
| Fast mode (denormal numbers treated as zero), which both DLLs use | the same 7 of 7 identical. An observation on seven utterances, not a guarantee |
| Through the SAPI voice into WAV files | 7 of 7 identical (measured on 2026-10-06, before the changes below) |
| Through the driver DLL from Python | the same samples as the command-line program, two voices |
| Speed, fast mode, i7-1355U laptop, one thread | about 4 to 6 times real time for this pack, 4.5 to 31 for the others. First audio 45 to 100 ms after the request. The laptop throttles, and figures move by a factor of two between runs |
| Engine start | 0.2 to 0.4 s; another voice loads in 0.1 to 0.4 s, and three stay loaded |

Only `en_us_l03` has been compared with an oracle. The other packs speak (result 0 and audio); nobody has
listened to most of them, and each was made for its own build of the engine, not this one.

### Stress tests (2026-10-07)

What the tests found, and what was done about it, is in [docs/notes.md](docs/notes.md). In short: one word
could take every voice down until the program was restarted (Samsung's Hindi front end miscounts a word with two
apostrophes and writes off the end of the engine's stack into the memory next to it); an Italian word of thirty
phonemes overran a buffer; Chinese and Thai overrun theirs on sixty-four Latin letters in a row; and every fault
left 100 to 300 MB behind. On three lines that fault out of seven, the DLL as it was reached 5.6 GB in three
minutes with 35 utterances silent.

After the fixes, on the build of 5:34 pm (before the engine's data was built into the DLLs):

| | |
|---|---|
| Every pack, 39 lines of awkward tokens each (`soak --sweep`) | no fault in any of the 54 that speak |
| Engine, 20 minutes, 12 voices, 70% of utterances stopped, rate and pitch varying | 1500 utterances, nothing wrong: the same samples every time a line was spoken in full, 567 MB at the end, 56 handles throughout |
| Engine with the two guards taken off, 5 minutes | 57 faults in 774 utterances; each cost its utterance and nothing else: no voice abandoned, 595 MB, the other voices' audio unchanged |
| A host stack overflow inside the engine, 40 times | the process went on each time |
| SAPI DLL, 3 callers at once, 10 minutes, 64% of calls aborted | 465 calls, nothing wrong, 440 MB, slowest abort 479 ms |
| SAPI DLL, 1 caller, 80% aborted | an abort once audio is flowing: 186 ms at worst. One that arrives before the first audio of a very long sentence waits for it: up to 2 s (not fixed) |
| SAPI DLL on the lines that used to fault, 3 minutes | 506 calls, none silent, 539 MB |

On the final build (data built in, long runs of letters cut, UTF-8 paths): the 7 oracle utterances identical; the
driver test's 19 checks pass; the SAPI DLL from 2 callers for 2 minutes on the faulting lines plus Chinese, 525
calls, nothing wrong. **The long runs above were not repeated on the final build**, and the survey of how long a
run each voice takes reached 51 of the 57 packs before it was stopped (the machine ran short of memory); the
six it did not reach are the Chinese ones, four of which had been tried by hand.

Not tested: a real phone as the reference, 32-bit programs (both DLLs are 64-bit only), sessions of many hours,
text in most voices' own languages, screen readers other than by the author's daily use.

## Building

Needs Windows 11 x64, Python 3 with `capstone`, Visual Studio Build Tools with clang-cl, and under `work\`
(which git ignores):

- `work\l03`: voice pack `com.samsung.SMT.lang_en_us_l03` version 3.1.25.4, unpacked. Its
  `lib\arm64-v8a\libsamsungtts.so` is the engine that gets translated; the addresses in this repository are
  that build's. `python tools\fetch_voice.py en_us_l03 --keep-apk` downloads the pack Samsung offers now
  (check the version it prints) and leaves the APK in `work\apk` to unzip.
- `work\sys\libm.so`: an arm64 Android `libm.so`, from a device or an emulator image.

```
powershell -ExecutionPolicy Bypass -File build.ps1              both packages, into dist\
powershell -ExecutionPolicy Bypass -File build.ps1 -Installer   and dist\SamsungTTS-Setup-<version>.exe (Inno Setup 6)
powershell -ExecutionPolicy Bypass -File build.ps1 -Package     and dist\SamsungTTS-Driver-<version>.zip
powershell -ExecutionPolicy Bypass -File sapi\install.ps1       register the development copy (asks for elevation)
```

The first build translates the library (two minutes) and compiles 56 MB of generated C (seven minutes here with
two compilers at once, a gigabyte or more each: `-Jobs` sets how many). Both library files are built into the
programs, so they are needed for building and not afterwards. Run the builds from PowerShell: from Git Bash the
Visual Studio environment comes out incomplete.

## Testing

```
python tools\fetch_voice.py en_gb_l02                                  a voice pack into work\voice
work\engine\engine.exe work\voice tests\corpus\hard.txt out --fast    speak a corpus into out\
python tools\compare_runs.py work\run_hard out                         against the oracle's recording
powershell -ExecutionPolicy Bypass -File tests\build.ps1               the stress tests
python tests\driver_test.py                                            the driver DLL, through ctypes
```

- `tests\soak.c` drives the engine for a long time the way a screen reader does (short texts, most of them cut
  off, voices changing) and checks that the same text gives the same samples every time, that stops stop, and
  that memory and speed stay where they were. `--sweep` goes through a corpus once with every voice and names
  the function and call stack of anything that faults.
- `tests\sapi_stress.cpp` does the same to the SAPI DLL from several threads, without registering it, so it can
  run beside an installed copy that a screen reader is using.
- `tests\token_limits.py` finds how long a run of letters each voice's front end takes.
- `tests\guards_off.c` and `tests\guards_overflow.c` make the engine fault on purpose, to test what happens then.
- `tools\oracle\` runs Samsung's own engine on an Android device or emulator and records what it says: the
  reference for `tools\compare_runs.py`.

## How it works

```
voice pack APK (the builder's own)
  libsamsungtts.so  --tools/a2c-->  C, one function per original function  --clang-->  native code
                    and the file itself, built in beside the code: its tables, strings and vtables
  cfg, lng, *.ivc   the voice: read at run time, as on the phone
```

- `tools/a2c/` translates arm64 machine code to C: registers are C locals, the library's data is mapped at a
  fixed address, calls into the C library go to host functions (`a2c_host.c`). Vector instructions become SSE
  and FMA intrinsics with arm64's semantics. `tools/translate_engine.py` is the recipe for this engine: what to
  translate, what to stub, which functions get a host function in front.
- `engine/` is the engine behind a small C interface (`sstts.h`); `fast.c`, hand-written AVX2 versions of the
  hottest routines that compute the same values in the same order per output; `guards.c`, corrections in front
  of engine functions that run off the end of their buffers on some input; and the command-line program.
- `sapi/` is the SAPI 5 engine DLL and its registration; `driver/` the driver DLL; `center/` the voice centre;
  `installer/` the Inno Setup script.
- `tools/` besides: the store catalogue and pack download (`catalogue.py`, `fetch_voice.py`), trying every pack
  (`try_voices.py`, whose results `make_catalogue.py` and `make_samples.py` turn into the voice centre's list and
  samples), the voice file reader (`ivc.py`).

The translation is exact, so the engine's own bugs come with it. On a phone a crash in the engine takes down the
TTS service, which Android starts again; here the engine lives inside the program that is speaking. So every
call into it is made with faults caught (the utterance is lost, the voice is reloaded, the program goes on), its
stack and heap are kept apart from the host's, and the crashes the stress tests found are corrected at their
source. [docs/notes.md](docs/notes.md) has the details and addresses.

## What is not here

Nothing of Samsung's: no engine, no voice, no decompiled or translated code, no recordings. `work/` and `dist/`
are ignored by git and hold the downloaded packs, the generated C, the built DLLs and the samples.

**The built DLLs are another matter. Each contains Samsung's engine**: the library's code translated mechanically
to native code, and the library file itself for its data. The installer and the driver package are therefore
copies of Samsung's software in another form, and nobody here has Samsung's permission to pass them on. Whether
to publish them, and whether running a voice this way is permitted at all, is for whoever does it to judge under
Samsung's terms and their own law. The voice centre downloads voice data from Samsung's store at the user's
request, and only from hosts under `samsungapps.com`. The maths library built in beside the engine is Android's
(bionic `libm`), under the BSD-style licences in [licenses/bionic-libm-NOTICE.txt](licenses/bionic-libm-NOTICE.txt)
(the notice file of the Android Open Source Project's main branch), which goes into both packages.

## Related

[samsungGalaxyVoices](https://github.com/OnjLouis/samsungGalaxyVoices) is an NVDA add-on that runs the same
libraries under an arm64 JIT, with a manager for many voices; it is the source of the package list and of how
rate, pitch and stopping are driven. This project differs in translating ahead of time (for speed), in being a
SAPI voice, and in offering the engine as a driver.
