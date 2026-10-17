# Adding Discipline to Peepo

Written 17 October 2026 for whoever adds Discipline to Peepo (`C:\Pedal PI`).
Discipline is ready for its first Peepo build. It has passed every desktop,
sanitizer, Ingen (WSL) and emulated-arm64 check. It has not run on the Pi:
that test, and listening, are what this integration is for.

Read first: Discipline's [design](design.md) (the spec) and
[implementation plan](implementation-plan.md), section 11 "Peepo-side work".

## What it is

One bundle, `discipline.lv2`, one binary, two plugins:

| Plugin | URI | Class | Audio |
| --- | --- | --- | --- |
| Discipline | `https://github.com/ABustos2606/discipline#discipline` | Delay | 2 in, 2 out, 2 Send |
| Discipline Return | `https://github.com/ABustos2606/discipline#return` | Utility | 2 in, no outputs |

The user patches Discipline's Send through any modules into a Discipline
Return with the same Pair. Without that cable no echo is heard: that is the
design (Ingen rejects a loop back into the same plugin). Several pairs can run
at once with different Pair values (1-16); a second Discipline with a Pair
already in use stays unpaired.

## 1. Build step

Add it to `tools/ingen-spike/14-more-plugins.sh` with the existing `third`
helper. Pin the commit the user gives you (they commit and push; there is no
release tag yet, `v0.3.0` comes after the Pi test):

```sh
third discipline https://github.com/ABustos2606/discipline.git <commit> "make" \
    make -j"$JOBS" OPT="-O2 $( [ "$(uname -m)" = aarch64 ] && echo -mcpu=cortex-a76 )" LV2DIR="$LV2DIR" install
```

- Needs only `build-essential`, `pkg-config` and `lv2-dev` (already in
  `BUILD_DEPS`). No network, no configure step, GCC 12 on Debian 12 is fine.
- Prefer the plain `OPT` above. Passing the script's own `$OPT` also works:
  the Makefile adds `-fno-fast-math` after `OPT`, because the NaN guards and
  exact-neutral paths need IEEE arithmetic. The emulated arm64 suite passed
  with the script's full aarch64 `OPT`.
- `make install` writes `$LV2DIR/discipline.lv2/` (`discipline.so`, three
  `.ttl` files, `LICENSE`). `make test` runs the offline suite if wanted
  (needs `python3-rdflib` for the Turtle check).
- License MIT; it can ship in Peepo's GPLv3 package.

## 2. Overlay entry

A starting point for `config/catalogue/overlay.json`. Check every key against
the overlay reader; the reader refuses the whole file on an invalid entry.

```json
{"uri": "https://github.com/ABustos2606/discipline#discipline", "name": "Discipline", "category": "Delay",
 "description": "Delay with a 3-minute memory you can scan back into. Cable Send through any modules to a Discipline Return with the same Pair.",
 "bypass": "port", "tempo": "bpm",
 "parameters": [
  {"symbol": "size_beats", "unit": "beats"},
  {"symbol": "enabled", "hidden": true},
  {"symbol": "paired", "hidden": true}
 ],
 "groups": [
  {"name": "Echo", "parameters": ["level", "repeats", "size", "routing"]},
  {"name": "Memory", "parameters": ["scan", "spread", "scan_mode", "collect"]},
  {"name": "Modify", "parameters": ["modifier", "modify", "wipe", "memory_dry"]},
  {"name": "Play", "parameters": ["loop", "latch", "scan_hold", "clear"]},
  {"name": "Bypass", "parameters": ["dry_kill", "always", "trails", "pair"]},
  {"name": "Tempo", "parameters": ["sync", "size_beats", "bpm"]}
 ]},
{"uri": "https://github.com/ABustos2606/discipline#return", "name": "Discipline Return", "category": "Utility",
 "description": "Brings the sound from Discipline's Send back in. Set the same Pair."}
```

Notes for the entry:

- **`"bypass": "port"` is required.** Engine bypass never calls `run()`, so
  Always, Collect and Trails could not work. The port is `enabled`
  (`lv2:designation lv2:enabled`), default 1.
- **Tempo.** `bpm` takes the patch tempo. It only changes Size when `sync` is
  on; then Size is `size_beats` beats. With Sync off, `size` (ms, logarithmic,
  50-60000, default 500) is used.
- **Choices** come from the `.ttl` scale points: `scan_mode` Auto/Manual,
  `routing` In/Feed/Out (default In), `memory_dry` Off/Scanning/On (default
  Scanning), `dry_kill` Off/Always/Effect on, `modifier` Off/Stepped
  speed/Smooth speed/Trimmer/Dropper. `pair` is an integer 1-16.
- **Output level.** Discipline has no output control of its own, so it gets
  Peepo's general Output. The Return has no audio output; it needs none.
- **CV and clock.** CV inputs `level_cv`, `repeats_cv`, `size_cv`,
  `spread_cv`, `scan_cv`, `modify_cv` (each adds to its knob, -1..1 sweeps the
  whole range) and the CV output `clock` (1.0 for 1 ms at each echo boundary;
  Peepo's PolyLV2 clock divider was checked against it in Ingen) are ports for
  cables, not parameters.
- **Footswitch-friendly controls:** `loop` (momentary hold; with `latch` on,
  each press toggles), `scan_hold` (momentary), `clear` (trigger on a rising
  edge), and bypass through `enabled`.
- Names are plain ASCII, at most 16 characters, and never name the hardware
  the design was inspired by. Please keep that in any renamed labels.

## 3. Checks on the Peepo side

1. **A module with no outputs.** The Return is a sink. Ingen accepts it
   (Discipline's host test patches it). Check that Peepo's canvas, export and
   save/load handle it.
2. **Pair on add and duplicate (optional).** Default Pair 1 works for one pair.
   For more, the interface could give a new Discipline the lowest unused Pair
   and a Return added next the same value. A duplicated Discipline otherwise
   stays unpaired, by design.
3. **Memory.** About 92 MB per Discipline at 48 kHz (69 MB memory, 23 MB echo),
   allocated and page-touched when the module is added, never in `run()`.
   Adding the module therefore takes a moment. It tries `mlock` and ignores
   failure. The memory is not saved with patches; that is accepted.
4. **Block length.** Discipline reads the period from Ingen's options. It
   refuses periods of 50 ms or longer, or over 4096 frames.

## 4. First test on the Pi

The milestone needs this before it is called done:

- Patch: input -> Discipline -> output, Send -> (TAP Pitch or a filter) ->
  Discipline Return, plus a Clouds and an amp. Pair 1 on both.
- 30 minutes with guitar or a looped DI, moving Size, Scan, Spread, Clear,
  Loop, bypass, and every Modifier, including fast Stepped speed (4x) at high
  Repeats (the most expensive case).
- Record JACK xruns (0 beyond the same patch without Discipline), DSP load and
  RSS, into Peepo's `docs/evidence/`.
- Listening notes go back to the user for Discipline: Trimmer's 1 ms slice
  crossfades, Dropper's drop shapes and Speed's restart seams are first
  values to tune.

## Known limits (by design)

- Stability and Filter (Habit's A2/B2) are not built in: patch Peepo modules
  between Send and Return.
- Above a 30 s Size, Speed and Trimmer work on each half of the echo, so the
  60 s echo line stays at 23 MB.
- A plugin can't tell whether Send is cabled; with no Return cable there is no
  echo.
