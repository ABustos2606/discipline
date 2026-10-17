# Discipline implementation plan

**Written:** 16 October 2026. **Follows:** [design.md](design.md), the spec. Where this plan and the spec disagree, the spec wins and this plan is fixed. **Checked against:** Ingen's source in Peepo's `reference/upstream/ingen/` and Peepo's docs 20, 22, 26 and 29.

This plan says how to build Discipline: code layout, ports, signal flow, how the pair works, real-time rules, tests and milestones. Behaviour that only listening can settle (curves, fade lengths, limiter settings) is given a starting value and marked **tune**.

## 1. What Ingen's source changes

Four facts from Ingen's source shape the design. The spec doesn't mention them.

1. **Engine bypass does not run the plugin.** `BlockImpl::process` (`src/server/BlockImpl.cpp:230`) calls `bypass()` when a block is disabled. That copies audio input *n* to audio output *n* and CV input *n* to CV output *n*, and never calls `run()`. Peepo's software bypass sets `ingen:enabled false` on the block (Peepo `docs/15-ingen-adapter.md`). With that bypass Discipline can't keep recording (ALWAYS), let echoes ring (TRAILS) or keep its memory while bypassed (COLLECT). **So Peepo must bypass Discipline through its `lv2:enabled` port, never through the block.** Peepo now does this for any plugin its overlay marks `"bypass": "port"` (see [Peepo-side work](#11-peepo-side-work)). Ingen gives that port no special treatment: it is a normal control.
2. **`run()` is called in slices.** In the same function, Ingen splits each period at every control value change and calls `run()` once per slice. A 128-frame period can arrive as runs of 37 and 91 frames. Nothing may assume one `run()` per period or a fixed `n_samples`. Smoothers advance per sample, not per call.
3. **Blocks run on several threads.** `threads` defaults to the number of cores (`src/Configuration.cpp:90`), which is 4 on the Pi. Blocks with no path between them can run at the same moment. Discipline and Discipline Return must share data through lock-free single-writer structures. When send is patched to return, Ingen's ordering puts Discipline first in the period. When they aren't connected, there is no order.
4. **Block length.** Ingen sets `bufsz:maxBlockLength` and `bufsz:nominalBlockLength` to the period (`src/server/LV2Options.hpp`). Discipline reads `nominalBlockLength` (falling back to `maxBlockLength`, then 128) to work out the return delay. It needs no other host features. `urid:map` is required only to read options; `options:options` is optional.

## 2. Technology and build

- **Plain LV2 C API in C++17, no DPF.** Both plugins must live in **one shared object**, so that they can share a pair registry through static storage. DPF builds one binary per plugin, so sharing would need a third library. The raw API is about 100 lines of glue for two descriptors.
- **License:** MIT. **Dependencies:** `lv2-dev` headers only (ISC license), which MIT allows. No Boost, no libsndfile in the plugin. The test tools may use a small WAV writer of their own.
- **One binary, `discipline.so`**, in `discipline.lv2/` beside `manifest.ttl`, `discipline.ttl` and `discipline_return.ttl`.
- **Makefile**, no configure step, no network:
    - `make` builds `build/discipline.lv2/`;
    - `make install` uses `DESTDIR`, `PREFIX` (default `/usr/local`) and `LV2DIR` (default `$(PREFIX)/lib/lv2`), so Peepo installs with `make install PREFIX=/opt/peepo` or `LV2DIR=/opt/peepo/lib/lv2`;
    - `make test` builds and runs the offline tests (section 9);
    - `make lint` runs `lv2lint` on the installed bundle, if it is on the path;
    - `OPT` overrides optimisation flags, the way Peepo's build scripts swap SSE flags for `-mcpu=cortex-a76`. Default `OPT = -O2`. Arch flags are never hard-coded; the Pi build passes `OPT="-O2 -mcpu=cortex-a76"`.
- **Flags:** `-std=c++17 -fPIC -fvisibility=hidden -Wall -Wextra`, with `-fno-math-errno -fno-trapping-math`. **Not `-ffast-math`**: Discipline relies on finite-value checks (section 8) that fast-math removes. Only `lv2_descriptor` is exported. Link `-shared -Wl,--no-undefined` and statically link `libstdc++` only if Peepo's arm64 root needs it (it hasn't so far).
- **Releases** are git tags (`v0.1.0` …). Peepo pins a commit, following `tools/ingen-spike/14-more-plugins.sh`.

### Repository layout

```
Makefile
LICENSE                      MIT text (milestone 0)
README.md                    what it is, build, install, the pair in one picture
discipline.lv2/
    manifest.ttl             both plugins, one binary
    discipline.ttl           ports of Discipline
    discipline_return.ttl    ports of Discipline Return
src/
    lv2_entry.cpp            lv2_descriptor(): index 0 Discipline, 1 Return
    ports.hpp                port index enums, the single source in C++
    discipline.hpp/.cpp      the main plugin: instantiate, activate, run, cleanup
    discipline_return.cpp    the return plugin
    pair_registry.hpp/.cpp   static slots shared by both plugins
    dsp/
        stereo_ring.hpp      fixed-size stereo ring buffer, power-of-two optional
        interp.hpp           4-point Hermite read at a fractional position
        smoother.hpp         one-pole and slew-limited smoothers
        xfade_tap.hpp        two read taps with an equal-power crossfade
        loop_gain.hpp        feedback-path level control (soft limiter)
        rng.hpp              xorshift32, no libc rand()
        auto_scan.hpp        the auto-scan state machine
        clock_out.hpp        Size clock trigger generator
        modifiers.hpp        selection, speed smoothing, Trimmer and Dropper
        speed_reader.hpp     captured Size windows, reverse and antialias reader
tests/
    host.hpp/.cpp            minimal in-process LV2 host: loads descriptors, slices runs
    test_*.cpp               one file per behaviour (section 9)
    ttl_check.py             checks .ttl indices and symbols against ports.hpp
tools/
    render.cpp               offline renderer: WAV in, settings, WAV out, for listening
docs/
    design.md, implementation-plan.md, the manual PDF
```

Every text file is LF (`.gitattributes` already says `* text=auto eol=lf`).

## 3. Ports

Port **symbols and URIs are permanent** once a patch has been saved with them. All ports, including those whose behaviour arrives in milestone 2 or 3, are declared in milestone 1. Until a port does something it is inert, which keeps symbols and indices fixed from the first saved patch. Ports added later (milestone 3 modifiers) are only ever appended, with `lv2:minorVersion` raised.

Names are short plain ASCII. They say what the control does.

### Discipline — `https://github.com/ABustos2606/discipline#discipline`

| # | Symbol | Name | Kind | Range | Default | Notes |
|---|---|---|---|---|---|---|
| 0 | `in_l` | In L | audio in | | | |
| 1 | `in_r` | In R | audio in | | | |
| 2 | `out_l` | Out L | audio out | | | main outputs first, so engine bypass would pass in to out |
| 3 | `out_r` | Out R | audio out | | | |
| 4 | `send_l` | Send L | audio out | | | to Discipline Return, directly or through modules |
| 5 | `send_r` | Send R | audio out | | | |
| 6 | `level` | Level | control | 0 – 1 | 0.5 | echo volume; curve **tune** (start: x², 1 = +6 dB) |
| 7 | `repeats` | Repeats | control | 0 – 1 | 0.4 | feedback; top 3% is exactly 1.0 (endless) |
| 8 | `size` | Size | control, ms, log | 50 – 60000 | 500 | used when Sync is off |
| 9 | `size_beats` | Size beats | control, unit beats | 0.125 – 128 | 1 | used when Sync is on; result clamped to 50 ms – 60 s |
| 10 | `sync` | Sync | toggle | 0/1 | 0 | Size follows BPM |
| 11 | `bpm` | BPM | control | 30 – 300 | 120 | set by Peepo's patch tempo |
| 12 | `spread` | Spread | control | 0 – 1 | 0 | 0 = off |
| 13 | `scan` | Scan | control | 0 – 1 | 0 | AUTO: how often and how far; MANUAL: position |
| 14 | `scan_mode` | Scan mode | enum | 0 Auto, 1 Manual | 0 | |
| 15 | `routing` | Routing | enum | 0 In, 1 Feed, 2 Out | 0 | where the return goes; default to check against the manual |
| 16 | `collect` | Collect | toggle | 0/1 | 0 | the memory overdubs itself |
| 17 | `loop` | Loop | toggle | 0/1 | 0 | held, or a rising edge flips it when Latch is on |
| 18 | `latch` | Latch | toggle | 0/1 | 0 | |
| 19 | `scan_hold` | Scan hold | toggle | 0/1 | 0 | momentary scan: Scan at maximum while 1 |
| 20 | `clear` | Clear | trigger | 0/1 | 0 | acts on a rising edge; `pprops:trigger` |
| 21 | `memory_dry` | Memory dry | enum | 0 Off, 1 Scanning, 2 On | 1 | |
| 22 | `wipe` | Wipe | toggle | 0/1 | 0 | echo muted, Memory dry forced On |
| 23 | `dry_kill` | Dry kill | enum | 0 Off, 1 Always, 2 Effect on | 0 | 1 = TRUE, 2 = SERIES in the manual |
| 24 | `always` | Always rec | toggle | 0/1 | 0 | memory records while bypassed |
| 25 | `trails` | Trails | toggle | 0/1 | 0 | |
| 26 | `pair` | Pair | integer | 1 – 16 | 1 | must match the Return's |
| 27 | `enabled` | On | toggle, `lv2:designation lv2:enabled` | 0/1 | 1 | bypass; hidden in Peepo |
| 28 | `level_cv` | Level CV | CV in | | | added to Level |
| 29 | `repeats_cv` | Repeats CV | CV in | | | |
| 30 | `size_cv` | Size CV | CV in | | | added on Size's log scale |
| 31 | `spread_cv` | Spread CV | CV in | | | |
| 32 | `scan_cv` | Scan CV | CV in | | | |
| 33 | `clock` | Size clock | CV out | | | 1 ms trigger at each echo boundary |
| 34 | `paired` | Paired | control out | 0/1 | | 1 when a Return with the same Pair is running |
| 35 | `modifier` | Modifier | enum | 0 Off, 1 Stepped speed, 2 Smooth speed, 3 Trimmer, 4 Dropper | 0 | appended in milestone 3 |
| 36 | `modify` | Modify | control | 0 - 1 | 0.5 | amount for the selected modifier |
| 37 | `modify_cv` | Modify CV | CV in | | | added to Modify, appended in milestone 3 |

Main's LV2 version is 0.2.0 (`lv2:minorVersion 2`) after appending the modifier
ports. Return stays at 0.1.1. All original symbols and indices remain unchanged.

Enums use `lv2:integer`, `lv2:enumeration` and `lv2:scalePoint`, counting from 0, so Peepo's overlay can name them as choices. Toggles use `lv2:toggled`. `size` uses `pprops:logarithmic`, and `pair` uses `lv2:integer`.

**CV scaling.** Each CV input is added to its knob in the control's *normalised* range (0 – 1 across the control's scale; logarithmic for Size), then clamped. A CV of +1 sweeps the whole range. Before milestone 1 closes, check what Peepo's LFO and random modules actually output (0 – 1 or −1 – 1). If most are unipolar, a `cv_depth`-style attenuverter isn't needed, because Peepo's CV utility modules can scale.

**CV read rate.** CV buffers are per sample. Level and Repeats use the CV per sample (they only scale gains). Modify uses its CV per sample too, because Speed and the rhythm gates smooth per sample and an LFO must not step. Scan, Spread and Size read their CV every 32 samples and feed it to their smoothers. That is cheap and fine enough for moves that glide or crossfade anyway.

### Discipline Return — `https://github.com/ABustos2606/discipline#return`

| # | Symbol | Name | Kind | Range | Default |
|---|---|---|---|---|---|
| 0 | `in_l` | In L | audio in | | |
| 1 | `in_r` | In R | audio in | | |
| 2 | `pair` | Pair | integer | 1 – 16 | 1 |
| 3 | `paired` | Paired | control out | 0/1 | |

The Return has no audio output. Check in milestone 1 that Peepo's canvas and catalogue export accept a module with no outputs. If they don't, add a silent `thru` pair of outputs (appending ports is allowed; renaming is not).

Both plugins declare `lv2:hardRTCapable` and `lv2:optionalFeature options:options`. Discipline requires `urid:map`. Return declares it optional and requires nothing. The class is `lv2:DelayPlugin`. The Return is `lv2:UtilityPlugin`, so the catalogue doesn't list it as a second delay.

## 4. The pair

### Registry

`pair_registry.cpp` holds a static array of 16 slots, one per Pair ID, inside `discipline.so`. Both plugins are loaded from that one object, so they see the same array. Each slot holds:

```cpp
struct PairSlot {
    std::atomic<void*>    owner_main;    // Discipline instance that claimed it, or null
    std::atomic<void*>    owner_return;  // Return instance that claimed it, or null
    std::atomic<uint64_t> written;       // frames the Return has written (release)
    std::atomic<uint64_t> main_frames;   // Discipline's frame counter (release)
    std::atomic<uint32_t> epoch;         // bumped on claims, release and resync
    std::atomic<uint32_t> written_epoch;
    std::atomic<uint32_t> block_length;
    struct Cell {
        std::atomic<uint64_t> stamp;     // epoch and frame tag; 0 while replacing
        std::atomic<uint64_t> stereo;    // two floats, bit-preserving packing
    };
    Cell ring[kReturnRing];              // kReturnRing = 8192 frames
};
```

- **The ring is static memory, owned by neither instance.** Its size is about 2 MB including tags. When a Discipline is deleted while its Return is still running on another thread, nothing is left dangling. All cells are page-touched once during instantiation, before either plugin runs.
- **Cells are atomic.** A plain float ring is not safe when an unconnected peer overtakes the reader or a slot is reused. The writer invalidates the stamp, replaces the packed stereo sample, then publishes its stamp. The reader checks the stamp, loads the stereo sample and checks the stamp again. These cell operations are sequentially consistent; a failed snapshot returns silence without spinning. The tag includes the epoch and the low 32 frame bits; the full 64-bit published write range excludes old or overwritten frames.
- **Claiming** is a compare-and-swap of `owner_main` (or `owner_return`) from null to `this`, done in `run()` when the Pair value changes. It is lock-free, so it is real-time safe. The old slot is released by compare-and-swap back to null. `deactivate()` and `cleanup()` also release.
- **A second instance with a taken ID** stays unpaired: no return audio, `paired` = 0. That covers a duplicated module in Peepo that copied the Pair value (section 11).
- **Patch load order doesn't matter.** Whichever of the two runs first claims its side. The other joins when it runs.

### Timing of the return

The Return writes each slice of its input into `ring` at index `written mod kReturnRing`, then publishes `written += n` with release ordering. Discipline reads the return at its own index `r` minus a fixed lag `B`, where `B` is the nominal block length.

- **Alignment.** `main_frames` is an exclusive end. Return starts at `floor((main_frames - 1) / B) * B`, plus its local phase within the period, with startup protected against underflow. This gives the current period's start whether main's first call was a full period or just a slice. On a main claim, the persistent slot timeline rounds up to a block boundary and adds the main's local block phase, so a previous owner's different block size does not offset the new pair and mid-period changes preserve timing. Zero-frame runs do not establish Return timing. In the patched case, send in period *k* returns in *k + 1*, exactly `B` frames later.
- **Unpatched or reversed order.** The raw return delay can be another constant. A reversed-order cable supplies the previous Send block. Atomic tags keep any overlapping access safe, including a peer that runs far ahead.
- **Stale return.** For each sample, main checks the published epoch, 64-bit write range and cell stamp. Missing frames fade out using the last valid sample over 64 frames. After 0.5 s of missing data `paired` becomes 0. New data fades back in. Return also detects a stopped main.
- **Slices and recovery.** Both sides count samples, independent of slice boundaries. A Return that resumes after missing a period must resync: otherwise its write counter remains behind forever. A gap beyond the normal one-block lead, or a writer over half the ring ahead, bumps the epoch and realigns. This is bounded work, with no ring clear or retry loop.

The test in section 9 sends an impulse from send to return through a cable, under random slicing and both run orders. It checks the delay is `B` in the patched case and constant in every other case.

## 5. Signal flow

Per sample, stereo. `M` is the memory, `E` the echo line, `ret` the return read for this sample. Gains that change are all smoothed (section 6).

```
                ┌─────────────────────── MEMORY M (180 s, always recording) ─┐
in ─┬─────────▶ write: in + collect·M[w] (+ FEED: rep·ret)  (limit recirculating parts only)
    │           read:  p = M[w - scan]      s = M[w - scan - spread]
    │                  mem = p + spread_on·s     (equal-power mix when Spread is on)
    │                                │
    │                                ├──────────────▶ memory dry (Off / Scanning / On)
    │                                ▼
    │           ECHO E (Size <= 60 s): write  e_in·(1 - loop) + fb
    │                tap = E[size - B]   (two taps, crossfaded on Size change)
    │                fb  = IN:   loopgain(rep·ret)
    │                      OUT:  loopgain(rep·E[size])
    │                      FEED: 0            (the loop goes through the memory instead)
    │                send = modifier(tap)
    │                wet  = ret              (in every routing)
    ▼
out = dry_gain·in + level·wet·(1 - wipe) + mdry_gain·mem
```

Feed takes Return before Level and Wipe. Memory dry is playback only. Bypass
scales the output and cuts the echo input as in 6.10; Loop's routing exception
is described in 6.9.

The rule that makes the three routings simple: **the echo you hear is always the return**. The send always carries the echo tap. Only the place the return is fed back into changes:

- **IN.** The return is the echo's feedback, so patched modules act again on every repeat (the main Modify sound).
- **OUT.** The echo feeds back on itself, unprocessed. The modules process each repeat once on the way out, so every repeat sounds the same.
- **FEED.** The echo has no ordinary internal feedback. Repeats times Return enters memory through the Feed limiter, independently of Level, Wipe and memory dry. Send carries only the echo tap. Memory dry is playback only; Collect provides its overdubbing. Live dry is excluded because memory already records it. This restores Milestone 1's return-only feedback after the Milestone 2 audit rejected the added one-block memory-dry path and Level coupling.

The spec takes it as given that the user always patches send to return. Without that, no echo is heard in any routing. The README and the Peepo module description say so.

**Echo tap compensation.** Send reads `size - B`, so send → return → output lands at exactly Size. IN and FEED include that trip in their loop. OUT's internal feedback instead reads at the full `size`; using the Send tap would shorten every OUT repeat by `B`. Both reads share the same crossfade phase. Instantiation rejects a nominal period of 50 ms or longer, or more than 4096 frames, because one-block transport cannot meet the advertised minimum Size in that configuration.

**Memory reads at "the present".** With Scan and Spread at 0, `p = M[w]` is the sample just written, so the echo is fed the live input (plus Collect). The memory acts as a pre-delay of 0.

## 6. Components

### 6.1 Buffers and allocation

- **Memory:** 180 s × sample rate × 2 channels, plus 4 guard frames for interpolation. 69.1 MB at 48 kHz. Sized from the rate given to `instantiate`, so 44.1 and 96 kHz also work (96 kHz is 138 MB, noted for the Pi's 2 GB).
- **Echo line:** 60 s + 50 ms crossfade margin + one max block, stereo. 23 MB at 48 kHz. Speed reads a captured window during the next one, up to two windows back, so its windows are capped at 30 s (6.12).
- **Allocated in `instantiate`** with `std::nothrow`. A failed allocation returns NULL (Ingen reports a failed instantiation) rather than throwing.
- **Pages are touched in `instantiate`** with a single `memset`. On Linux a fresh allocation is mapped lazily, so first writes in `run()` would otherwise page-fault in the audio thread. Instantiate touches about 92 MB at 48 kHz, then calls `mlock` on both buffers and ignores failure; JACK setups often allow it. Speed's roughly 0.22 MB of filter coefficients and its fade table are also populated before `run()`.
- **Positions are 64-bit frame counters.** They become indices only at access (`idx = pos % len`). The memory has its own clock (`mem_frames`), separate from the host's, because the memory can pause (6.10).

### 6.2 Clear without xruns: a logical clear

Clearing never touches the buffer. Discipline keeps `valid_from`, a memory frame counter. **Clear sets `valid_from = mem_frames`.** Every memory read (playback positions and Collect's read of `M[w]`) returns 0 for positions older than `valid_from`, with a 256-sample fade at that boundary so the edge doesn't click. Recording overwrites the old data over the next 3 minutes. The cost is O(1) whatever the buffer's size: no `std::fill`, no slices.

- The audible part of Clear is a 20 ms fade-out of every memory read before `valid_from` moves (**tune**), followed by a 256-frame fade-in. Old history is silent; fresh live input continues recording. A second trigger during fade-in begins from the current gain, avoiding a jump.
- Clear empties the memory only. Echoes already in the echo line ring out, as the manual's memory clear does. If listening shows it should also cut the echo, the echo line gets its own `valid_from` the same way.
- Bypass without COLLECT or ALWAYS uses the same clear (6.10).

The same trick makes startup clean. `valid_from` starts at 0, so nothing older than the instance is ever read.

### 6.3 Smoothing

- **Gains:** Level, Repeats, routing and Collect use 10 ms one-poles; Memory dry uses 50 ms. Dry kill, Wipe, bypass and echo input use 10 ms linear ramps with exact endpoints. Loop enters over 10 ms and releases over 50 ms. Every envelope advances per sample; a slice boundary only changes its target.
- **Scan and Spread:** a local scrub at up to 0.5 frame per sample, limited to 10 ms of history (at most 20 ms of scrubbing), accompanies a 60 ms crossfade to a large move's destination. Both outgoing and incoming audio scrub briefly. Targets coalesce during a fade, so an isolated or last pending move settles within 120 ms, below the 150 ms requirement. Snap-backs use 30 ms. Complementary raised-cosine weights avoid boosting correlated audio. The audit rejected full-distance gliding because a three-minute offset took six minutes to reach.
- **Size:** never glides, always crossfades (6.4).
- **Denormals.** Every feedback path flushes values below 1e-15 to 0. Discipline doesn't change the thread's FTZ/FPCR flags: they belong to the host thread.

### 6.4 Size by crossfading two taps

`xfade_tap` holds two integer tap delays, A (current) and B (incoming), and a crossfade phase.

1. Every 32 samples the target Size is worked out: knob or beats × 60/BPM, plus CV on the log scale, clamped to 50 ms – 60 s, converted to frames and rounded to an integer, minus the return lag.
2. If the target differs from the current tap by more than 1 frame and no fade is running, B is set to the target and an **equal-power fade** of 30 ms (**tune**) starts. The tap reads `cos·E[A] + sin·E[B]`. At the end A = B.
3. A target that changes during a fade waits. When the fade ends, the latest target starts the next fade. A Size CV that sweeps all the time therefore produces a chain of fades, at most one per 30 ms, with no pitch bend.
4. Integer tap delays need no interpolation.

The echo's feedback reads the same crossfaded tap, so the loop length changes with the same fade.

### 6.5 Repeats and loop gain

- Repeats maps the knob to feedback gain `g`: a smooth curve up to 0.97 of travel, then exactly 1.0 (**tune**).
- The implemented starting curve is `min(repeats / 0.97, 1)`. Level is `2 * level * level`. Gain smoothers and limiter envelopes keep double-precision state, so they can settle back to exact unity instead of stalling below it through float rounding. Repeats is applied before the feedback limiter. Stereo limiting shares an envelope and has a final ceiling of 4; the dry input and its first memory write remain linear.
- **`loop_gain`** sits only in the feedback path: the echo feedback in IN and OUT, and the return-to-memory path in FEED. It is a soft limiter. Below a threshold (start −6 dBFS) its gain is exactly 1.0, so normal playing is uncoloured. Above it the gain is reduced, with a fast attack (1 ms) and a slow release (300 ms). At Repeats = 1 echoes build up and then stay at a steady level, "like a loop", instead of running away. In IN it also catches gain added by the patched modules.
- Nothing nonlinear touches the dry path or the first pass of the input into the memory. That fixes the skeleton's `tanh` on every write.
- **Collect** (memory overdub) uses a fixed recirculation gain (start 0.7, **tune**) through its own `loop_gain`, so three-minute-old audio comes back quieter and never builds up.

### 6.6 Scan

The scan offset `S` (frames behind the present) comes from one of three sources:

- **MANUAL:** `S = scan · (memory length - margin)`, snapped to a multiple of Size (6.8), then reached with the bounded transition in 6.3. Scan 0 is the present.
- **AUTO:** the `auto_scan` state machine:
    - **Present.** At each Size boundary (from the clock, 6.11) roll `rng` against `p_jump(scan)`. Start: 0 at scan 0, rising to 1 at scan 1 (**tune**).
    - **Jump.** Pick a target depth: a whole number of Sizes, from 1 to `depth_max(scan)` (start: scan² × memory length, **tune**). Reach it using the bounded scrub/crossfade in 6.3; Away begins after the transition completes.
    - **Away.** Stay for 1 – 4 echoes (random, **tune**), then go back to Present: crossfade (30 ms) from the scanned read position to the present position. That is the "snap back".
    - At scan = 1, `p_jump` is 1 and the hold is endless, so it never comes back to the present: "scans all the time".
- **Scan hold** (momentary scan) forces the AUTO machine to behave as at scan = 1 while held, whatever the mode. Pressing it starts an excursion immediately, including at long Size settings. On release it crossfades to the current snapped Manual target in Manual mode, or the present in Auto. State resets preserve the random stream; instances receive distinct seeds during instantiation, while tests can supply explicit repeatable seeds.
- **Scan CV** adds to the knob before either mode reads it.

Crossfades between two memory read pairs use complementary weights, unlike the echo's equal-power Size fades in 6.4. Memory has up to four interpolated reads per sample: primary, spread, and a fade partner for each. The fractional segment next to the present uses linear interpolation because the fourth Hermite sample has not been written yet.

### 6.7 Spread

The second read position is `S + P`, where `P = spread · (memory length - S - margin)`, snapped to a multiple of Size. Spread is off whenever snapped P is zero, regardless of the raw knob value. Position changes use 6.3's bounded transition. With Spread on, the two positions are mixed at `0.707` each (**tune**). The spread position moves with Scan because it is relative to `S`. Scanning memory dry omits a present-time primary rather than adding it to live dry again.

### 6.8 Clamping and snapping

- **Snap:** `S` and `P` are rounded to a whole number of Sizes, so taps stay in rhythm. At Size 60 s that leaves three positions; that is what the spec asks for.
- **Clamp:** `S + P ≤ memory length - (max block + 4 + crossfade)`. Without it the read position would wrap round and play the newest audio, with a click (one of the skeleton's faults). The clamp applies after snapping: a snapped value over the limit drops to the previous multiple.
- Reads older than `valid_from` are silent (6.2). So right after start or Clear, a deep scan plays silence until the memory has filled that far. That is correct.

### 6.9 Loop

- **Loop on:** the echo line's input is muted (`e_in` × 0, faded over 10 ms), and its feedback is forced to exactly 1.0 with `loop_gain` bypassed, so the content recirculates unchanged. The memory keeps recording. The user can play over it: the dry signal is unaffected.
- In **IN**, the loop runs through the return, so modules keep acting on it. That is the patched equivalent of the hardware, and it is allowed.
- In **FEED**, Loop uses the internal full-Size feedback, like OUT, while the memory keeps recording. Without this exception, muting the echo input would erase the held echo because ordinary FEED has no echo feedback.
- Size changes crossfade as in 6.4. The loop keeps its contents but its length changes, so it is never erased.
- **Loop off:** feedback fades back to Repeats over 50 ms.
- **Latch:** with Latch off, Loop is active while the port is 1 (Peepo's momentary stomp). With Latch on, each rising edge flips an internal state.

### 6.10 Memory dry, Wipe, Dry kill and bypass

- **Memory dry:** Off is silent. On monitors the full memory mix. Scanning monitors historical contributions only, with the present primary omitted, and fades over 50 ms. A short taper when a head leaves/reaches the present avoids a discontinuity. A raw Spread value that snaps to zero cannot activate it. Explicit On can double live dry at the present; Dry kill is available for memory-only playback.
- **Wipe:** the echo (`wet`) is muted and Memory dry is forced to On.
- Feed still records Return while Wipe is on; processed audio can therefore be heard later through memory playback. Wipe does not create a separate memory feedback path.
- **Dry kill:** Off passes dry. Always (TRUE) never passes dry, even when bypassed. Effect on (SERIES) mutes dry only while the effect is on.
- **Bypass** (the `enabled` port, 1 → 0; 10 ms crossfade on the output):

| | Output while bypassed | Echo | Memory |
|---|---|---|---|
| Neither ALWAYS nor COLLECT | dry (or silence with Dry kill Always) | muted, or rings out with TRAILS (input to echo cut) | **cleared** (6.2) |
| COLLECT | as above | as above | **paused**: `mem_frames` stops and reads stop moving. With TRAILS, keeps moving and overdubbing but takes no new input |
| ALWAYS | as above | as above | keeps recording. For safety Collect is treated as off and Repeats is limited to 0.7 while bypassed (**tune**) |

When it is switched back on, nothing jumps: positions are relative to `mem_frames`, which was either paused or kept running.

Without Trails, bypass fades wet to zero and logically invalidates the echo
before fading it back in, even if the effect is re-enabled during that fade.
Old Return samples are excluded at that boundary. With Trails, held or latched
Loop releases over 50 ms to ordinary Repeats; maximum Repeats can still sustain
unity feedback, as it can during normal use.

### 6.11 Tempo and the Size clock

- Size in beats = `size_beats × 60 / bpm` s, clamped to 50 ms – 60 s. A BPM change is a Size change, so it crossfades.
- **Size clock** (`clock`): a phase counter of length Size (the current tap A). At each wrap it writes 1.0 for `round(rate * 0.001)` frames, then 0. It is also the beat that Auto Scan uses for its decisions. When Size crossfades, the counter adopts the completed tap's length at its next wrap. Activation starts with a pulse at sample zero. Bypass, Loop, Clear and Pair changes leave the phase running. This drives Peepo's clock divider in the Ingen host check.

### 6.12 Built-in modifiers

- The user selected all four proposed modifiers. `modifier` selects one at a
  time; `modify` is its normalized amount, plus `modify_cv` read per sample
  and clamped to 0 - 1. Off is the default. Mode weights,
  amount and speed use 10 ms one-poles; all state advances per sample.
- Processing is on Send, ahead of any patched effects. IN recirculates the
  modified Return; OUT keeps its full-Size internal feedback unmodified; FEED
  records Repeats times modified Return into memory. Live dry and memory dry
  are unaffected. External filters and stability effects remain patchable.
- Stepped Speed's 15 equally spaced settings are -4, -3, -2, -1.5, -1, -0.75,
  -0.5, 0, 0.5, 0.75, 1, 1.5, 2, 3 and 4. Smooth Speed is -2 through +2,
  stopped at noon, unity at 0.75 (with a small unity detent). Speed glides
  intentionally; Size still changes through the original fixed-tap crossfade.
- Speed references completed Size windows without copying audio. A new window
  starts each Size with a 1 ms complementary raised-cosine crossfade. Reverse
  starts at its last sample and runs backward across the full window. Slow
  playback reads only part before the next restart; fast playback wraps within
  the window, with a short fade at wraps. Window boundaries clamp interpolation
  neighbors, so reads never pull future or unrelated audio into a captured clip.
  If a wrap fade overlaps a Size restart, the next fade starts from the last
  mixed sample, retaining continuity without adding a third read head.
  A Size commit inside Send's one-period lead corrects the window relative
  to that same boundary, rather than retaining an old length for a full period.
- A window is read while the next is recorded, so it can be up to two windows
  old. To fit the 60 s echo line, Speed windows are capped at 30 s: above a
  30 s Size each echo is split into two halves, each captured and played on
  its own. Non-unity speeds then play the newest half, so forward playback
  starts about Size/2 behind instead of Size. Trimmer and Dropper keep the
  full Size.
- Hermite interpolation handles rates up to unity; faster playback uses a
  32-tap windowed-sinc antialias filter, with precomputed cutoff/fractional-phase
  tables. No coefficient generation occurs in `run()`. Positive unity crossfades
  to the ordinary tap and becomes bit-exact. At stop a 5 ms gain ramp removes
  held DC. Snapshot restarts deliberately change transient placement and startup
  audio; they are a rhythmic effect rather than a latency-neutral pitch shifter.
- Trimmer's left half cuts the start, its right half cuts the end. Noon is
  bit-exact neutral and both endpoints silence the echo. Its shaped edges and
  gain ramp span 1 ms. Dropper uses a repeatable 16-step hash pattern, increasing
  dropout density with amount. The top 10% crossfades to a roughly 5 ms grain
  grid. Zero is bit-exact neutral; maximum still contains sound and gaps.
- Rhythm gates and captured windows use the clock phase advanced by the host
  period, compensating for Return transport. Echo discard/fault invalidates
  captured readers in constant time; Clear remains memory-only.

### 6.13 Output and safety

- A final finite check per slice: if any output sample is NaN or infinite (a module in the loop can produce that), Discipline resets the echo line's feedback state, writes silence for that slice and raises an internal flag. It doesn't throw or log in `run()`. Ingen must never receive a non-finite value (Peepo's catalogue withholds plugins that output one).
- Non-finite live input is zeroed only at the affected channel/sample. It does not trigger the feedback fault reset or erase the rest of the slice.
- Return audio passes through `loop_gain`'s ceiling before it reaches the memory or the echo, so a module with heavy gain can't overload the buffers.
- Level: Peepo applies its general Output after the plugin (`peepo:outputLevel`), so Discipline has no output trim of its own.

## 7. Per-sample loop

`run(n)` does only this, in order. It doesn't allocate, lock, do I/O or loop over a whole buffer.

1. Read control ports. Detect rising edges (Clear, Loop with Latch, Scan hold). Handle Pair changes (claim or release a slot). Set smoother targets.
2. Read the return slice availability (`written`, acquire). Publish `main_frames` at the end.
3. For each sample:
    1. advance smoothers; every 32 samples, update Size, Scan and Spread targets from knob + CV, plus auto-scan;
    2. `ret` = return ring at `r - B` (or 0 if stale), through the ceiling;
    3. memory: read `M[w]` for Collect (silent if older than `valid_from`), write the new sample unless paused;
    4. memory reads: primary, spread, and fade partners, Hermite-interpolated when fractional;
    5. echo: read crossfaded tap, compute feedback for the routing, write `E`;
    6. send = modifier(tap); wet = ret; out = dry + level·wet + memory dry, with bypass and Dry kill gains;
    7. clock output.
4. Finite check (6.13). Advance counters.

**Budget.** Ordinary delay/memory processing has a few interpolated reads per
sample. Fast Speed can add two 32-tap stereo reads during a restart fade. Work
stays bounded, but the former rough CPU estimate does not cover these filters.
Measure worst-case `run()` and whole-patch xruns on the physical Pi (section 9).

## 8. Real-time rules, checked

- `run()` and anything it calls are marked in the source with a comment block heading the file section, and are covered by the timing test.
- `-fno-exceptions` is not used (C++17 containers outside `run()` are fine). But nothing in `run()` can throw: no `std::vector` growth, no `std::string`, no `new`.
- No `printf` and no `lv2_log` in `run()`. Faults set flags. A debug build may report them from `deactivate()`.
- Atomics are only `std::atomic<uint64_t/uint32_t/void*>`, checked lock-free with `static_assert(std::atomic<uint64_t>::is_always_lock_free)` (true on arm64 and x86-64).
- No `std::fill` or `memset` over the buffers outside `instantiate` and `activate`. `activate` may reset counters, not clear memory.

## 9. Tests

### 9.1 Offline (`make test`, desktop and Pi, no host needed)

`tests/host.cpp` loads both descriptors from the built object, connects buffers, and calls `run()` with **random slice sizes** that sum to a period, as Ingen does. It can run the two plugins in either order per period, and route send to return through a callback (a cable, a gain, a filter, a NaN generator). Each test prints one PASS or FAIL line; any FAIL makes `make test` fail.

| Test | Check |
|---|---|
| `ttl_check` | every port in the `.ttl` files matches `ports.hpp`: index, symbol, kind; names ASCII ≤ 16 characters |
| `pair` | claim and release; second instance with the same ID unpaired; both creation orders; Discipline deleted while Return keeps running (no crash, run under ASan) |
| `return_latency` | impulse through a cable: delay is exactly `B` patched, constant otherwise, at B = 64, 128, 256 and random slicing |
| `echo_time` | first echo of an impulse lands at Size ±1 frame through the return, Size 50 ms, 500 ms, 60 s, ms and beats at 30/120/300 BPM |
| `size_no_pitch` | a 1 kHz sine through the echo while Size moves: the output frequency stays at 1 kHz ±0.5% outside the crossfades (zero-crossing count) |
| `routing` | with a −6 dB gain as the patched module: IN repeats decay by Repeats × 0.5 per trip, OUT by Repeats with each repeat 6 dB down once, FEED's audio reappears from the memory |
| `repeats_max` | Repeats 1, 60 s of noise bursts: level stays bounded and never silent; dry path bit-exact when wet is muted |
| `clear` | after Clear, memory reads are silent within 20 ms + 256 samples; `run()` time unchanged by Clear |
| `scan_clamp` | Scan 1 + Spread 1 at Size 60 s: read positions never newer than allowed, never wrap; reads before `valid_from` silent |
| `scan_snap` | Manual scan positions are whole multiples of Size |
| `auto_scan` | over 10 min at scan 0.5: jumps happen, always return, depth within limits; seeded RNG, so the result can be repeated |
| `loop` | Loop holds the echo bit-exact for 10 trips; Latch flips on rising edges only |
| `bypass` | the three rows of 6.10's table; Dry kill both ways; no click (max sample step below a threshold) at each switch |
| `cv` | CV adds and clamps; Size CV sweeping makes at most one fade per 30 ms |
| `nonfinite` | a NaN from the patched module: output stays finite, recovers |
| `rt_timing` | 10 simulated minutes with random control changes: report the worst and the 99.9th-percentile `run()` time per period |
| `m3_clock` | Size clock pulses: exact period and 1 ms width, committed Size, BPM, CV, lifecycle |
| `m3_speed` | measured pitch, stop without DC, full-window reverse up to 30 s, 30 s chunks above |
| `m3_alias/neutral`, `m3_rhythm` | antialiasing; bit-exact neutral settings; Trimmer halves; repeating Dropper |
| `m3_transitions`, `m3_routing/discard` | bounded mode changes, slice independence, IN/OUT/FEED, bypass discard |
| `m3_modify_cv` | Modify CV adds and clamps, reaches exact neutral and Stepped settings, per sample |

The tests build with `-fsanitize=address,undefined` on the desktop (`make test SAN=1`).

### 9.2 Desktop hosts

- `lv2lint` on both plugins: no errors. Document any warning that stays.
- `jalv` and Carla. Carla can patch Discipline → effect → Return as two plugins in its patchbay, which gives a quick listening setup without Ingen.
- `tools/render` renders a guitar DI file through fixed settings to a WAV, for listening and before/after comparisons while tuning.

### 9.3 Ingen under WSL

With Peepo's `tools/ingen-spike/env.sh`:

1. Load both plugins and patch send → a filter → Return. Check `COMPILATION_FAILED` doesn't occur. Then try a cable from the external effect's output to Discipline's input to confirm ordinary feedback is rejected while the pair works. Return itself has no audio output.
2. Save and reload a patch: controls and Pair come back, the buffer doesn't (expected).
3. Run with `-p 1` and `-p 4` (threads): same `return_latency` result measured with a test signal.
4. Engine bypass (`ingen:enabled false`) passes in → out and leaves Send silent. The `enabled` port does the real bypass.

A Peepo-side script in the style of `11-clouds.sh` or `15-compat.sh` records these.

### 9.4 On the Pi

A milestone is done only after this. Build natively on the Pi (or in the arm64 root) at the tagged commit. Run a typical patch (input → Discipline → TAP Pitch → Return, plus a Clouds and an amp) for 30 minutes with guitar or a looped DI, with Clear, Scan and Size changes and Loop. Record:

- the JACK xrun count, which must be 0 beyond what the same patch without Discipline produces;
- the DSP load;
- RSS;
- `rt_timing` run natively.

Results go to Peepo's `docs/evidence/`.

## 10. Milestones

Each has an end check. "Done" means the offline tests pass on x86-64 and arm64, plus the stated host checks.

### Milestone 0 — scaffolding

- License: MIT (chosen 16 October 2026). Add `LICENSE` with the standard MIT text and the line `Copyright (c) 2026 Alejandro Bustos`. Put `SPDX-License-Identifier: MIT` at the top of every source file, header, Makefile and `.ttl` file, and declare `doap:license <https://spdx.org/licenses/MIT>` on both plugins in the `.ttl` files (`lv2lint` checks it).
- Makefile, both descriptors, both `.ttl` files with **the full port list of section 3**, `ports.hpp`, and the `ttl_check` test.
- Discipline passes input to output and outputs silence on Send. Return claims its slot.
- **Done when:** `make`, `make install DESTDIR=…` and `lv2lint` are clean, and both plugins load in jalv and in Ingen under WSL. Tag `v0.0.1`, so Peepo can wire its build step early.

**Implementation status, 16 October 2026:** scaffolding is implemented, including
all 35 main ports and 4 Return ports. Offline tests pass on x86-64 (also under
ASan/UBSan) and arm64 under emulation. Staged installation, jalv and Ingen with
one and four processing threads pass. `lv2lint` has no errors and two documented
metadata warnings per plugin. See [the validation record](milestone-0-validation.md)
for coverage and limits. The physical Pi check and release tag remain pending.

### Milestone 1 — the core (spec milestone 1)

1. `stereo_ring`, `interp`, `smoother`, `xfade_tap`, `loop_gain`, with unit tests.
2. Memory and echo allocation, page touching, `mem_frames`, `valid_from` and logical Clear.
3. Pair registry, the return ring and latency alignment (`pair`, `return_latency`).
4. Echo line with Size by crossfade, ms and beats, BPM, tap compensation (`echo_time`, `size_no_pitch`).
5. Repeats with `loop_gain`. IN / OUT / FEED as in section 5 (`routing`, `repeats_max`).
6. Level, CV inputs for Level, Repeats and Size. Scan and Spread CV arrive with milestone 2's Scan and Spread, but are already declared.
7. `nonfinite`, `rt_timing`.
- **Done when:** all tests above pass. A patch in Ingen under WSL plays echoes through a module in all three routings. Then the Pi check (9.4) shows no added xruns. Tag `v0.1.0`. Peepo adds its overlay entry and pinned build.

**Implementation status, 16 October 2026:** the core, its tests and an offline
WAV renderer are implemented. The full suite passes on x86-64, with ASan/UBSan,
and on arm64 under emulation. The return transport passes ThreadSanitizer.
Ingen at 128 and 1024 frames, with one and four threads, passes exact echo
timing/decay in every routing, BPM sync, a filter return path, feedback rejection
and save/reload with empty audio buffers. See [the validation record](milestone-1-validation.md).
The physical Pi xrun check, listening, release tag and Peepo integration remain
release follow-up. Milestone 2's controls are implemented below.

### Milestone 2 — memory features (spec milestone 2)

1. Scan MANUAL with snap, clamp and glide. Then Scan AUTO (`auto_scan`) and Scan hold.
2. Spread.
3. Collect with its own `loop_gain`.
4. Loop and Latch.
5. Memory dry (three modes), Wipe, Dry kill.
6. Bypass through `enabled`: ALWAYS, COLLECT, TRAILS (in Peepo, through the overlay's `"bypass": "port"`, section 11).
- **Done when:** `scan_clamp`, `scan_snap`, `auto_scan`, `loop`, `bypass` and `cv` pass. There has been a listening session on the Pi with a guitar, and a Pi xrun check. Tag `v0.2.0`. Expect several `v0.2.x` tags of tuning after this.

**Implementation status, 16 October 2026:** memory controls and their offline
and Ingen checks are implemented. Following the Milestone 2 audit and user
decisions, bounded scrub/crossfade moves replace full-distance glides, Feed is
return-only and independent of Level, and the smaller audit findings have
dedicated audio regressions. The three earlier audit corrections
are included: Pair claims preserve the local block phase on both sides;
non-finite live input is zeroed per channel without resetting the echo; Return
declares optional `urid:map`. See [the validation record](milestone-2-validation.md)
for results and the remaining physical Pi/listening/release gates.

### Milestone 3 — clock and modifiers (spec milestone 3)

1. Size clock output (6.11), checked with a Peepo trigger module, for example a clock divider.
2. Decide which built-in modifiers to add, from the spec's list: **Speed** (stepped and smooth, including reverse) and **Trimmer** need the echo's timing; **Dropper** in pattern mode needs per-echo position.
    - Speed and reverse read the echo line at a variable rate from the tap: a second interpolated reader that restarts at each Size boundary, with a short crossfade at each restart.
    - Each chosen modifier gets a selector and an amount, **appended** as new ports (`modifier`, `modify`), with `lv2:minorVersion` raised.
- **Done when:** the clock triggers a module in step with the echo, and each chosen modifier has a test and a listening note. Tag `v0.3.0`.

**Implementation status, 17 October 2026:** the Size clock and all four
user-selected modifiers are implemented, including appended ports (Modifier,
Modify, Modify CV) and the minor-version bump. Speed windows are capped at 30 s
so the echo line stays at 60 s. Offline checks cover clock phase, measured pitch, full
reverse windows, alias suppression, neutral settings, rhythm, routing,
transitions and slicing. Ingen checks the clock through Peepo's actual clock
divider. [Milestone 3 validation](milestone-3-validation.md) records results and
reproducible listening comparisons. Physical Pi xruns and listening/tuning are
pending, so the release gate and `v0.3.0` tag remain open.

## 11. Peepo-side work

These are in Peepo's repository, not here. They are listed so the plugin doesn't wait on them unexpectedly.

1. **Bypass through the port. Done in Peepo.** Peepo's catalogue export treats a control named `enabled`, or one with `lv2:designation lv2:enabled`, as the module's on/off switch (`tools/export_lv2_catalogue.py:67`). By default it keeps that switch at 1 and uses the engine's block bypass. An overlay rule with `"bypass": "port"` reverses this: software bypass, stomp bypass and saved patches set the plugin's switch to 0, and the block stays enabled (`ingen:enabled` is never set false). A copy keeps the bypass, and a module whose switch reads 0 is shown as bypassed. Peepo refuses the rule for a plugin with no such switch, so Discipline's `enabled` port must keep its symbol or its `lv2:enabled` designation. `"bypass": "engine"` gives the usual behaviour back. See Peepo `docs/16-catalogue-export.md` ("A plugin that bypasses itself") and `docs/15-ingen-adapter.md`; the tests use a stand-in plugin, so milestone 2 still has to check it with Discipline in Ingen.
2. **Overlay entry** (`config/catalogue/overlay.json`). Name "Discipline", category Delay. Description, for example: "Delay with a 3-minute memory. Cable Send to a Discipline Return, through any modules." Then:
    - `"bypass": "port"` (item 1);
    - `"tempo": "bpm"`;
    - `size_beats` with unit `beats`;
    - enum choices for `scan_mode`, `routing`, `memory_dry` and `dry_kill`;
    - `pair` as whole numbers; `enabled` and `paired` hidden;
    - suggested groups of four: **Echo** (Level, Repeats, Size, Routing); **Memory** (Scan, Spread, Scan mode, Collect); **Play** (Loop, Latch, Scan hold, Clear); **Sound** (Memory dry, Wipe, Dry kill, Sync); **Bypass** (Always rec, Trails, Pair). Size beats and BPM go on More.

    The Return gets the description "Brings the sound from Discipline's Send back in. Set the same Pair.", category Delay or Utility.
3. **Pair on add and duplicate.** When a Discipline is added, the interface could pick the lowest Pair that no Discipline in the patch uses, and a Return added next could take the same value. Duplicating Discipline should do the same. Otherwise the copy stays unpaired, by design (section 4).
4. **Build step**, pinned commit: `make && make install LV2DIR=/opt/peepo/lib/lv2 OPT="… -mcpu=cortex-a76"` on arm64, following the existing `fetch` / `step` pattern of `14-more-plugins.sh`.
5. **A module with no outputs.** Check that the canvas and the export handle Discipline Return (section 3).

## 12. Open questions and risks

| Item | Why it matters | When to settle |
|---|---|---|
| Default routing | The manual describes the routings but does not prescribe a default. Keep the published project default, In (0). | Settled for Milestone 1 |
| Dry kill's three states | Preserve published Off/default; Always and Effect on implement the manual's TRUE and SERIES behavior. All three are checked enabled and bypassed. | Settled for Milestone 2 |
| Peepo CV range | AMS tempo LFO has unipolar and bipolar modes; Marbles emits 0-0.4, 0-1 or -1-1. Add normalized CV and clamp; existing CV utilities handle depth. | Checked in source for Milestone 1; see validation record |
| Return with no outputs in Peepo | Might need a silent pass-through port | Milestone 0 |
| Peepo bypass via the port | ALWAYS / COLLECT / TRAILS can't work through engine bypass. Peepo uses `"bypass": "port"` (section 11). Real-plugin enabled-port checks pass in Ingen under WSL with one and four threads, at 128/1024 frames. | Physical Pi check pending |
| Memory at high sample rates | Two instances at 96 kHz are 460 MB | Note it in the README; Peepo runs at 48 kHz |
| Sound tuning | Curves, fades, limiter, auto-scan feel: months, per the spec | `v0.2.x` and later, via `tools/render` and listening on the Pi |
| Modifier ports | Appending ports later is safe for Ingen (it saves by symbol) but bumps the minor version | Milestone 3 |
