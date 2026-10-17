# Milestone 1 validation

Recorded 16 October 2026. The delay core is implemented. Physical Pi validation,
guitar listening and the `v0.1.0` release tag remain pending, as do the overlay
and pinned build in Peepo. No Pi performance or musical-tuning claim is made.

| Check | Result |
| --- | --- |
| `make -j4 test`, x86-64 Debian under WSL | PASS; all DSP, lifecycle, audio, renderer and timing tests |
| `make -j4 test SAN=1` | PASS; ASan and UBSan on the shared library and test host |
| `make -j4 test BUILD_DIR=build/arm64 OPT="-O2 -mcpu=cortex-a76"` | PASS; actual AArch64 library, tests and renderer under QEMU in the existing Debian 12 root |
| ThreadSanitizer transport check | PASS; `make SAN=thread build/tsan/test_dsp`, then `setarch x86_64 -R build/tsan/test_dsp` |
| Staged `make install` and `lv2lint` | PASS with the author-email warning below; both plugin identities use one shared object |
| Exported symbols | Only `lv2_descriptor` |
| jalv | Both plugins load and run; main preserves dry stereo and emits delayed audio on Send |
| Ingen, 128 and 1024 frames, 1 and 4 processing threads | PASS; exact 50 ms stereo echoes and repeat levels through external -6 dB amplifiers in IN, OUT and FEED |
| Ingen BPM sync | PASS; 0.5 beats at 300 BPM produces a 100 ms echo |
| Ingen external filter | PASS; a FOMP low-pass filter between Send and Return produces audible-level test output without a graph cycle |
| Ingen feedback rejection | PASS; an ordinary cable from the external effect back to the main input is rejected and the engine survives |
| Ingen save/reload | PASS; Size, Repeats and Pair 7 restore, old repeating audio is discarded, and the reloaded pair produces new echoes |
| WAV renderer | PASS; mono PCM16/24/32 and float32 inputs render to stereo float32 with the first echo exactly 2400 samples later at 48 kHz |

The host checker creates its own named dummy JACK server, private bundle copy
and engine sockets, then stops only its own processes. Logs and saved test
patches are under `build/host-smoke/`. Peepo's source, installed bundles and
instrument services were not changed. The temporary arm64 workspace mount was
removed after the tests.

Offline coverage:

- Buffer wraparound, startup silence, integer and Hermite reads, logical
  invalidation, and silence for fractional positions before valid history.
- Per-sample one-pole and slew smoothing; exact unity after settling; Clear's
  20 ms fade and 256-frame recovery; fixed Size taps with 30 ms equal-power
  fades and queued target changes.
- Limiter unity below threshold, linked stereo gain, overload ceiling,
  release back to unity and denormal flushing. The limiter affects feedback;
  the dry path is bit-exact at Level zero, including signed zeros.
- Both pair run orders, collisions, changes, activation, deactivation and
  cleanup while the other side runs. Concurrent ring overwrite returns a
  coherent stereo frame or silence. Half-second stale status and recovery
  after a stopped Return are tested.
- Raw return latency at 64, 128 and 256 frames with independently randomized
  slices. Main-first raw input returns after exactly one period. Reversed
  order has a constant raw-input delay; its cable uses the previous Send block.
- First echoes at 50 ms, 500 ms and 60 s, at 44.1/48/96 kHz. Beats cover
  0.125, 1 and 128 at 30/120/300 BPM, including time clamps. Typed block options,
  nominal/maximum precedence and the 128-frame fallback are checked.
- All routings have exact Size-spaced repeats through a half-gain effect.
  Clear leaves IN echo feedback intact while attenuating FEED's memory path.
  A 1 kHz tone remains within 0.5% outside Size-change crossfades.
- Level CV responds to a single-sample pulse; normalized gain CVs clamp at
  both ends; Size CV spans the full logarithmic range. NaN/Inf from inputs or
  Return never escape and processing recovers.
- Sixty simulated seconds of noise bursts at Repeats 1 through an external
  gain of 8 remain finite, bounded and audible. Ten simulated minutes of
  random Level, Repeats, Size, Clear and Routing changes report worst and
  99.9th-percentile callback times.

Timing sums the main and Return `run()` calls per 128-frame period, excluding
host port connection and signal-generation work. It measures wall time and
includes OS preemption. The final x86-64 run reported a 312.965 us p99.9 and
10494.6 us maximum while sanitizer and QEMU jobs also ran; the emulated arm64
run reported 505.389 us and 5399.55 us. These figures do not establish a native
Pi audio deadline: the period at 48 kHz is about 2667 us and desktop scheduling
outliers exceed it. Native Pi load, RSS and comparative xrun measurements are
still required by implementation-plan section 9.4.

`lv2lint` 0.17.151, upstream revision
`9661902c922650de99bd68e69ee0380d299aed3e`, reports no errors. Online, ELF and
X11 tests were disabled; exports were checked separately. Its remaining
warning on each plugin is **Plugin Author Email**, because no `foaf:mbox` has
been supplied. No email was invented. Both plugins now declare supported
nominal/maximum block options, so the scaffold's options warning is resolved.

Three implementation-plan corrections were required and are now recorded in
that plan: atomic tagged cells protect against overwrite and re-pair races;
a resumed Return realigns instead of staying permanently behind; and OUT
feedback reads at full Size while Send reads at Size minus one period. The
shared transport is therefore about 2 MB instead of the original 1 MB estimate.

Peepo CV ranges were checked in the readable upstream source: AMS
`avwlv2/src/lfo_tempo.cpp` implements unipolar and bipolar sine, triangle, saw
and sample-and-hold modes (the rectangle output remains bipolar);
`lenticular_lv2/src/polymarbles.cpp` divides Marbles' voltages by five, giving
0-0.4, 0-1 or -1-1 according to its range. The normalized additive CV contract
is retained without adding ports. The default Routing stays In (0), the
existing project choice; the reference manual does not identify a default.

Memory scanning, Spread, Collect, Loop/Latch, Memory dry, Wipe, Dry kill and
On/Always/Trails behaviour are Milestone 2 work. Clock output is zero until
Milestone 3. All existing port symbols and indices are unchanged. Neither
plugin saves audio buffers as LV2 state.
