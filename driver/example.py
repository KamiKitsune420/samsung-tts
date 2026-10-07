"""Speak a text into a WAV file with samsungtts.dll, from Python (64-bit), with nothing but the standard library.

    python example.py <voice root> <voice> "text to speak" out.wav [rate] [pitch]

    python example.py C:\\voices en_US_l03 "Hello from Samsung's engine." hello.wav

<voice root> holds one folder per voice pack (README.md says where they come from). samsungtts.dll is looked for
beside this file. The same calls work from an NVDA add-on's synth driver: there the audio function would hand
the samples to nvwave.WavePlayer.feed instead of collecting them.
"""
import ctypes
import os
import sys
import wave

# int audio(void *user, const short *pcm, int samples, int sample_rate, int word_offset, int word_length)
AUDIO = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.POINTER(ctypes.c_short), ctypes.c_int, ctypes.c_int,
                         ctypes.c_int, ctypes.c_int)


def load(path=None):
    dll = ctypes.CDLL(path or os.path.join(os.path.dirname(os.path.abspath(__file__)), "samsungtts.dll"))
    dll.samsungtts_speak.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int,
                                     ctypes.c_int, AUDIO, ctypes.c_void_p]
    dll.samsungtts_speak.restype = ctypes.c_int
    dll.samsungtts_sample_rate.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    dll.samsungtts_sample_rate.restype = ctypes.c_int
    dll.samsungtts_ending_silence.argtypes = [ctypes.c_int]
    dll.samsungtts_last_error.restype = ctypes.c_char_p
    dll.samsungtts_version.restype = ctypes.c_char_p
    return dll


def speak(dll, voice_root, voice, text, rate=100, pitch=100, volume=100):
    """Returns (result, sample rate, samples as bytes, [(sample index, byte offset in the UTF-8 text, byte length)])."""
    pieces, words, seen = [], [], {"rate": 0, "samples": 0, "word": -1}

    def on_audio(user, pcm, samples, sample_rate, word_offset, word_length):
        seen["rate"] = sample_rate
        if word_offset >= 0 and word_offset != seen["word"]:
            seen["word"] = word_offset
            words.append((seen["samples"], word_offset, word_length))
        pieces.append(ctypes.string_at(pcm, samples * 2))       # the memory is the engine's: copy it now
        seen["samples"] += samples
        return 0                                                # anything else stops the speech

    callback = AUDIO(on_audio)      # keep a reference for as long as the call lasts
    # Strings are UTF-8, paths included.
    result = dll.samsungtts_speak(voice_root.encode("utf8"), voice.encode("utf8"), text.encode("utf8"), rate, pitch,
                                  volume, callback, None)
    return result, seen["rate"], b"".join(pieces), words


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    root, voice, text, out = sys.argv[1:5]
    rate = int(sys.argv[5]) if len(sys.argv) > 5 else 100
    pitch = int(sys.argv[6]) if len(sys.argv) > 6 else 100
    dll = load()
    result, sample_rate, pcm, words = speak(dll, root, voice, text, rate, pitch)
    if result < 0:
        sys.exit("samsungtts %s: result %d: %s" % (dll.samsungtts_version().decode(), result,
                                                   dll.samsungtts_last_error().decode("utf8", "replace")))
    with wave.open(out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate or 24000)
        w.writeframes(pcm)
    data = text.encode("utf8")
    print("%s: %.2f s at %d Hz, %d words: %s" % (out, len(pcm) / 2 / (sample_rate or 24000), sample_rate, len(words),
                                                  " ".join(data[o:o + n].decode("utf8", "replace") for _, o, n in words)))


if __name__ == "__main__":
    main()
