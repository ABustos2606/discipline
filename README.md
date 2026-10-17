# Discipline

An LV2 stereo delay with a 3-minute memory, written for Peepo and other LV2
hosts. MIT licensed, Copyright (c) 2026 Alejandro Bustos.

Milestone 1 implements the stereo delay core: 50 ms to 60 s echoes, Repeats,
Level, Size in milliseconds or beats at BPM, IN / OUT / FEED routing, Clear,
and CV inputs for Level, Repeats and Size. Size changes crossfade between fixed
read positions to preserve pitch. The separate 3-minute memory records the
input and, in FEED, the processed repeats.

Scan, Spread, Collect, Loop, Memory dry, Wipe, Dry kill, On/bypass, Always and
Trails are reserved for Milestone 2. Their controls and Scan/Spread CV inputs
are currently inert. Size clock stays at zero until Milestone 3. All 39 ports
retain the indices and symbols established in Milestone 0.

The delay uses two plugins in one bundle:

```text
Input --> Discipline ------------------------------> Output
              |
              +-- Send --> [optional effects] --> Discipline Return
```

Set the same **Pair** (1-16) on both plugins. The two instances share a registry
inside `discipline.so`, avoiding a cable loop in Ingen. A second main or Return
with an occupied Pair stays unpaired and retries when that slot becomes free.
Paired falls to zero if the other owner goes away or stops processing for half
a second. Return loss and recovery fade over 64 samples; a stopped Return
realigns when it resumes. Both plugins must run in the same host process and
load the same shared object. Separate jalv processes cannot pair.

**Always cable Send to Return**, directly or through effects. Without that
cable there is no audible echo. Return has
two audio inputs and no audio outputs.

| Control | Behaviour |
| --- | --- |
| Level | Echo volume, with untouched dry audio; 0 mutes wet, 1 is +6 dB wet |
| Repeats | Feedback gain; the top 3% reaches unity, with feedback limiting |
| Size | Echo time in ms; changes crossfade over 30 ms |
| Sync / Size beats / BPM | Echo time is beats times 60/BPM, limited to 50 ms-60 s |
| Routing | In: effects repeat inside the echo loop; Out: each echo is processed once; Feed: processed repeats enter memory |
| Clear | Rising edge fades memory reads over 20 ms and invalidates old history in constant time; existing echoes remain |

CV is added to the corresponding knob's normalized 0-1 position, then clamped.
Size uses a logarithmic scale. Level and Repeats read CV per sample; Size reads
it every 32 samples. Peepo's LFOs provide bipolar and unipolar modes; its CV
utility modules can scale modulation.

Build on Linux with a C++17 compiler, GNU Make, pkg-config and LV2 headers
(`build-essential pkg-config lv2-dev` on Debian). The build needs no network:

```sh
make
make install PREFIX=/usr/local
# Or install where Peepo finds plugins:
make install LV2DIR=/opt/peepo/lib/lv2
```

The bundle is `build/discipline.lv2/`. Add its parent directory to `LV2_PATH`
to try it without installing. Both descriptors are in the same binary:

| Plugin | URI | Ports |
| --- | --- | --- |
| Discipline | `https://github.com/ABustos2606/discipline#discipline` | 35 |
| Discipline Return | `https://github.com/ABustos2606/discipline#return` | 4 |

Discipline requires `urid:map`; Return requires no host features. Neither has a
custom UI. Both offer `lv2:hardRTCapable`. Block length comes from optional
LV2 options: nominal, then maximum, then a 128-frame fallback. Correct echo
compensation needs the host's actual period. A period must be at most 4096
frames and shorter than 50 ms at the sample rate; unsupported configurations
fail instantiation rather than producing misleading delay times.

Install variables are `PREFIX` (default `/usr/local`), `LV2DIR` (default
`$(PREFIX)/lib/lv2`) and `DESTDIR` (a staging root). `OPT` replaces the optimisation
flags without introducing architecture-specific defaults:

```sh
make OPT="-O2 -mcpu=cortex-a76"             # native Pi build
make install DESTDIR="$PWD/build/stage" PREFIX=/opt/peepo
```

Offline tests also need Python 3 and `python3-rdflib`. They parse the Turtle,
check every port against `src/ports.hpp`, load the actual shared object, exercise
random audio slices, check exact timing and routing, exercise concurrent
cleanup and buffer overwrite, and run ten simulated minutes of random controls:

```sh
make test
make test SAN=1                           # AddressSanitizer + UBSan, separate build
make test-timing                          # timing stress run alone
make lint                                # lv2lint, after installing
make lint DESTDIR="$PWD/build/stage" PREFIX=/opt/peepo
```

`make lint` prints SKIP if lv2lint is absent. It fails when an installed lv2lint
reports an error. The remaining author-email warning and validation limits are
recorded in [the Milestone 1 validation record](docs/milestone-1-validation.md).

For a focused ThreadSanitizer transport check:

```sh
make SAN=thread build/tsan/test_dsp
setarch "$(uname -m)" -R build/tsan/test_dsp  # WSL may need this per-process ASLR setting
```

Optional host checks need `jackd`, `jalv`, Ingen on `PATH`, and the Python
packages `jack-client` and `numpy` (`python3-jack-client python3-numpy` on Debian).
The extended Ingen checks also use the LV2 example amplifier and FOMP's first
Moog low-pass filter (`lv2-examples fomp` on Debian):

```sh
make test-hosts
# Peepo's engine environment, under WSL:
. '/mnt/c/Pedal PI/tools/ingen-spike/env.sh'
make test-hosts
```

This check starts a private dummy JACK server, loads both plugins in jalv, then
patches Discipline Send through external effects into the input-only Return
in Ingen with one and four processing threads. It checks exact echo timing and
decay in all three routings, BPM sync, a filter in the return path, feedback
rejection, and control/Pair restoration with empty audio buffers after reload.
It stops only its own child processes. Logs go
to `build/host-smoke/`. The default period is 1024 frames to tolerate WSL
scheduling; use `python3 tests/host_smoke.py build/discipline.lv2 --period 128`
for a shorter period. These are functional checks, not xrun measurements.

For offline listening, render a mono or stereo PCM16/24/32 or float32 WAV through
the actual bundle, with a direct Send-to-Return cable:

```sh
make render
build/render build/discipline.lv2/discipline.so input.wav output.wav 500 0.4 0.5 0 4
# Arguments after output: Size ms, Repeats, Level, Routing (0 In/1 Feed/2 Out), tail seconds
```

The renderer writes stereo float32 WAV and needs no audio server or extra audio
libraries. It preserves input sample rate and duplicates mono input to stereo.

The memory and echo buffers use about 92 MB per instance at 48 kHz,
about 184 MB at 96 kHz. Multiple instances multiply that cost. Buffers will not
be saved with patches. Allocation, page touching and an optional memory-lock
attempt happen during instantiation. The shared transport adds about 2 MB per
loaded library. Clear and activation reset counters without clearing buffers.

The behaviour specification is [docs/design.md](docs/design.md); the build and
milestone requirements are in [docs/implementation-plan.md](docs/implementation-plan.md).
Do not rename plugin URIs or port symbols after patches use them. Peepo's
catalogue overlay, pinned build integration and hardware evidence belong in
Peepo's repository.
