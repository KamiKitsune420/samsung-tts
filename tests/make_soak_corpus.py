"""Write the generated corpora for tests/soak.c and tests/sapi_stress.cpp.

    python tests/make_soak_corpus.py

tests/corpus/soak.txt: the kind of text a screen reader sends. Short interface strings, single characters,
punctuation alone, numbers, paths, code, other scripts, emoji, and a few very long lines, on top of hard.txt and
plain.txt.

tests/corpus/torture.txt: text meant to break a front end. Tokens thousands of characters long, deep nesting,
hundreds of apostrophes, commas or sentences in one line, control and invisible characters. It is a long time
of speech: use it with soak --sweep --rate 300 on a few voices at a time.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
TEXT = r"""OK
Cancel
File
Edit menu
a
b
Z
7
.
?
...
!!!
,
-
Desktop, list
Recycle Bin, 1 of 12
checked
not checked
button
link, visited
heading level 2
Save changes to Untitled before closing?
C:\Users\User\Documents\GitHub\samsung-tts\README.md
https://github.com/OnjLouis/samsungGalaxyVoices/blob/main/readme.md#installation
12:45 PM, Wednesday, October 7, 2026
Battery 83 percent, plugged in, charging
3.14159265358979323846264338327950288419716939937510
1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20
void sstts_stop(void) { g_stop = 1; }
for (int i = 0; i < n; i++) x[i] = y[i] * 2.5f + 0x1p-50f;
{"name": "value", "list": [1, 2, 3], "nested": {"a": null, "b": true}}
<html><body><p class="x">Hello &amp; goodbye</p></body></html>
AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
ha ha ha ha ha ha ha ha ha ha ha ha ha ha ha ha ha ha ha ha ha ha ha ha
xzqwvkj bcdfgh mnprst wxyz qqqq zzzz jjjj
The   quick     brown		fox.
Line one;line two;line three:line four
"Quoted," she said, 'and nested "quotes" too.'
It's 5 o'clock — or isn’t it? “Curly quotes” and… an ellipsis.
Emoji: 😀 🎉 👍🏽 ❤️ and symbols ™ © ® € £ ¥ § ¶ † ‡ • →
Café naïve résumé jalapeño Zürich Ångström Søren Dvořák
Привет мир, こんにちは世界, 你好世界, 안녕하세요, مرحبا, שלום
1,000,000 + 2.5e10 - 0.001 = ? 50% of $99.99 is $49.995; 3/4 > 2/3.
Call +1 (555) 010-9999 ext. 42 before 11/30/26, ref. #A-1138.
I.B.M., U.S.A., e.g., i.e., etc., vs., approx., Dept., St. John St., Dr. Dr. Mr. Mrs. Ms.
WWW HTTP SQL JSON NVDA SAPI TTS LPCNet ONNX GPU CPU RAM USB HDMI
Wait. What? No! Yes... Maybe; perhaps: okay, fine.
The engine is single-threaded, not re-entrant and wants a deep stack, so one worker thread owns it for the life of the process. Speak hands the worker one piece of text at a time and stays on the caller's thread itself: it takes the audio from a queue, writes it to the site, raises word events and watches for an abort.
It was the best of times, it was the worst of times, it was the age of wisdom, it was the age of foolishness, it was the epoch of belief, it was the epoch of incredulity, it was the season of Light, it was the season of Darkness, it was the spring of hope, it was the winter of despair, we had everything before us, we had nothing before us, we were all going direct to Heaven, we were all going direct the other way.
Call me Ishmael. Some years ago, never mind how long precisely, having little or no money in my purse, and nothing particular to interest me on shore, I thought I would sail about a little and see the watery part of the world. It is a way I have of driving off the spleen and regulating the circulation."""


def main():
    lines = []
    for name in ("hard.txt", "plain.txt"):
        lines += open(os.path.join(HERE, "corpus", name), encoding="utf8").read().splitlines()
    lines += TEXT.split("\n")
    lines.append("Supercalifragilisticexpialidocious " * 12)
    lines.append("word " * 400)
    lines.append("1234567890" * 30)
    lines.append(", ".join(str(i) for i in range(1, 120)))
    lines = [l for l in lines if l.strip()]
    with open(os.path.join(HERE, "corpus", "soak.txt"), "w", encoding="utf8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("soak.txt: %d lines" % len(lines))

    # sizes straddle the buffer sizes a front end is likely to have: 100, 256, 1024 and 4096 bytes
    t = ["x" * 1100, "A" * 300, "Supercalifragilistic" * 60, "7" * 1100, "word " * 300,
         "(" * 1100 + "deep" + ")" * 1100, "," * 1100, "a, " * 400,
         "'".join(["abcde"] * 220), "'" * 1100, "it's " * 150, "O'Neil's d'Artagnan's rock'n'roll's " * 20, "aaaa'" * 250,
         "1,234,567.89 " * 100, "https://example.com/" + "path/" * 200 + "?q=" + "a%20b" * 20,
         "https://example.com/" + "p" * 2100, chr(0x1F600) * 300,
         ("abc " + chr(0x4F60) + chr(0x597D) + " " + chr(0x41F) + chr(0x440) + " ") * 100,
         "Yes. No. " * 150, "a-b-" * 300, "A.B." * 300, "<b>" * 400, "\x01\x02\x03 control \x1b[0m characters \x7f here",
         "combining " + "e" + chr(0x301) * 200 + " marks", "zero" + chr(0x200D) * 200 + "width",
         chr(0x202E) + "right to left" + chr(0x202C), chr(0xFFFD) * 100 + " replacement", "\t" * 50 + "tabs" + "\t" * 50,
         "Mr. " * 150, "e.g. i.e. etc. " * 60, "12:34:56 " * 60, "$" * 1100, "9" * 30 + "." + "9" * 30, "-" * 1100,
         "_" * 1100, "a" + "b" * 98, "a" + "b" * 99, "a" + "b" * 100, "a" + "b" * 255, "a" + "b" * 256, "a" + "b" * 1023,
         "a" + "b" * 1024]
    with open(os.path.join(HERE, "corpus", "torture.txt"), "w", encoding="utf8", newline="\n") as f:
        f.write("\n".join(t) + "\n")
    print("torture.txt: %d lines" % len(t))


if __name__ == "__main__":
    main()
