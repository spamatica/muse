#!/usr/bin/env python3
#
# Checks the stems exported from the test project made by make_song.py (see README).
#   check_stems.py <project dir> <stem dir> <mode 0|1|out|midi>
#
# Every stem must have exactly the length of its range, start exactly at its first
# frame, and contain its track only:
#   A = a.wav * 1.5   (the track plus its aux send of 0.5, the aux at 0 dB)
#   B = b.wav
# Mode "out" (range markers, through the output track, its fader at 0.5): half of that.
# A difference of 2 (16 bit) is allowed for the float conversions.

import array
import sys
import wave
from pathlib import Path

TOLERANCE = 2


def read(path):
    with wave.open(str(path), "rb") as w:
        assert w.getsampwidth() == 2, f"{path}: not 16 bit"
        assert w.getnchannels() == 2, f"{path}: not stereo"
        data = array.array("h")
        data.frombytes(w.readframes(w.getnframes()))
        return w.getnframes(), data


def check(stem_path, src, gain, start, end):
    if not stem_path.exists():
        return f"{stem_path}: missing"
    n, data = read(stem_path)
    if n != end - start:
        return f"{stem_path}: {n} frames, expected {end - start}"

    def worst(offset):
        w = 0
        for i in range(0, n * 2):
            j = (start + offset) * 2 + i
            exp = src[j] * gain if 0 <= j < len(src) else 0
            w = max(w, abs(data[i] - exp))
        return w

    w = worst(0)
    if w > TOLERANCE:
        # Find out how far off it is.
        for off in range(-4096, 4097):
            if off and worst_quick(data, src, gain, start + off) <= TOLERANCE:
                return f"{stem_path}: shifted by {off} frames"
        return f"{stem_path}: content differs (worst difference {w})"
    return None


def worst_quick(data, src, gain, start):
    w = 0
    for i in range(0, min(len(data), 2000)):
        j = start * 2 + i
        exp = src[j] * gain if 0 <= j < len(src) else 0
        w = max(w, abs(data[i] - exp))
        if w > TOLERANCE:
            break
    return w


def energy(data, start, end):
    """Mean absolute value of frames start - end."""
    seg = data[start * 2:end * 2]
    return sum(abs(v) for v in seg) / max(1, len(seg))


def check_midi(stems):
    # M1 plays in file frames 0 - 40204, M2 in 110250 - 150454 (plus the release of the notes).
    errors = 0
    expect = {"M1": (True, False), "M2": (False, True), "Synth": (True, True)}
    for name, (first, second) in expect.items():
        path = stems / f"miditest_{name}.wav"
        if not path.exists():
            print("FAIL", path, "missing")
            errors += 1
            continue
        n, data = read(path)
        e1 = energy(data, 1000, 39000)
        e2 = energy(data, 111250, 149000)
        ok = n == 176400 and (e1 > 50) == first and (e2 > 50) == second
        print("ok  " if ok else "FAIL", path, f"frames {n}, level M1 part {e1:.1f}, M2 part {e2:.1f}")
        errors += 0 if ok else 1
    sys.exit(1 if errors else 0)


def main():
    if sys.argv[3] == "midi":
        check_midi(Path(sys.argv[2]))
    proj, stems = Path(sys.argv[1]), Path(sys.argv[2])
    _, a = read(proj / "a.wav")
    _, b = read(proj / "b.wav")
    if sys.argv[3] == "out":
        jobs = [(stems / "stemtest_A.wav", a, 0.75, 22050, 198450),
                (stems / "stemtest_B.wav", b, 0.5, 22050, 198450)]
    elif sys.argv[3] == "0":
        jobs = [(stems / "stemtest_A.wav", a, 1.5, 22050, 198450),
                (stems / "stemtest_B.wav", b, 1.0, 22050, 198450)]
    else:
        jobs = []
        for name, s, e in (("Intro", 22050, 110250), ("Verse", 110250, 198450)):
            jobs.append((stems / name / f"stemtest_{name}_A.wav", a, 1.5, s, e))
            jobs.append((stems / name / f"stemtest_{name}_B.wav", b, 1.0, s, e))
    errors = 0
    for job in jobs:
        err = check(*job)
        if err:
            errors += 1
            print("FAIL", err)
        else:
            print("ok  ", job[0])
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
