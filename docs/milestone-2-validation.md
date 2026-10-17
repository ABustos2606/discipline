# Milestone 2 validation

Date: 16 October 2026. The memory features are implemented. Physical Raspberry
Pi listening and xrun measurements remain release gates; no `v0.2.0` tag has
been created by this work.

## Changes

- Main and Return preserve the local position within a host period when a
  Pair claim joins a slot's rounded timeline. A regression changes both Pair
  controls 37 samples into a period and verifies the exact one-block return.
- Non-finite live input becomes zero only at the affected channel/sample.
  A comparison against sanitized input verifies the existing echo, opposite
  channel and other samples remain unchanged. Non-finite external Return
  still uses the feedback fault containment path.
- Return declares optional `urid:map`; it continues to instantiate without it.
- Manual Scan and Spread snap to whole Sizes and clamp after snapping. A
  local 20 ms scrub accompanies a 60 ms complementary crossfade to large
  destinations. Even a full-depth move or a pending replacement settles in
  under 150 ms. Snap-backs use 30 ms. Correlated audio retains its level.
- Auto Scan uses seeded, instance-local xorshift32 decisions at Size boundaries:
  probability equals Scan, depth is limited by Scan squared, and excursions
  hold for one to four echoes. At maximum Scan they stay away. Scan hold
  starts immediately, even between CV updates at a 60-second Size, and snaps
  back on release to the Manual target in Manual mode or the present in Auto.
  State resets retain the RNG stream, and instances receive distinct seeds.
- Collect reads the overwritten slot before the memory write, recirculating
  the previous 180-second turn at 0.7 through a separate linked limiter. Its
  read uses a 256-sample fade at startup and post-Clear validity boundaries.
- Loop fades echo input out over 10 ms and uses unity feedback with the
  limiter bypassed. Release returns to Repeats over 50 ms. Latch responds to
  rising edges. Memory keeps recording and Size changes preserve held audio.
- Memory dry, Wipe, Dry kill and `enabled` bypass are active. Collect pauses
  during bypass, or advances without live input with Trails. Always records
  during bypass, overrides Collect and caps ordinary Repeats feedback at 0.7.
  Without Trails, bypass discards echoes after fading, even on a quick
  bypass/re-enable. Trails releases Loop to ordinary Repeats over 50 ms.

All 35 Main and four Return port indices, symbols and URIs are unchanged.
Metadata version is 1.1. Size clock remains zero until Milestone 3. Audio
buffers and the internal latched Loop state are not saved as LV2 state.

## Routing decisions

Behavior after the audit and the user's selected corrections:

1. Feed normally has no internal echo feedback. Loop therefore uses internal
   full-Size feedback in Feed, as in Out; otherwise cutting echo input would
   erase its contents. In continues to loop through the external Return.
2. Feed records only Repeats times Return, independently of Level and Wipe.
   Send carries only the echo tap. Memory dry is playback only, while Collect
   provides memory overdubbing. The rejected one-block memory-dry feedback
   buffer has been removed. This prevents a 375 Hz comb at a 48 kHz/128-frame
   period and restores unity repeats at every Level setting. Wipe mutes the
   direct echo output, but Feed returns can still be heard later through
   memory playback.
3. Scanning memory dry monitors only historical contributions. A present-time
   primary is omitted and Spread activates only when its snapped offset is
   positive. Explicit Memory dry On deliberately monitors the full mix; Dry
   kill can remove live dry when full memory playback alone is wanted.

The implementation plan and README describe these choices. Curves, glide
speed, fade times and excursion timing remain listening/tuning candidates.
The earlier full-distance 0.5-frame glide was rejected because reaching the
full memory depth took six minutes. The selected short scrub/crossfade keeps
the texture without that travel time.

## Offline checks

The actual shared library is loaded by the offline host. Main and Return use
independently randomized slices, with both execution orders covered by the
core tests. `tests/test_memory.cpp` adds:

| Check | Evidence |
| --- | --- |
| Audit regressions | Exact Pair delay after a mid-period change; sanitized-input comparison including an existing echo |
| Scan snap/clamp | 50 ms, 500 ms and 60 s grids; total offset never exceeds the guarded memory length |
| History validity | Reads beyond recorded or cleared history remain silent |
| Auto Scan | Ten simulated minutes of deterministic decisions, bounded depths, repeated returns, zero/max endpoints |
| Loop/Latch | Ten bit-exact trips in all routings, above the limiter threshold; new Size holds unchanged audio; release and rising edges |
| Collect | Two complete 180-second memory turns at the expected 0.7 and 0.49 amplitudes |
| Memory dry/Spread/Wipe | No dry doubling at present; manual history; equal-power Spread; Wipe forces memory dry |
| Feed with Wipe | No block-period feedback; historical returns remain on the Size grid |
| Bypass | Memory clear, paused Collect clock, Always recording, Trails clock/input behavior and echo tails |
| Bypass safety | Always disables Collect and limits feedback; Dry kill works on both sides of bypass |
| Transition smoothing | Constant-input bypass and Dry kill switches have bounded sample steps |
| CV/slicing | Scan and Spread CV engage playback, clamp at endpoints and give bit-identical output across host slicing |
| Scan hold | Overrides mode/CV, starts immediately between CV updates at maximum Size, snaps back on release |

`tests/test_m2_audit.cpp` independently checks all nine audit findings: full
depth and retargeting within 150 ms; audible local scrubbing and constant-gain
fades; Level-independent unity Feed; no block-period comb or growing monitor
feedback; Size-grid repeat timing with deep Scan; Manual hold returning to
10 seconds within 100 ms; snapped-zero Spread and no duplicate present dry;
short-bypass echo disposal and Loop release under Trails; startup/post-Clear
Collect fades; continuing RNG streams and distinct plugin instances; and
constant-signal interpolation in the first fractional sample behind present.

The full `make test` suite also retains the Milestone 1 timing, routing,
non-finite Return, maximum Repeats, pitch-preserving Size and WAV renderer
checks. Its ten-minute stress test now changes all memory and bypass controls.
The 1 kHz sample rate in the long-memory tests reduces runtime while exercising
the actual 180-second ring; the core also tests 44.1, 48 and 96 kHz.

Validation environments:

- Debian under WSL, native x86-64: `make -j4 test`.
- AddressSanitizer and UBSan: `make -j4 test SAN=1`, separate build directory.
- ThreadSanitizer: `make SAN=thread build/tsan/test_dsp`, then
  `setarch x86_64 -R build/tsan/test_dsp`. The per-process ASLR setting avoids
  WSL's sanitizer mapping conflict; no system setting is changed.
- Existing Debian arm64 root under QEMU: full `make -j4 test`
  with `BUILD_DIR=build/arm64 OPT="-O2 -mcpu=cortex-a76"`.
  The bundle is verified as an AArch64 ELF shared object. Emulation is not a
  physical Pi performance measurement.

The final corrected native x86-64 ten-minute stress run reported 96.082 microseconds
at the 99.9th percentile and 1528.06 microseconds worst per 128-frame period
(2667-microsecond budget). These elapsed WSL times include desktop scheduling
and do not establish a physical real-time or xrun guarantee.

The first 128-frame host capture attempt during concurrent sanitizer/arm64
builds exhausted its three retries on WSL scheduling xruns. Repeating after
those heavy jobs finished passed at 128 frames with both one and four threads.
The 1024-frame host checks also passed. This does not substitute for the
physical Pi xrun gate.

## Host and metadata checks

An isolated dummy JACK server loads both plugins in jalv and then Ingen at
periods of 128 and 1024 frames, with one and four processing threads. The
extended host test verifies Wipe overriding Memory dry Off, both Dry kill
behaviors during `enabled` bypass, and audio recorded by Always while that
port is zero. Ingen continues to run the block throughout these checks.
Existing checks cover exact echo timing/decay through external amplifiers,
BPM sync, a filter return path, cable feedback rejection, and saved
controls/Pair restoring with empty audio memory. Only this test's own child
processes are stopped. Logs are under ignored `build/host-smoke/`.

Staged installation and lv2lint 0.17.151 pass for both plugins. The remaining
warning is missing author email (`foaf:mbox`); no address is invented. The
extended lint run reports no allocation/syscall failures. The sole defined
dynamic export is `lv2_descriptor`.

## Remaining release gates

No physical Pi or guitar/listening session was available in this workspace.
Run the plan's 30-minute typical patch test on the Pi, record xruns, DSP load,
RSS and native timing, and listen to Scan transitions, Collect, Loop changes
and bypass. Peepo's overlay/pinned-build integration belongs in its repository.
The milestone's release checkbox and `v0.2.0` tag remain pending those checks.
