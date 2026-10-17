# Discipline design

Discipline is a delay with a long memory, inspired by the Chase Bliss Habit. It is an LV2 plugin written for Peepo, a pedal built on a Raspberry Pi 5 running the Ingen modular host. It must also work as a normal LV2 plugin in Carla or Ardour.

This spec moved here from Peepo's `docs/26-pedal-inspired-plugins.md` on 16 October 2026. That document keeps the reasons for choosing a Habit-style plugin and the Peepo-side steps.

## Where it lives

Discipline has its own repository, outside Peepo:

- Every other plugin Peepo runs is built from another repository at a pinned commit. Discipline is built the same way.
- It is a normal LV2 bundle, so it can be tested on a desktop without Ingen or the Pi.
- It has its own build (Makefile, and DPF if used), apart from Peepo's CMake and LVGL build.
- Its license is MIT (chosen 16 October 2026), separate from Peepo's GPLv3. Peepo can still build and ship it, since MIT is compatible with the GPL.
- Sound tuning will take months. Tagged releases let Peepo update to a new version on purpose.

What stays in Peepo:

- its entry in `config/catalogue/overlay.json`;
- the build step that fetches and builds it at a pinned commit;
- patches that use it, and xrun measurements;
- test notes.

The plugin URI is `https://github.com/ABustos2606/discipline#`, not Peepo's namespace. Fix it before the first saved patch, because changing it later breaks patches. Don't use "Habit" or "Chase Bliss" in names or descriptions.

## Send and return: two plugins, not one

Ingen refuses connections that create a loop. When a connection is added, Ingen works out a new order to run the plugins in. If the connection closes a loop, that step fails and the connection is rejected with `COMPILATION_FAILED` and a "Feedback compiling ..." log line (Peepo's `reference/upstream/ingen/src/server/events/Connect.cpp:137`, `CompiledGraph.cpp`). So a cable from a plugin's send, through other modules, back to the same plugin's return can never be patched.

Discipline is two plugins in the same bundle, sharing one buffer:

```
Discipline ── send ──▶ [any modules] ──▶ Discipline Return
```

- **Discipline** records the input into the memory, reads the playback positions, runs the echo, and outputs the echo on its send.
- **Discipline Return** writes the processed audio back into Discipline. Discipline reads it on the next audio block. That adds one block of delay (about 2.7 ms at 128 samples), which doesn't matter because the shortest echo is 50 ms.
- The return goes to a different place for each routing (manual p. 50), set by the routing control on Discipline:
    - **IN:** into the echo's Repeats feedback loop, so the modules are applied again on every repeat. This is the main Modify sound.
    - **FEED:** into the memory, so everything heard is recorded.
    - **OUT:** into Discipline's output only, in place of the plain echo. Every echo sounds the same. Patching modules after Discipline's output would also process the dry signal.
- The two instances find each other through a shared "pair" ID control. Beebo's `loop_common_in/out` modules appear to be the same kind of pair.
- The user always wires send to return, either directly or through effects. A plugin can't tell whether a port is patched (Ingen connects unpatched ports to silence), so the plugin shouldn't try to guess.

## Modulation through CV

Habit's built-in ramp engine exists because the hardware has no other way to modulate its knobs. Peepo already has CV modules, so Discipline exposes a CV input for each main control (Level, Repeats, Size, Spread, Scan, Modify) instead of copying that engine. Each control's value is its knob setting plus its CV input, limited to the control's range. Each control can then follow a different LFO, envelope or random source, which the hardware cannot do.

Auto-scan is not part of the ramp engine and must be built in. It is Habit's default Scan mode. It wanders into the past now and then and snaps back to the present, which a CV random source can't do on its own (see [What the Habit manual says](#what-the-habit-manual-says)).

Some of the six built-in modifiers can wait, and some can't:

- **Filter (B2) and Stability (A2)** are covered by existing modules patched between Discipline and Discipline Return.
- **Trimmer (A3)** cuts each echo in time with Size. **Dropper (B3)** in pattern mode drops audio at the same place in every echo. **Stepped and Smooth Speed (A1, B1)** play each echo faster, slower, stopped or reversed. All three need the echo's timing.
- A **Size clock output** (a CV trigger at each echo boundary) lets external modules sync to the echo. Speed and reverse probably need to be built in, because they read the echo buffer differently.

## Tempo

Peepo gives every patch a tempo (30 to 300 BPM) and passes it to modules the way Beebo does: on a BPM control port of the module's own. The overlay names that port (`"tempo": "BPM_1"` style), and a time control with the unit `beats` is shown as a note value (1/8, dotted, triplet) with its milliseconds. See Peepo's `docs/29-parameters-files-tempo.md`.

So Size can follow the patch tempo if Discipline has a BPM control and a Size control in beats. Tap tempo is a Peepo footswitch setting that changes the patch tempo; Discipline doesn't need its own tap input. A milliseconds Size is still needed for the long settings (up to 60 s is 120 beats at 120 BPM).

## Pitfalls from Gemini's skeleton (15 October 2026)

Gemini produced a feature list for Habit and a C++ skeleton. Its milestone order is good: memory buffer and delay first, then Scan, then the OUT / IN / FEED routing. Don't build on the code:

- **Wipe is the wrong name for clearing.** In Habit, WIPE mutes the echo so only the memory is heard. Clearing the memory is a separate action. Discipline has a Clear control and a separate Wipe mode.
- Its Wipe (really Clear) empties the whole 34 MB buffer with `std::fill` in a single `run()` call, a guaranteed xrun. Clear a slice per block, or fade the buffer out.
- The "fall back to the internal path when the return is silent" test runs per sample. It flips at every zero crossing and in quiet passages, which distorts the sound. The two-plugin pair replaces it.
- IN and FEED run identical code. All three modes write feedback into the long buffer, but in OUT and IN the memory should hold only the input (plus its own overdub when Collect is on). That needs a separate echo loop, with FEED alone writing the output into the memory.
- Size glides through a smoothing filter, which bends the pitch. A pitch-free change needs a crossfade between two playback positions.
- Size plus Scan can reach about 239 s in a 180 s buffer. With a separate echo line, Size no longer adds to the memory position, but Scan plus Spread still can go past 180 s. The read position wraps around and plays recent audio, with a click. Clamp it.
- `tanh` on every write colours even the dry signal. Level is not smoothed, so it can zipper. The URI `http://peepo.dsp/...` is not a domain anyone owns.

## What the Habit manual says

Checked on 16 October 2026 against the Chase Bliss Habit manual (Peepo's `reference/Habit_Manual_Pedal_Chase+Bliss.pdf`, CBA ref 2022 - HAB1). Page numbers are the manual's own.

**Signal flow (p. 50).** Input, Collect and FEED are mixed into the **memory** (3 minutes, always recording). Two playback positions (primary and Spread) are mixed and sent to the **echo**, a separate delay line with its own Repeats feedback loop. The modifiers sit inside that loop (IN) or after it (OUT). FEED sends the final output back into the memory. Memory Dry sends the playback positions straight to the output, skipping the echo.

**Knobs (pp. 6-9).**

- **Level:** echo volume. Turns into ramp speed when ramping is on.
- **Repeats:** echo feedback. At maximum the echoes build up forever but stay stable, like a loop. In FEED it sets feedback for the whole system. The loop needs gain control or a limiter, kept out of the dry path.
- **Size:** echo time, 50 ms to 60 s, with no pitch change while it moves. Can be set by tap tempo or MIDI clock.
- **Modify:** amount for the selected modifier. Noon is neutral, each side does something different.
- **Spread:** distance of the second playback position behind the first. Off at minimum, can reach all the way back into the memory. It moves with Scan.
- **Scan:** two modes, set by the MANUAL switch:
    - **AUTO (default):** Habit wanders into the memory now and then and snaps back to the present. The knob sets how often and how far back. At maximum it scans all the time.
    - **MANUAL:** a fixed position in the memory, like a pre-delay. Minimum plays the present.
- Scan and Spread snap to multiples of Size, so taps stay in rhythm. While Scan moves you hear it scrub through the memory ("cloudy"). That is wanted, not something to crossfade away.

**Routing (p. 11).** IN: the modifier is applied again on every repeat. OUT: every repeat sounds the same. FEED: the output is recorded into the memory.

**Modifiers (pp. 16-17).** A1 Stepped Speed (octaves and fifths, reverse to forward, up to 4x, stopped at noon), A2 Stability (wow, flutter, filtering, optional tape noise), A3 Trimmer (cuts the start or end of each echo, synced to Size), B1 Smooth Speed (up to 2x either way), B2 Filter (low-pass or high-pass), B3 Dropper (random dropouts or a repeating pattern, granular at maximum).

**Footswitch actions (pp. 12-13).** These become control ports that Peepo footswitches can drive:

- **Loop:** freezes the echo. You can play over it, and Size can change without erasing it. The memory keeps recording. Momentary, or latching with the LATCH switch.
- **Tap:** sets Size.
- **Momentary scan:** like Scan at maximum while held.
- **Clear:** empties the memory.

**Switches (pp. 40-43).** All are saved with presets, so all should be control ports, not hidden state:

- **MANUAL:** Scan mode.
- **COLLECT:** the memory overdubs itself. What was played 3 minutes ago comes round again mixed with what is played now.
- **DRY KILL:** TRUE (no dry signal at all) or SERIES (dry muted only while the effect is on).
- **ALWAYS:** the memory keeps recording while bypassed. For safety, bypass then turns Collect off and limits Repeats.
- **LATCH:** Loop latches.
- **WIPE:** mutes the echo, so only the memory is heard. Forces Memory Dry on.
- **MEMORY DRY:** OFF, SCANNING (default: heard only while Scan or Spread move) or ON.
- **TRAILS:** echoes fade out after bypass. With Collect on, the memory keeps moving while bypassed instead of pausing.

**Bypass (pp. 20, 33).** Bypassing empties the memory unless COLLECT or ALWAYS is on. With COLLECT, the memory pauses and keeps its audio. Discipline needs an `lv2:enabled` port to know when it is bypassed.

**Memory.** A 3-minute stereo memory at 48 kHz in 32-bit float is about 69 MB. The 60 s echo line needs its own buffer, about 23 MB more. Speed and reverse play a captured window during the following one, so a window can be at most 30 s: above a 30 s Size they play two chunks per echo. Allocate both when the plugin is instantiated, never in `run()`. The buffers are not saved with patches (no LV2 state).

## Milestones

**Behavior decisions after the Milestone 2 audit:** large Scan/Spread moves
use a short audible scrub plus a crossfade, reaching their selected history
within 150 ms. They do not scrub across the entire intervening history.
Feed recirculates only Repeats times Return, independent of Level; Memory dry
is playback only and Collect provides memory overdubbing. These choices
supersede the original implementation's full-distance glides and block-period
memory-dry feedback. Scanning memory dry omits the present-time tap; explicit
On monitors the full mix. Scan hold returns to the Manual target when released
in Manual mode. Without Trails, bypass discards echoes after its fade; with
Trails, Loop releases back to ordinary Repeats. The manual summary above is
reference material, not an instruction to restore the rejected feedback path.

**Implementation update, 16 October 2026:** Milestones 1 and 2's delay core and
memory controls are implemented, with desktop, emulated arm64, jalv and Ingen
checks. [Validation and remaining release gates](milestone-2-validation.md)
record the scope. The checkbox below stays open until physical Pi validation
and the release follow-up are complete.

**Milestone 3 implementation update, 17 October 2026:** the user selected the
Size clock and all four proposed modifiers: Stepped Speed, Smooth Speed with
reverse, Trimmer and patterned Dropper. They process Send, preserving the
existing IN/OUT/FEED return routing. Speed captures complete Size windows and
restarts with short crossfades; positive unity bypasses that reader. Noon
stops both speed modes without held DC. Trimmer noon and Dropper zero are
transparent. Modifier, Modify and Modify CV append ports 35 to 37; existing
ports and URIs stay fixed. After review, the echo line stays at 60 s and Speed
windows are capped at 30 s (two chunks per echo above a 30 s Size). Details, tests, listening material and remaining release
gates are in [Milestone 3 validation](milestone-3-validation.md).

- [x] Choose the license: MIT.
- [ ] Milestone 1: the Discipline and Discipline Return pair, the shared memory, a separate echo loop, Size changes by crossfade, IN / OUT / FEED routing of the return, a Clear that can't cause xruns, and the `.ttl` files with CV inputs.
- [ ] Milestone 2: Collect, Scan AUTO and MANUAL with Size-locked positions, Spread, Loop, Memory Dry and Wipe, and bypass behaviour (`lv2:enabled`, ALWAYS, TRAILS).
- [ ] Milestone 3 release: Size clock, Stepped and Smooth Speed/reverse, Trimmer and patterned Dropper implemented; physical Pi/listening validation pending.
- [ ] Lay out the control ports to fit Peepo's catalogue overlay format.
