# Discipline

An LV2 stereo delay with a 3-minute memory, written for Peepo and other LV2
hosts. MIT licensed, Copyright (c) 2026 Alejandro Bustos.

Milestones 1-3 implement the stereo delay core, memory controls and timed modifiers: 50 ms to 60 s echoes, Repeats,
Level, Size in milliseconds or beats at BPM, IN / OUT / FEED routing, Clear,
and CV inputs for Level, Repeats and Size. Size changes crossfade between fixed
read positions to preserve pitch. The separate 3-minute memory records the
input and, in FEED, the processed repeats.

Scan (Auto and Manual), Spread, Collect, Loop/Latch, Memory dry, Wipe, Dry kill,
On/bypass, Always and Trails are active, including Scan/Spread CV. Size clock,
Stepped Speed, Smooth Speed/reverse, Trimmer and Dropper are active.
The original 39 ports retain their indices and symbols; Modifier, Modify and
Modify CV append three main-plugin ports. Physical Pi and listening release gates remain
pending; see [Milestone 3 validation](docs/milestone-3-validation.md). Peepo's build step and catalogue entry are
described in [Adding Discipline to Peepo](docs/peepo-integration.md).

Discipline works on its own: the echo, its Repeats loop and the modifiers
all run inside the plugin. A fresh Discipline with default controls is a
500 ms delay; no cables or second module are needed.

To put your own modules inside the echo, pick **Outside** in Modifier and
add a Discipline Return:

```text
Input --> Discipline ------------------------------> Output
              |
              +-- Send --> [your modules] --> Discipline Return
```

The Return is a second plugin in the same bundle, because Ingen rejects a
cable loop back into the same plugin. There is one pair per host process: the
Return joins the Discipline whose Modifier is Outside, with no ID to match. A
second Discipline on Outside keeps its echo inside and takes the Return when
the first lets go. Return found (port `paired`) shows the link. Return loss
and recovery fade over 64 samples, and the echo crossfades back inside in
10 ms whenever no Return audio arrives, so a missing or stopped Return never
silences the echo. Both plugins must run in the same host process and load the
same shared object; separate jalv processes cannot pair. Send is silent unless
Modifier is Outside. The Return has two audio inputs and no audio outputs.

The old Pair control (port 26 on Discipline, 2 on the Return) is no longer
read. It stays, hidden, so patches saved with it still load.

| Control | Behaviour |
| --- | --- |
| Level | Echo volume, with untouched dry audio; 0 mutes wet, 1 is +6 dB wet |
| Repeats | Feedback gain; the top 3% reaches unity, with feedback limiting |
| Size | Echo time in ms; changes crossfade over 30 ms |
| Sync / Size beats / BPM | Echo time is beats times 60/BPM, limited to 50 ms-60 s |
| Routing | In: the modifier acts again on every repeat; Out: repeats stay plain and each echo is modified once on the way out; Feed: the modified echo is recorded into memory |
| Clear | Rising edge fades memory reads over 20 ms and invalidates old history in constant time; existing echoes remain |
| Scan | Manual: position in the 3-minute memory; Auto: frequency and depth of excursions, with a snap back to the present |
| Scan hold | Forces maximum Auto Scan while held, then returns to the Manual setting or Auto present |
| Spread | Adds a second memory position behind Scan; the two positions mix at equal power |
| Collect | Overdubs the memory's previous 3-minute turn at 0.7 gain, with its own feedback limiter |
| Loop / Latch | Holds the echo unchanged while memory records; Latch toggles on each Loop rising edge |
| Memory dry | Off, Scanning (history only, default), or On (full memory mix, including present) |
| Wipe: no echo | Mutes the echo, so only the memory is heard, and forces Memory dry On |
| Dry kill | Off; Always (including bypass); Effect on (passes dry while bypassed) |
| On / Always / Trails | Bypass through On; Always records while bypassed; Trails lets echoes decay |
| Size clock | CV output: 1.0 for 1 ms at each Size boundary, otherwise zero |
| Modifier / Modify | Off, Stepped speed, Smooth speed, Trimmer, Dropper or Outside; Modify sets the amount |
| Modify CV | CV input added to Modify per sample, then limited to 0 - 1 |

Modifier defaults to Off. The modifiers sit in the echo loop: In applies them
again on every repeat, Out keeps the repeats plain and modifies only what is
heard, and Feed records the modified echo. Outside replaces them with the
modules between Send and Return, in the same three places. Live dry and memory
dry bypass modifiers.

| Modifier | Modify, from 0 to 1 |
| --- | --- |
| Stepped speed | Rates -4, -3, -2, -1.5, -1, -0.75, -0.5, 0, 0.5, 0.75, 1, 1.5, 2, 3, 4; evenly spaced knob positions |
| Smooth speed | -2x to +2x, even in pitch: stopped in a narrow band at noon, 1/4x just off noon, 1/2x at about 0.36 and 0.64, ordinary +1x at 0.75, 2x at the ends |
| Trimmer | Stepped. Noon is unchanged; left keeps the start of each echo, right keeps its end (3/4, 2/3, 1/2, 1/3, 1/4, 1/6, 1/8) and silences the rest, so each echo gets shorter. Cuts have 3 ms edges |
| Dropper | Noon is unchanged; left drops at random (new drops every echo), right repeats a 16-step pattern every echo. Drops vary from shallow and smooth to deep and abrupt; the last quarter of each side moves toward 15 ms grains, half of them kept |
| Outside | The echo leaves through Send and comes back through a Discipline Return; Modify does nothing |

Negative speed reverses a complete captured Size window, up to 30 s. Above a
30 s Size, Speed captures and plays each half of the echo separately. Each Size
boundary captures the echo just completed. Slow rates read only part of each
window; fast rates wrap within it. Each restart and wrap is a splice of up to
10 ms (at most half of one fast window), and the outgoing read keeps moving
through the audio around its window, so a splice joins two moving sounds
rather than a held one. Capturing windows changes the placement of transients
and startup audio; it is a rhythmic effect, with unchanged echo timing at +1x.
Playback above 1x uses a windowed-sinc antialias filter; stopped playback fades
to silence instead of holding DC. Changes of mode and speed smooth over 10 ms.

The clock shares Auto Scan's decision phase. It takes a completed Size change
at its next wrap, continues through On/bypass, Loop and Clear, and restarts at
activation. The modifiers follow the same phase.

Scan and Spread targets snap to whole multiples of Size and clamp inside the
memory. Moves use a short local scrub and a 60 ms crossfade, so even a full
3-minute jump settles within 150 ms. A target received during a fade replaces
the pending target. Snap-backs use 30 ms. Complementary fade weights avoid a
volume boost on correlated audio. At maximum Auto
Scan the excursion stays away until the setting is reduced. History that has
not been recorded, or was cleared, plays silence. Loop holds the modified echo
in In routing and the plain full-Size echo in Out and Feed. Size can change
without erasing a held loop. Loop releases back to Repeats over 50 ms.

Feed records only Repeats times the echo, independently of Level and memory
dry. Memory dry monitors playback; Collect provides memory overdubbing. Send
carries only the echo tap. There is no feedback path at the host block period.
Wipe mutes the echo output; in Feed, the echo still enters memory and can be
heard later through memory dry.

Spread is off when its snapped offset is zero. In Scanning mode the present
tap is omitted, so turning on Spread does not add another copy of live dry.
Memory dry On deliberately monitors the full mix; Dry kill can remove live
dry when that full memory playback is wanted by itself.

Bypass fades the effect over 10 ms. Without Trails it also discards the echo
after fading, including on a quick bypass/re-engage. With Trails, turning the
effect back on fades out and clears tails still ringing, unless Always or
Collect is on. Trails releases Loop back
to ordinary Repeats, so a held Loop cannot override tail decay. At maximum
Repeats, ordinary unity feedback can still sustain a tail. Ordinarily bypass clears memory. Collect
pauses the memory and its read heads; with Trails, it keeps moving and
overdubbing without live input. Always keeps recording live input, overrides
Collect while bypassed, and caps Repeats feedback at 0.7. **Use the On control
(`enabled`) for bypass.** Ingen's block bypass stops running the plugin and
cannot provide these behaviors; Peepo's overlay must use `"bypass": "port"`.

CV is added to the corresponding knob's normalized 0-1 position, then clamped.
Size uses a logarithmic scale. Level and Repeats read CV per sample; Size reads
it every 32 samples, as do Scan and Spread. Peepo's LFOs provide bipolar and unipolar modes; its CV
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
| Discipline | `https://github.com/ABustos2606/discipline#discipline` | 38 |
| Discipline Return | `https://github.com/ABustos2606/discipline#return` | 4 |

Discipline's LV2 version is 0.4.0; Return's is 0.2.0. Discipline requires
`urid:map`; Return declares it optional and requires no host features. Neither has a
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
recorded in [the Milestone 3 validation record](docs/milestone-3-validation.md).

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
in Ingen with one and four processing threads. It checks the echo inside
Discipline with the Return unused, exact Outside echo timing and decay in all
three routings, Size clock pulses, BPM sync, enabled-port bypass, Wipe, Memory
dry, Dry kill, Always recording, a filter in the return path, feedback
rejection, and restoration of saved controls (including the unused Pair) with
empty audio buffers after reload.
It stops only its own child processes. Logs go
to `build/host-smoke/`. The default period is 1024 frames to tolerate WSL
scheduling; use `python3 tests/host_smoke.py build/discipline.lv2 --period 128`
for a shorter period. These are functional checks, not xrun measurements.
Add `--clock-divider` when Peepo's PolyLV2 clock divider is on `LV2_PATH` to
check a real module dividing Size triggers by four.

For offline listening, render a mono or stereo PCM16/24/32 or float32 WAV through
the actual bundle (Modifier 5, Outside, uses a direct Send-to-Return cable):

```sh
make render
build/render build/discipline.lv2/discipline.so input.wav output.wav 500 0.4 0.5 0 4
# Arguments after output: Size ms, Repeats, Level, Routing (0 In/1 Feed/2 Out), tail seconds, Modifier, Modify
build/render build/discipline.lv2/discipline.so input.wav reverse.wav 500 0.4 0.5 2 4 2 0.25
make render-examples                      # synthetic comparisons in build/m3-listening/
```

The renderer writes stereo float32 WAV and needs no audio server or extra audio
libraries. It preserves input sample rate and duplicates mono input to stereo.

The memory and echo buffers use about 92 MB per instance at 48 kHz,
about 184 MB at 96 kHz, plus about 0.22 MB of speed filter coefficients.
Multiple instances multiply that cost. Buffers will not
be saved with patches. Allocation, page touching and an optional memory-lock
attempt happen during instantiation. The shared transport adds about 2 MB per
loaded library. Clear and activation reset counters without clearing buffers.

The behaviour specification is [docs/design.md](docs/design.md); the build and
milestone requirements are in [docs/implementation-plan.md](docs/implementation-plan.md).
Do not rename plugin URIs or port symbols after patches use them. Peepo's
catalogue overlay, pinned build integration and hardware evidence belong in
Peepo's repository.
