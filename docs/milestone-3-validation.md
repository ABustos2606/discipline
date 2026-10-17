# Milestone 3 implementation and validation

**Superseded in part after the first Pi test (17 October 2026):** the
modifiers now run inside Discipline's echo loop rather than on Send, Trimmer
shortens each echo instead of repeating a slice, Speed splices are longer and
Smooth speed's curve changed, Dropper's grains are 15 ms over the last
quarter, and Modifier gains Outside. See the update in [design](design.md) and
[the feedback](pi-feedback-2026-10-17.md). The record below is as of `812eb9c`.

Implemented 16-17 October 2026. The user selected the Size clock and all four
proposed modifiers. The implementation is available for review; physical Pi
xrun measurements, listening/tuning and the `v0.3.0` release tag remain pending.

## Behavior and compatibility

Main now has 38 ports. The original 35 Main ports and all four Return ports
keep their indices, symbols and URIs. `modifier` (35), `modify` (36) and
`modify_cv` (37) are appended; Main's LV2 version advances to 0.2.0. Return
remains 0.1.1. Modify CV is added to the knob per sample and clamped to 0 - 1. Modifier is
Off by default, preserving the existing audio behavior for saved patches.
The previously reserved Size clock output is now active.

Size clock writes a 0/1 trigger lasting `round(sample_rate * 0.001)` frames
at every Size boundary, including sample zero after activation. It uses the
same phase as Auto Scan. A Size change is adopted at the next wrap after the
new echo tap commits. On/bypass, Loop, Clear and Pair changes keep this clock
running. Modifiers anticipate the Return's one-period transport delay so
their audible gates follow this phase.

Modifiers act on Send, before any external modules. IN processes each repeat;
OUT leaves the internal feedback unchanged and processes its playback; FEED
records Repeats times processed Return into memory, independently of Level.
Live dry and memory dry bypass the modifiers. Existing Scan/Feed audit fixes
remain in place. Filter and stability effects can still be patched externally.

| Modifier | Modify mapping |
| --- | --- |
| Off (0) | Ordinary echo |
| Stepped speed (1) | 15 evenly spaced settings: -4, -3, -2, -1.5, -1, -0.75, -0.5, 0, 0.5, 0.75, 1, 1.5, 2, 3, 4 |
| Smooth speed (2) | -2x to +2x; noon stops, 0.75 is +1x with a small unity detent |
| Trimmer (3) | Stepped; noon unchanged; left keeps the start, right keeps the end (3/4 to 1/8), the kept slice repeats to fill the echo |
| Dropper (4) | Noon unchanged; left random (new every echo), right a fixed 16-step pattern; drops vary in depth and edge; last 10% of each side moves toward roughly 5 ms grains |

Speed references complete captured Size windows, restarting each Size with a
1 ms complementary raised-cosine fade. Reverse starts at the last sample of
the captured window. Fast rates wrap within that window; slow rates read only
part before the next restart. This deliberately changes transient placement
and startup audio. It is a rhythmic effect, with ordinary delay timing at
positive unity. Both speed modes fade to silence at stop rather than holding
DC. Mode/rate changes use 10 ms smoothers, with a 5 ms stop gain ramp. Trimmer
crossfades each slice jump over 1 ms; Dropper edges run from 0.5 to 20 ms,
followed by a 1 ms gain ramp.

When a fast wrap fade overlaps the next Size restart, the new fade starts
from the last mixed output sample. This keeps the seam continuous with at
most two read heads; it does not discard a partly audible fade partner.
If a Size fade commits inside Send's one-period lead, the captured reader
updates its length and position relative to that same boundary. It does not
wait a full new Size while continuing to play the old, shorter window.

Rates up to unity use Hermite interpolation; faster playback uses a 32-tap
windowed-sinc antialias filter with precomputed cutoff and fractional-phase
tables. Positive unity bypasses that reader exactly after its transition.
Trimmer noon and Dropper noon are also bit-exact neutral settings.

A captured window is read while the next one records, so it can be two
windows old. The echo line stays at 60 s and Speed windows are capped at
30 s: above a 30 s Size, each half of the echo is captured and played
separately, so non-unity forward playback starts about Size/2 behind.
Trimmer uses the same windows; Dropper follows the full Size. Windows are never copied in
`run()`. Memory and echo buffers total about 92 MB at 48 kHz or 184 MB at
96 kHz per instance,
plus about 0.22 MB of filter coefficients and small fade tables. Allocation,
page touching and buffer-lock attempts stay in instantiation. Restart, Clear,
echo discard and fault handling reset counters/references in constant time.

## Automated evidence

`tests/test_m3.cpp` loads the actual shared object and checks:

| Check | Evidence |
| --- | --- |
| Clock | Exact pulse positions/width at 44.1, 48 and 96 kHz with 64/128/256-frame periods; committed Size changes; BPM/beats and maximum Size CV; bypass/Loop/Clear/Pair continuity and activation reset |
| Speed | Measured pitch within 0.5%, forward/reverse/stopped playback, coherent stereo, complete reverse windows up to 30 s, 30 s chunks at a 60 s Size at +/-4x, Size commits inside Send's transport lead |
| Aliasing | At 2x, a 19 kHz input's folded component is at least 20 dB below the wanted shifted 1 kHz component |
| Neutral settings | Bit-identical full audio/feedback versus Off at +1x, Trimmer noon and Dropper noon |
| Rhythm | With a coded ramp, all 15 Trimmer steps replay exactly the expected frames (start on the left, end on the right), also with 30 s halves at a 60 s Size; Dropper's pattern side repeats sample-for-sample on successive Sizes, its random side changes, both have shallow drops, never boost, and produce grains at maximum |
| Trails | Re-enabling while Trails ring clears them; with Always or Collect they keep ringing |
| Transitions | Bounded constant-input sample steps through mode/amount changes; a coded ramp catches overlapping 4x wrap/Size fades; bit-identical output with independently randomized host slices |
| Modify CV | Knob plus CV and clamping are bit-identical to the equivalent knob setting (Smooth unity, Trimmer noon, Stepped 2x, endpoints); a per-sample LFO is bit-identical under random host slicing in all four modes |
| Routing/discard | Muting IN/FEED destroys recirculating content, whereas OUT preserves its internal repeats; a short bypass discards captured reverse windows along with the echo |

The native full `make -j4 test` suite passes, including prior milestone/audit
tests. The sanitizer build (`make -j4 test SAN=1`) passes with AddressSanitizer
and UBSan. The WAV renderer retains its format/timing tests and now verifies the
new optional Modifier/Modify arguments with stopped Smooth speed (silent echo)
and unchanged live dry. Ten simulated minutes of random controls now include all
modifier modes and amounts alongside Size, memory and bypass controls.

The final native x86-64 stress run, alongside sanitizer/arm64 rebuilds,
reported 252.696 microseconds at the 99.9th percentile and 4702.52 microseconds
worst per 128-frame period. That worst elapsed WSL time exceeds the
2667-microsecond audio period. It includes desktop scheduling and is neither
a physical Pi measurement nor an xrun guarantee.

The full suite also passes in the existing Debian arm64 root under QEMU,
with `BUILD_DIR=build/arm64 OPT="-O2 -mcpu=cortex-a76"`. The bundle is an
AArch64 ELF shared object. After the final fade/Size corrections, the arm64
bundle and focused Milestone 3 suite are rebuilt and checked again. Emulation
provides architecture/function coverage, not physical Pi performance evidence.

## Hosts and metadata

Private dummy JACK tests pass at 1024 and 128 frames in jalv and Ingen, with
one and four Ingen processing threads. Ingen routes Size clock through
Peepo's actual PolyLV2 clock divider at a division of four. Captured edges
remain on the Size grid at both 50 ms and BPM-synced 100 ms, with 1 ms source
pulses. These are functional checks, not physical realtime measurements.

The divider was compiled from the unmodified source in Peepo's local upstream
reference into an ignored private `build/test-lv2/` bundle. Its GPL code is
not added to Discipline or installed with the MIT bundle. The host check
accepts `--clock-divider` when that plugin is available on `LV2_PATH`:

```sh
python3 tests/host_smoke.py build/discipline.lv2 --period 128 --clock-divider
```

Existing host checks retain exact echo timing/decay through external effects,
all routings, BPM sync, enabled-port bypass, Wipe/Memory dry/Dry kill/Always,
a filter return path and cable-feedback rejection. Save/reload now also
verifies Modifier and Modify restoration, with empty audio buffers.

The Turtle checker verifies 38/4 ports, freezes all original symbols/indices,
and validates the new bounds/defaults. Staged installation and lv2lint
0.17.151 pass for both plugins, including extended allocation/syscall checks.
The existing warning is missing author email (`foaf:mbox`); no address is
invented. The sole defined dynamic export remains `lv2_descriptor`.

## Review changes, 17 October 2026

After review, the echo line went back to 60 s (from 120 s) with Speed windows
capped at 30 s, and Modify CV was appended. The full suite, the sanitizer
build and the Ingen checks (1 and 4 threads, clock divider) pass. Ingen also
checks Modify CV: with Smooth speed at +1x, a CV of -0.25 from a patch CV
input reaches noon, which stops the echo and leaves exactly the live dry. Deliberately
removing the window cap, ignoring the CV, or reading it once per run each
make `test_m3` fail. The arm64 build and lv2lint were not rerun.

A second pass followed the manual's Modify dials and Trails note (pp. 16, 43).
Trimmer was reversed (the manual has cut end on the left) and only gated;
it is now stepped and repeats the kept slice. Dropper is now neutral at noon
with random drops on the left, as on the B3 dial; its pattern side keeps the
earlier behaviour with varied drop shapes. Re-enabling now clears ringing
Trails unless Always or Collect is on. Swapping Trimmer's sides, removing
its repeat, or removing the Trails fix each make `test_m3` fail.

## Listening material and notes

No human listening session or guitar recording was available here. These are
specific checks to perform, not claims of completed listening. Generate
synthetic stereo plucked-tone comparisons through the actual bundle with:

```sh
make render-examples
```

The files and settings manifest are in ignored `build/m3-listening/`. They
include live dry, Size 500 ms, Repeats 0.4, Level 0.5 and OUT routing, so each
modified repeat can be compared with `off.wav`. The renderer also accepts
arbitrary guitar WAV files; see README for its arguments.

| Modifier | Comparison and listening check |
| --- | --- |
| Stepped Speed | `stepped-2x.wav`: confirm an octave up, recognizable attacks, repeat restart/wrap seams; sweep stepped settings on guitar and listen near stop |
| Smooth Speed/reverse | `smooth-reverse.wav` and `smooth-stop.wav`: confirm a full reversed Size phrase and dry-only stop; sweep through reverse/zero/+1x for clicks, level dips and filter coloration |
| Trimmer | `trimmer-keep-start-half.wav` and `trimmer-keep-end-quarter.wav`: hear the start half repeated twice and the last quarter repeated four times per echo; sweep all steps for seams at the 1 ms slice crossfades |
| Dropper | `dropper-random.wav`, `dropper-pattern.wav` and `dropper-grains.wav`: random holes change each echo, pattern holes recur, maximum makes grains; judge drop shapes and usable density on sustained guitar |

Also change Size/tempo while each modifier is active, compare IN's successive
processing with OUT's unchanged stored repeats, and listen with Loop, deep
Scan/Spread, Wipe, Trails and high Repeats. The automated transition bounds
cover constant signals; they do not settle musical quality on transients.

## Remaining release gates

Run the plan's 30-minute typical patch on a physical Pi, including fast Speed
and restarts at high Repeats. Record xruns, DSP load, RSS and native worst-case
timing, and complete the listening notes above. Peepo's modifier choices,
catalogue grouping and pinned-build integration belong in its repository.
No Peepo files or release tags were changed by this implementation.
