# Discipline implementation plan

**Written:** 16 October 2026. **Follows:** [design.md](design.md), the spec. Where this plan and the spec disagree, the spec wins and this plan is fixed. **Checked against:** Ingen's source in Peepo's `reference/upstream/ingen/` and Peepo's docs 20, 22, 26 and 29.

This plan says how to build Discipline: code layout, ports, signal flow, how the pair works, real-time rules, tests and milestones. Behaviour that only listening can settle (curves, fade lengths, limiter settings) is given a starting value and marked **tune**.

## 1. What Ingen's source changes

Four facts from Ingen's source shape the design. The spec doesn't mention them.

1. **Engine bypass does not run the plugin.** `BlockImpl::process` (`src/server/BlockImpl.cpp:230`) calls `bypass()` when a block is disabled. That copies audio input *n* to audio output *n* and CV input *n* to CV output *n*, and never calls `run()`. Peepo's software bypass sets `ingen:enabled false` on the block (Peepo `docs/15-ingen-adapter.md`). With that bypass Discipline can't keep recording (ALWAYS), let echoes ring (TRAILS) or keep its memory while bypassed (COLLECT). **So Peepo must bypass Discipline through its `lv2:enabled` port, never through the block** (see [Peepo-side work](#11-peepo-side-work)). Ingen gives that port no special treatment: it is a normal control.
2. **`run()` is called in slices.** In the same function, Ingen splits each period at every control value change and calls `run()` once per slice. A 128-frame period can arrive as runs of 37 and 91 frames. Nothing may assume one `run()` per period or a fixed `n_samples`. Smoothers advance per sample, not per call.
3. **Blocks run on several threads.** `threads` defaults to the number of cores (`src/Configuration.cpp:90`), which is 4 on the Pi. Blocks with no path between them can run at the same moment. Discipline and Discipline Return must share data through lock-free single-writer structures. When send is patched to return, Ingen's ordering puts Discipline first in the period. When they aren't connected, there is no order.
4. **Block length.** Ingen sets `bufsz:maxBlockLength` and `bufsz:nominalBlockLength` to the period (`src/server/LV2Options.hpp`). Discipline reads `nominalBlockLength` (falling back to `maxBlockLength`, then 128) to work out the return delay. It needs no other host features. `urid:map` is required only to read options; `options:options` is optional.

## 2. Technology and build

- **Plain LV2 C API in C++17, no DPF.** Both plugins must live in **one shared object**, so that they can share a pair registry through static storage. DPF builds one binary per plugin, so sharing would need a third library. The raw API is about 100 lines of glue for two descriptors.
- **Dependencies:** `lv2-dev` headers only (ISC license), so the license choice stays open. No Boost, no libsndfile in the plugin. The test tools may use a small WAV writer of their own.
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
LICENSE                      (milestone 0)
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
| 33 | `clock` | Size clock | CV out | | | trigger at each echo boundary (milestone 3; zeros before) |
| 34 | `paired` | Paired | control out | 0/1 | | 1 when a Return with the same Pair is running |

Enums use `lv2:integer`, `lv2:enumeration` and `lv2:scalePoint`, counting from 0, so Peepo's overlay can name them as choices. Toggles use `lv2:toggled`. `size` uses `pprops:logarithmic`, and `pair` uses `lv2:integer`.

**CV scaling.** Each CV input is added to its knob in the control's *normalised* range (0 – 1 across the control's scale; logarithmic for Size), then clamped. A CV of +1 sweeps the whole range. Before milestone 1 closes, check what Peepo's LFO and random modules actually output (0 – 1 or −1 – 1). If most are unipolar, a `cv_depth`-style attenuverter isn't needed, because Peepo's CV utility modules can scale.

**CV read rate.** CV buffers are per sample. Level and Repeats use the CV per sample (they only scale gains). Scan, Spread and Size read their CV every 32 samples and feed it to their smoothers. That is cheap and fine enough for moves that glide or crossfade anyway.

### Discipline Return — `https://github.com/ABustos2606/discipline#return`

| # | Symbol | Name | Kind | Range | Default |
|---|---|---|---|---|---|
| 0 | `in_l` | In L | audio in | | |
| 1 | `in_r` | In R | audio in | | |
| 2 | `pair` | Pair | integer | 1 – 16 | 1 |
| 3 | `paired` | Paired | control out | 0/1 | |

The Return has no audio output. Check in milestone 1 that Peepo's canvas and catalogue export accept a module with no outputs. If they don't, add a silent `thru` pair of outputs (appending ports is allowed; renaming is not).

Both plugins declare `lv2:hardRTCapable` and `lv2:optionalFeature options:options`. Discipline requires `urid:map`. Return requires nothing. The class is `lv2:DelayPlugin`. The Return is `lv2:UtilityPlugin`, so the catalogue doesn't list it as a second delay.

## 4. The pair

### Registry

`pair_registry.cpp` holds a static array of 16 slots, one per Pair ID, inside `discipline.so`. Both plugins are loaded from that one object, so they see the same array. Each slot holds:

```cpp
struct PairSlot {
    std::atomic<void*>    owner_main;    // Discipline instance that claimed it, or null
    std::atomic<void*>    owner_return;  // Return instance that claimed it, or null
    std::atomic<uint64_t> written;       // frames the Return has written (release)
    std::atomic<uint64_t> main_frames;   // Discipline's frame counter (release)
    std::atomic<uint32_t> epoch;         // bumped when either side (re)claims
    float ring[2][kReturnRing];          // kReturnRing = 8192 frames, static storage
};
```

- **The ring is static memory, owned by neither instance.** Its size is 16 × 8192 × 2 × 4 B = 1 MB. When a Discipline is deleted while its Return is still running on another thread, nothing is left dangling. That rules out the use-after-free a pointer into Discipline's own memory would invite.
- **Claiming** is a compare-and-swap of `owner_main` (or `owner_return`) from null to `this`, done in `run()` when the Pair value changes. It is lock-free, so it is real-time safe. The old slot is released by compare-and-swap back to null. `deactivate()` and `cleanup()` also release.
- **A second instance with a taken ID** stays unpaired: no return audio, `paired` = 0. That covers a duplicated module in Peepo that copied the Pair value (section 11).
- **Patch load order doesn't matter.** Whichever of the two runs first claims its side. The other joins when it runs.

### Timing of the return

The Return writes each slice of its input into `ring` at index `written mod kReturnRing`, then publishes `written += n` with release ordering. Discipline reads the return at its own index `r` minus a fixed lag `B`, where `B` is the nominal block length.

- **Alignment.** When it claims, or when it sees `epoch` change, the Return sets `written = main_frames - B`, rounded to the block. In the patched case (Discipline runs first in each period), the sample sent in period *k* is then read back in period *k + 1*: a return delay of exactly `B` frames. Discipline subtracts `B` from the echo tap delay, so Size is exact including the trip through the return (section 5).
- **Unpatched or reversed order.** The return delay is some other constant; nothing breaks. Read and write regions never overlap, because the ring is 8192 frames and the lag is one block.
- **Stale return.** Before reading, Discipline checks that `written >= r - B + n` (acquire). If not, the Return is missing, bypassed by the engine or behind. It then reads silence for the missing frames, and after 0.5 s of that sets `paired` = 0. A short fade (64 samples) covers loss and recovery, so they don't click.
- **Slices.** Both sides count frames per slice, so Ingen's slicing (section 1) doesn't break alignment. Resync happens only on claim or epoch change, never while running.

The test in section 9 sends an impulse from send to return through a cable, under random slicing and both run orders. It checks the delay is `B` in the patched case and constant in every other case.

## 5. Signal flow

Per sample, stereo. `M` is the memory, `E` the echo line, `ret` the return read for this sample. Gains that change are all smoothed (section 6).

```
                ┌─────────────────────── MEMORY M (180 s, always recording) ─┐
in ─┬─────────▶ write: in + collect·M[w] (+ FEED: rep·ret)  (soft limit on recirculating part only)
    │           read:  p = M[w - scan]      s = M[w - scan - spread]
    │                  mem = p + spread_on·s     (equal-power mix when Spread is on)
    │                                │
    │                                ├──────────────▶ memory dry (Off / Scanning / On)
    │                                ▼
    │           ECHO E (60 s line): write  e_in·(1 - loop) + fb
    │                tap = E[size - B]   (two taps, crossfaded on Size change)
    │                fb  = IN:   rep·loopgain(ret)
    │                      OUT:  rep·loopgain(tap)
    │                      FEED: 0            (the loop goes through the memory instead)
    │                send = tap
    │                wet  = ret              (in every routing)
    ▼
out = dry_gain·in + level·wet·(1 - wipe) + mdry_gain·mem
```

The rule that makes the three routings simple: **the echo you hear is always the return**. The send always carries the echo tap. Only the place the return is fed back into changes:

- **IN.** The return is the echo's feedback, so patched modules act again on every repeat (the main Modify sound).
- **OUT.** The echo feeds back on itself, unprocessed. The modules process each repeat once on the way out, so every repeat sounds the same.
- **FEED.** The echo has no internal feedback. The return is written into the memory, scaled by Repeats, and comes back round through the read positions. Repeats then sets the feedback of the whole system, as in the manual. Two nested loops each at Repeats would exceed unity gain once Repeats > 0.5. The send carries the wet signal only (echo plus memory dry), never the dry input, which the memory already records.

The spec takes it as given that the user always patches send to return. Without that, no echo is heard in any routing. The README and the Peepo module description say so.

**Echo tap compensation.** The tap is read `size - B` frames back, so send → return → output lands at exactly Size. In IN, one trip round the loop also takes exactly Size. Size's minimum (50 ms = 2400 frames) is far above any `B` (≤ 1024).

**Memory reads at "the present".** With Scan and Spread at 0, `p = M[w]` is the sample just written, so the echo is fed the live input (plus Collect). The memory acts as a pre-delay of 0.

## 6. Components

### 6.1 Buffers and allocation

- **Memory:** 180 s × sample rate × 2 channels, plus 4 guard frames for interpolation. 69.1 MB at 48 kHz. Sized from the rate given to `instantiate`, so 44.1 and 96 kHz also work (96 kHz is 138 MB, noted for the Pi's 2 GB).
- **Echo line:** 60 s + 50 ms crossfade margin + one max block, stereo. 23.1 MB at 48 kHz.
- **Allocated in `instantiate`** with `std::nothrow`. A failed allocation returns NULL (Ingen reports a failed instantiation) rather than throwing.
- **Pages are touched in `instantiate`** with a single `memset`. On Linux a fresh allocation is mapped lazily, so the first write to each 4 KB page in `run()` would page-fault in the audio thread: about 23,000 faults spread over the first three minutes. `instantiate` runs outside the audio thread, and touching 92 MB takes tens of milliseconds there. Then call `mlock` on both buffers and ignore failure; JACK setups often allow it.
- **Positions are 64-bit frame counters.** They become indices only at access (`idx = pos % len`). The memory has its own clock (`mem_frames`), separate from the host's, because the memory can pause (6.10).

### 6.2 Clear without xruns: a logical clear

Clearing never touches the buffer. Discipline keeps `valid_from`, a memory frame counter. **Clear sets `valid_from = mem_frames`.** Every memory read (playback positions and Collect's read of `M[w]`) returns 0 for positions older than `valid_from`, with a 256-sample fade at that boundary so the edge doesn't click. Recording overwrites the old data over the next 3 minutes. The cost is O(1) whatever the buffer's size: no `std::fill`, no slices.

- The audible part of Clear is a 20 ms fade-out of every memory read before `valid_from` moves (**tune**), then reads start again from silence.
- Clear empties the memory only. Echoes already in the echo line ring out, as the manual's memory clear does. If listening shows it should also cut the echo, the echo line gets its own `valid_from` the same way.
- Bypass without COLLECT or ALWAYS uses the same clear (6.10).

The same trick makes startup clean. `valid_from` starts at 0, so nothing older than the instance is ever read.

### 6.3 Smoothing

- **Gains** (Level, Repeats, dry, memory dry, wipe, bypass mix): one-pole, about 10 ms (**tune**), advanced per sample. A slice boundary only changes the target.
- **Scan and Spread:** a **slew-limited glide** in samples of offset. The read position moves at most `±max_scrub` frames per frame (start: 1.0, which means playback between stopped and double speed while moving). Movement is audible as scrubbing. The spec wants that ("cloudy"), so it isn't crossfaded away. Large jumps (the auto-scan snap back to the present, 6.6) use a crossfade instead.
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
- **`loop_gain`** sits only in the feedback path: the echo feedback in IN and OUT, and the return-to-memory path in FEED. It is a soft limiter. Below a threshold (start −6 dBFS) its gain is exactly 1.0, so normal playing is uncoloured. Above it the gain is reduced, with a fast attack (1 ms) and a slow release (300 ms). At Repeats = 1 echoes build up and then stay at a steady level, "like a loop", instead of running away. In IN it also catches gain added by the patched modules.
- Nothing nonlinear touches the dry path or the first pass of the input into the memory. That fixes the skeleton's `tanh` on every write.
- **Collect** (memory overdub) uses a fixed recirculation gain (start 0.7, **tune**) through its own `loop_gain`, so three-minute-old audio comes back quieter and never builds up.

### 6.6 Scan

The scan offset `S` (frames behind the present) comes from one of three sources:

- **MANUAL:** `S = scan · (memory length - margin)`, snapped to a multiple of Size (6.8), then glided (6.3). Scan 0 is the present.
- **AUTO:** the `auto_scan` state machine:
    - **Present.** At each Size boundary (from the clock, 6.11) roll `rng` against `p_jump(scan)`. Start: 0 at scan 0, rising to 1 at scan 1 (**tune**).
    - **Jump.** Pick a target depth: a whole number of Sizes, from 1 to `depth_max(scan)` (start: scan² × memory length, **tune**). Glide there, so the scrub is heard.
    - **Away.** Stay for 1 – 4 echoes (random, **tune**), then go back to Present: crossfade (30 ms) from the scanned read position to the present position. That is the "snap back".
    - At scan = 1, `p_jump` is 1 and the hold is endless, so it never comes back to the present: "scans all the time".
- **Scan hold** (momentary scan) forces the AUTO machine to behave as at scan = 1 while held, whatever the mode. On release it snaps back.
- **Scan CV** adds to the knob before either mode reads it.

Crossfades between two memory read positions use a second read head, like 6.4. So the memory has up to four interpolated reads per sample: primary, spread, and the fade partner of each. That costs about 32 multiply-adds per sample.

### 6.7 Spread

The second read position is `S + P`, where `P = spread · (memory length - S - margin)`, snapped to a multiple of Size and glided. Spread 0 turns it off with a 10 ms fade, not a cut. With Spread on, the two positions are mixed at `0.707` each (**tune**). The spread position moves with Scan, as the manual says, because it is defined relative to `S`.

### 6.8 Clamping and snapping

- **Snap:** `S` and `P` are rounded to a whole number of Sizes, so taps stay in rhythm. At Size 60 s that leaves three positions; that is what the spec asks for.
- **Clamp:** `S + P ≤ memory length - (max block + 4 + crossfade)`. Without it the read position would wrap round and play the newest audio, with a click (one of the skeleton's faults). The clamp applies after snapping: a snapped value over the limit drops to the previous multiple.
- Reads older than `valid_from` are silent (6.2). So right after start or Clear, a deep scan plays silence until the memory has filled that far. That is correct.

### 6.9 Loop

- **Loop on:** the echo line's input is muted (`e_in` × 0, faded over 10 ms), and its feedback is forced to exactly 1.0 with `loop_gain` bypassed, so the content recirculates unchanged. The memory keeps recording. The user can play over it: the dry signal is unaffected.
- In **IN**, the loop runs through the return, so modules keep acting on it. That is the patched equivalent of the hardware, and it is allowed.
- Size changes crossfade as in 6.4. The loop keeps its contents but its length changes, so it is never erased.
- **Loop off:** feedback fades back to Repeats over 50 ms.
- **Latch:** with Latch off, Loop is active while the port is 1 (Peepo's momentary stomp). With Latch on, each rising edge flips an internal state.

### 6.10 Memory dry, Wipe, Dry kill and bypass

- **Memory dry:** `mdry_gain` is 0 (Off), 1 (On), or for Scanning, 1 while `S` or `P` is moving or away from the present (auto-scan Away, a glide in progress, Spread on), faded over 50 ms. When Scan and Spread are at 0 the memory read equals the dry input, so Scanning stays silent and doesn't double the dry signal.
- **Wipe:** the echo (`wet`) is muted and Memory dry is forced to On.
- **Dry kill:** Off passes dry. Always (TRUE) never passes dry, even when bypassed. Effect on (SERIES) mutes dry only while the effect is on.
- **Bypass** (the `enabled` port, 1 → 0; 10 ms crossfade on the output):

| | Output while bypassed | Echo | Memory |
|---|---|---|---|
| Neither ALWAYS nor COLLECT | dry (or silence with Dry kill Always) | muted, or rings out with TRAILS (input to echo cut) | **cleared** (6.2) |
| COLLECT | as above | as above | **paused**: `mem_frames` stops and reads stop moving. With TRAILS, keeps moving and overdubbing but takes no new input |
| ALWAYS | as above | as above | keeps recording. For safety Collect is treated as off and Repeats is limited to 0.7 while bypassed (**tune**) |

When it is switched back on, nothing jumps: positions are relative to `mem_frames`, which was either paused or kept running.

### 6.11 Tempo and the Size clock

- Size in beats = `size_beats × 60 / bpm` s, clamped to 50 ms – 60 s. A BPM change is a Size change, so it crossfades.
- **Size clock** (`clock`): a phase counter of length Size (the current tap A). At each wrap it writes 1.0 for 1 ms, then 0 (**tune** to Peepo's trigger convention). It is also the beat that auto-scan uses for its decisions. When Size crossfades, the counter takes the new length at its next wrap. The CV port exists from milestone 1 and writes 0 until milestone 3.

### 6.12 Output and safety

- A final finite check per slice: if any output sample is NaN or infinite (a module in the loop can produce that), Discipline resets the echo line's feedback state, writes silence for that slice and raises an internal flag. It doesn't throw or log in `run()`. Ingen must never receive a non-finite value (Peepo's catalogue withholds plugins that output one).
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
    6. send = tap; wet = ret; out = dry + level·wet + memory dry, with bypass and Dry kill gains;
    7. clock output.
4. Finite check (6.12). Advance counters.

**Budget.** About 60 – 100 multiply-adds per stereo sample, mostly the four interpolated memory reads. Memory reads are sequential, so cache misses are rare. Expected well under 1% of one A76 core at 48 kHz. That leaves the worst-case time per `run()` as the real measure (section 9).

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

The tests build with `-fsanitize=address,undefined` on the desktop (`make test SAN=1`).

### 9.2 Desktop hosts

- `lv2lint` on both plugins: no errors. Document any warning that stays.
- `jalv` and Carla. Carla can patch Discipline → effect → Return as two plugins in its patchbay, which gives a quick listening setup without Ingen.
- `tools/render` renders a guitar DI file through fixed settings to a WAV, for listening and before/after comparisons while tuning.

### 9.3 Ingen under WSL

With Peepo's `tools/ingen-spike/env.sh`:

1. Load both plugins and patch send → a filter → Return. Check `COMPILATION_FAILED` doesn't occur. Then patch Return's output side back toward Discipline's input to confirm that this is still rejected (expected) while the pair works.
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

- Choose the license (spec checklist). LV2 headers are ISC and there are no other dependencies, so any license works. Add `LICENSE` and SPDX headers.
- Makefile, both descriptors, both `.ttl` files with **the full port list of section 3**, `ports.hpp`, and the `ttl_check` test.
- Discipline passes input to output and outputs silence on Send. Return claims its slot.
- **Done when:** `make`, `make install DESTDIR=…` and `lv2lint` are clean, and both plugins load in jalv and in Ingen under WSL. Tag `v0.0.1`, so Peepo can wire its build step early.

### Milestone 1 — the core (spec milestone 1)

1. `stereo_ring`, `interp`, `smoother`, `xfade_tap`, `loop_gain`, with unit tests.
2. Memory and echo allocation, page touching, `mem_frames`, `valid_from` and logical Clear.
3. Pair registry, the return ring and latency alignment (`pair`, `return_latency`).
4. Echo line with Size by crossfade, ms and beats, BPM, tap compensation (`echo_time`, `size_no_pitch`).
5. Repeats with `loop_gain`. IN / OUT / FEED as in section 5 (`routing`, `repeats_max`).
6. Level, CV inputs for Level, Repeats and Size. Scan and Spread CV arrive with milestone 2's Scan and Spread, but are already declared.
7. `nonfinite`, `rt_timing`.
- **Done when:** all tests above pass. A patch in Ingen under WSL plays echoes through a module in all three routings. Then the Pi check (9.4) shows no added xruns. Tag `v0.1.0`. Peepo adds its overlay entry and pinned build.

### Milestone 2 — memory features (spec milestone 2)

1. Scan MANUAL with snap, clamp and glide. Then Scan AUTO (`auto_scan`) and Scan hold.
2. Spread.
3. Collect with its own `loop_gain`.
4. Loop and Latch.
5. Memory dry (three modes), Wipe, Dry kill.
6. Bypass through `enabled`: ALWAYS, COLLECT, TRAILS (needs Peepo's port bypass, section 11).
- **Done when:** `scan_clamp`, `scan_snap`, `auto_scan`, `loop`, `bypass` and `cv` pass. There has been a listening session on the Pi with a guitar, and a Pi xrun check. Tag `v0.2.0`. Expect several `v0.2.x` tags of tuning after this.

### Milestone 3 — clock and modifiers (spec milestone 3)

1. Size clock output (6.11), checked with a Peepo trigger module, for example a clock divider.
2. Decide which built-in modifiers to add, from the spec's list: **Speed** (stepped and smooth, including reverse) and **Trimmer** need the echo's timing; **Dropper** in pattern mode needs per-echo position.
    - Speed and reverse read the echo line at a variable rate from the tap: a second interpolated reader that restarts at each Size boundary, with a short crossfade at each restart.
    - Each chosen modifier gets a selector and an amount, **appended** as new ports (`modifier`, `modify`), with `lv2:minorVersion` raised.
- **Done when:** the clock triggers a module in step with the echo, and each chosen modifier has a test and a listening note. Tag `v0.3.0`.

## 11. Peepo-side work

These are in Peepo's repository, not here. They are listed so the plugin doesn't wait on them unexpectedly.

1. **Bypass through the port.** Peepo's catalogue export treats a control named `enabled`, or one with `lv2:designation lv2:enabled`, as the module's on/off switch: it keeps it at 1 and uses the engine's block bypass (`tools/export_lv2_catalogue.py:67`, Peepo `docs/16-catalogue-export.md`). For Discipline it must do the opposite: software bypass sets the `enabled` port, and the block stays enabled. That means an overlay flag, for example `"bypass": "port"`, honoured by the adapter's bypass command, by stomp bypass assignments and by saved patches. It is needed before milestone 2's bypass work can be checked in Peepo.
2. **Overlay entry** (`config/catalogue/overlay.json`). Name "Discipline", category Delay. Description, for example: "Delay with a 3-minute memory. Cable Send to a Discipline Return, through any modules." Then:
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
| Default routing | The spec doesn't give Habit's default; the manual (p. 11) should | Milestone 1, before the first saved patch (defaults can change later; symbols can't) |
| Dry kill's three states | The spec lists TRUE and SERIES; whether "off" is a third position on the hardware or a separate switch needs checking against pp. 40 – 43 | Milestone 2 |
| Peepo CV range | Sets how CV adds to knobs (section 3) | Milestone 1 |
| Return with no outputs in Peepo | Might need a silent pass-through port | Milestone 0 |
| Peepo bypass via the port | ALWAYS / COLLECT / TRAILS can't work through engine bypass | Before milestone 2 is checked on Peepo |
| Memory at high sample rates | Two instances at 96 kHz are 460 MB | Note it in the README; Peepo runs at 48 kHz |
| Sound tuning | Curves, fades, limiter, auto-scan feel: months, per the spec | `v0.2.x` and later, via `tools/render` and listening on the Pi |
| Modifier ports | Appending ports later is safe for Ingen (it saves by symbol) but bumps the minor version | Milestone 3 |
