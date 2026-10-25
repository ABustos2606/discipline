# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 Alejandro Bustos
"""Generate reproducible synthetic listening material through the actual LV2."""
from pathlib import Path
import math
import struct
import subprocess
import sys
import wave

binary, plugin, root = map(Path, sys.argv[1:4])
root.mkdir(parents=True, exist_ok=True)
rate = 48000
source = root / "source.wav"
audio = bytearray()
for frame in range(rate * 4):
    time = frame / rate
    onset = int(time / 0.7)
    age = time - onset * 0.7
    frequency = (110, 146.8324, 164.8138, 130.8128, 196, 220)[onset]
    envelope = min(1, age / 0.002) * math.exp(-age * 9)
    left = envelope * sum(0.10 / harmonic * math.sin(2 * math.pi * frequency * harmonic * age) for harmonic in range(1, 7))
    right = envelope * sum(0.08 / harmonic * math.sin(2 * math.pi * frequency * 1.5 * harmonic * age) for harmonic in range(1, 7))
    audio.extend(struct.pack("<hh", round(left * 32767), round(right * 32767)))
with wave.open(str(source), "wb") as output:
    output.setparams((2, 2, rate, 0, "NONE", "not compressed"))
    output.writeframes(audio)

settings = (
    ("off", 0, 0.5),
    ("stepped-2x", 1, 6 / 7),
    ("smooth-reverse", 2, 0.25),
    ("smooth-stop", 2, 0.5),
    ("trimmer-keep-start-half", 3, 3 / 14),
    ("trimmer-keep-end-quarter", 3, 12 / 14),
    ("dropper-random", 4, 0.2),
    ("dropper-pattern", 4, 0.8),
    ("dropper-grains", 4, 1),
)
notes = ["Synthetic plucked tones, not a guitar recording or a completed listening review.",
         "All renders: Size 500 ms, Repeats 0.4, Level 0.5, Out routing, 4 s tail.",
         "Compare off.wav first; live dry is included in every file.", ""]
for name, mode, amount in settings:
    subprocess.run([str(binary), str(plugin), str(source), str(root / f"{name}.wav"),
                    "500", "0.4", "0.5", "2", "4", str(mode), str(amount)], check=True)
    notes.append(f"{name}.wav: Modifier {mode}, Modify {amount:.8g}")
(root / "README.txt").write_text("\n".join(notes) + "\n", encoding="utf-8")
print(f"Listening material: {root}")
