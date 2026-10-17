# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alejandro Bustos
"""Check the offline renderer with independently encoded WAV impulses."""
from pathlib import Path
import struct
import subprocess
import sys

binary, plugin = map(Path, sys.argv[1:3])
root = binary.parent / "render-check"
root.mkdir(exist_ok=True)
for tag, bits in ((1, 16), (1, 24), (1, 32), (3, 32)):
    size = bits // 8
    if tag == 3:
        impulse = struct.pack("<f", 0.25)
    else:
        impulse = (1 << (bits - 3)).to_bytes(size, "little", signed=True)
    data = impulse + bytes(size * 4799)
    header = (b"RIFF" + struct.pack("<I", len(data) + 36) + b"WAVEfmt " +
              struct.pack("<IHHIIHH", 16, tag, 1, 48000, 48000 * size, size, bits) +
              b"data" + struct.pack("<I", len(data)))
    source, output = root / f"in-{tag}-{bits}.wav", root / f"out-{tag}-{bits}.wav"
    source.write_bytes(header + data)
    subprocess.run([str(binary), str(plugin), str(source), str(output), "50", "0", "0.7071067812", "0", "0.1"], check=True, stdout=subprocess.DEVNULL)
    encoded = output.read_bytes()
    assert encoded[:4] == b"RIFF" and struct.unpack_from("<HH", encoded, 20) == (3, 2)
    offset = 12
    while encoded[offset:offset + 4] != b"data":
        length = struct.unpack_from("<I", encoded, offset + 4)[0]
        offset += 8 + length + (length & 1)
    offset += 8
    samples = struct.unpack_from("<" + "f" * ((len(encoded) - offset) // 4), encoded, offset)
    assert abs(samples[0] - 0.25) < 1e-6 and abs(samples[2400 * 2] - 0.25) < 1e-6
    assert abs(samples[4800 * 2]) < 1e-6 and len(samples) == 9600 * 2
print("PASS renderer: PCM16/24/32 and float32 mono input, stereo output, exact 50 ms echo")

# New optional arguments reach the DSP without changing the live dry impulse.
trimmed = root / "trimmed.wav"
subprocess.run([str(binary), str(plugin), str(source), str(trimmed), "50", "0", "0.7071067812", "0", "0.1", "3", "0"], check=True, stdout=subprocess.DEVNULL)
encoded = trimmed.read_bytes()
samples = struct.unpack_from("<" + "f" * ((len(encoded) - offset) // 4), encoded, offset)
assert samples[0] == 0.25 and all(value == 0 for value in samples[2:])
print("PASS renderer: Modifier/Modify select a fully trimmed echo with dry preserved")
