# Discipline

Discipline is an LV2 delay with a 3-minute memory, inspired by the Chase Bliss Habit. It is the first plugin written for **Peepo**, and Peepo is the host that matters. Read `docs/design.md` before changing behaviour or ports: it is the spec.

## Peepo

Peepo is a guitar pedal built on a Raspberry Pi 5 (2 GB, Debian 12 arm64) running the **Ingen** modular host on JACK, with an LVGL touch UI. Users patch LV2 modules together, including CV modules (LFOs, envelopes, random sources). It was called "Pedal PI" before; old names (`pedalpi`, `urn:pedalpi:ns#`) still turn up.

- Repository: `C:\Pedal PI` (https://github.com/ABustos2606/Peepo). It is readable from here (see `.claude/settings.json`).
- Docs worth reading there:
    - `docs/26-pedal-inspired-plugins.md`: why Discipline exists and the Peepo-side steps;
    - `docs/22-plugin-catalogue.md` and `config/catalogue/overlay.json`: how plugins are named, grouped and described in the UI;
    - `docs/29-parameters-files-tempo.md`: patch tempo, BPM ports and times in beats;
    - `docs/20-clouds.md`: the closest existing plugin (no LV2 state, frozen buffer lost on reload);
    - `reference/upstream/ingen/`: Ingen's source;
    - `reference/Habit_Manual_Pedal_Chase+Bliss.pdf`: the manual the spec was checked against.
- Peepo builds every third-party plugin from its own repository at a pinned commit (`tools/ingen-spike/14-more-plugins.sh`, `reference/manifest.json`). Discipline will be built the same way, so tag releases and keep the build plain (`make`, no network access, works natively on arm64).

## Host limits that shape the code

- **Ingen rejects feedback loops** (`COMPILATION_FAILED`). That is why Discipline is a pair: Discipline and Discipline Return, in one bundle, sharing a buffer through a "pair" ID control.
- **xruns are the limit, not average CPU.** Ingen runs the whole patch as one JACK client. In `run()`: no allocation, no locks, no file or console I/O, no bulk work such as clearing a whole buffer at once. Allocate the memory (about 69 MB) and the echo line (about 23 MB) in `instantiate`.
- **Controls arrive once per audio block.** Smooth every control that changes sound (Level, Repeats, Scan, Spread), or it zippers. Size changes by crossfading two read positions, not by gliding (gliding bends pitch).
- **Unpatched inputs read silence.** A plugin can't tell whether a port is connected; don't guess.
- **No LV2 state for buffers.** The memory is lost when a patch reloads. That is accepted.
- **Bypass** comes through an `lv2:enabled` port; Discipline needs it for COLLECT, ALWAYS and TRAILS.
- Everything a user sets (switches, modes) is a control port, so patches save it.

## Rules

- Plugin URI: `https://github.com/ABustos2606/discipline#...`. Never use Peepo's namespace. Don't change a URI or port symbol once a patch has been saved with it.
- Never use "Habit" or "Chase Bliss" in plugin names, port names, descriptions or code identifiers. Describe it as a delay with a memory.
- Port names and descriptions appear on Peepo's small screen: short, plain ASCII (its font draws other characters as boxes), and they say what the control does, never who made it.
- Line endings are LF. Files go to Linux and the Pi.
- Use Peepo's name, never "Pedal PI", in new text.

## Testing

- Desktop: any LV2 host (Carla, Ardour, `jalv`), and `lv2lint` for the `.ttl` files.
- Peepo's development engine runs under WSL (Debian) with Ingen in `/opt/peepo`; Peepo's `tools/ingen-spike/env.sh` starts it.
- On the Pi: measure xruns with Discipline in a typical patch before calling a milestone done.
