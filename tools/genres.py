"""Writes one 8-bar .mid per genre with its basic ingredients (drums, bass, chords, melody), each on its own track,
so you can open it in Local MIDI Studio (Open MIDI...), mute/solo tracks and hear how the genre is built.

    python tools/genres.py [output folder]      default: Documents/LocalMIDIStudio/Genres

Patterns are 16 steps per bar (16th notes). Drums: x = accent, o = normal, . = rest (General MIDI notes, channel 10).
Bass/melody: 1 3 5 7 8 = root, third, fifth, seventh, octave of the bar's chord; a note lasts until the next one.
Chords: x = play the chord. Swing delays every second 16th by that fraction of a 16th.
"""
import pathlib
import struct
import sys

KICK, SIDESTICK, SNARE, CLAP, HAT, OPEN_HAT, RIDE = 36, 37, 38, 39, 42, 46, 51

GENRES = {
    "house": dict(bpm=124, swing=0.0, chords=["Am7", "Fmaj7", "C", "G"], drums={
        KICK: "x...x...x...x...", CLAP: "....x.......x...", HAT: "oooooooooooooooo", OPEN_HAT: "..x...x...x...x."},
        bass="..1...1...1...1.", comp="..x...x...x...x.", melody="5...3...1.....3."),  # offbeat bass and hats
    "hiphop": dict(bpm=90, swing=0.3, chords=["Dm7", "Dm7", "Am7", "Am7"], drums={
        KICK: "x......x..x.....", SNARE: "....x.......x...", HAT: "x.o.x.o.x.o.x.o."},
        bass="1......1..5.....", comp="x...............", melody="....5..3....1..."),  # boom bap: bass follows the kick
    "rock": dict(bpm=120, swing=0.0, chords=["E5", "B5", "C#5", "A5"], drums={
        KICK: "x.......x.x.....", SNARE: "....x.......x...", HAT: "x.x.x.x.x.x.x.x."},
        bass="1.1.1.1.1.1.1.1.", comp="x.......x.......", melody="1...1...5...8..."),  # I-V-vi-IV, driving 8ths
    "bossanova": dict(bpm=140, swing=0.0, chords=["Dm7", "G7", "Cmaj7", "A7"], drums={
        KICK: "x..xx..xx..xx..x", SIDESTICK: "x..x..x...x..x..", HAT: "x.x.x.x.x.x.x.x."},
        bass="1..5....1..5....", comp="x..x..x...x..x..", melody="3.....5.....7..."),  # chords follow the clave
    "reggae": dict(bpm=75, swing=0.15, chords=["Am", "D", "Am", "Em"], drums={
        KICK: "........x.......", SIDESTICK: "........x.......", HAT: "x.x.x.x.x.x.x.x."},
        bass="..1...3.5...1...", comp="....x.......x...", melody="1.....3.....5..."),  # one drop: beat 3, skank on 2 and 4
}

QUALITIES = {"": [0, 4, 7], "m": [0, 3, 7], "7": [0, 4, 7, 10], "m7": [0, 3, 7, 10], "maj7": [0, 4, 7, 11], "5": [0, 7, 12]}
PITCH = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
PPQ, STEP = 480, 120  # ticks per quarter, per 16th
BARS = 8


def chord(name):
    """'C#m7' -> (root pitch class, intervals)."""
    n = 2 if name[1:2] in ("#", "b") else 1
    root = PITCH[name[0]] + {"#": 1, "b": -1}.get(name[1:2], 0)
    return root % 12, QUALITIES[name[n:]]


def tone(intervals, c):
    """Chord degree character -> semitones above the root."""
    third = intervals[1] if intervals[1] in (3, 4) else 7  # power chords have no third: use the fifth
    return {"1": 0, "3": third, "5": 7, "7": intervals[3] if len(intervals) > 3 else 12, "8": 12}[c]


def vlq(n):
    out = [n & 0x7F]
    while n > 127:
        n >>= 7
        out.append(0x80 | (n & 0x7F))
    return bytes(reversed(out))


def track(name, events):
    """events: (tick, bytes). Note-offs sort before note-ons at the same tick."""
    data, last = b"\x00\xff\x03" + vlq(len(name)) + name.encode(), 0
    for tick, msg in sorted(events, key=lambda e: (e[0], e[1][0] & 0xF0 != 0x80)):
        data += vlq(tick - last) + msg
        last = tick
    data += b"\x00\xff\x2f\x00"
    return b"MTrk" + struct.pack(">I", len(data)) + data


def at(bar, step, swing):
    return (bar * 16 + step) * STEP + (round(swing * STEP) if step % 2 else 0)


def notes(ch, pitch_vel_on_off):
    ev = []
    for pitch, vel, on, off in pitch_vel_on_off:
        ev += [(on, bytes([0x90 | ch, pitch, vel])), (off, bytes([0x80 | ch, pitch, 0]))]
    return ev


def phrase(pattern, bar, swing):
    """Steps that play and how long each lasts (until the next one or the bar's end)."""
    hits = [i for i, c in enumerate(pattern) if c != "."]
    return [(pattern[i], at(bar, i, swing), at(bar, j, swing) - 10) for i, j in zip(hits, hits[1:] + [16])]


def song(g):
    sw, drums, bass, comp, mel = g["swing"], [], [], [], []
    for bar in range(BARS):
        root, iv = chord(g["chords"][bar % len(g["chords"])])
        for note, pat in g["drums"].items():
            drums += [(note, 110 if c == "x" else 80, at(bar, i, sw), at(bar, i, sw) + STEP // 2) for i, c in enumerate(pat) if c != "."]
        bass += [(36 + root + tone(iv, c), 95, on, off) for c, on, off in phrase(g["bass"], bar, sw)]
        comp += [(60 + pc, 70, on, off) for _, on, off in phrase(g["comp"], bar, sw)
                 for pc in sorted({(root + i) % 12 for i in iv})]
        mel += [(72 + root + tone(iv, c), 90, on, off) for c, on, off in phrase(g["melody"], bar, sw)]
    tempo = round(60_000_000 / g["bpm"])
    conductor = [(0, b"\xff\x51\x03" + tempo.to_bytes(3, "big")), (0, b"\xff\x58\x04\x04\x02\x18\x08")]  # tempo, 4/4
    tracks = [track("Tempo", conductor), track("Drums", notes(9, drums)), track("Bass", notes(0, bass)),
              track("Chords", notes(1, comp)), track("Melody", notes(2, mel))]
    return b"MThd" + struct.pack(">IHHH", 6, 1, len(tracks), PPQ) + b"".join(tracks)


assert vlq(0) == b"\x00" and vlq(127) == b"\x7f" and vlq(128) == b"\x81\x00" and vlq(16383) == b"\xff\x7f"
assert chord("C#m7") == (1, [0, 3, 7, 10]) and chord("Bb") == (10, [0, 4, 7])
assert all(len(p) == 16 for g in GENRES.values() for p in [g["bass"], g["comp"], g["melody"], *g["drums"].values()])

if __name__ == "__main__":
    out = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path.home() / "Documents" / "LocalMIDIStudio" / "Genres"
    out.mkdir(parents=True, exist_ok=True)
    for name, g in GENRES.items():
        (out / f"{name}.mid").write_bytes(song(g))
        print(f"{out / name}.mid  {g['bpm']} BPM  {' '.join(g['chords'])}")
