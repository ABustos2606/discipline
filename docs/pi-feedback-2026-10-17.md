# First Pi test: feedback and changes to make

Written 17 October 2026, after the first test of Discipline (`812eb9c`) in
Peepo on the Pi.

## What happened on the Pi

The user found it unusable: "a few notes here and there", no dense delay, and
modifiers that "add no sauce". The running patch showed why:

| Setting | Value | Effect |
| --- | --- | --- |
| Pair | Discipline 3, Return 1 | Unpaired: no Return, so no echo at all. Modifiers act only on Send, so they were never heard either. |
| Wipe | On | Echo muted, only the memory heard. |
| Routing | Feed | No ordinary repeats. |

Only memory fragments reached the output. The patch was
input -> Discipline -> output, Send -> MDA RezFilter -> Return.

The real fault is the design: a plain delay needs a cable, a second module and
matching numbers, and is easy to silence without knowing why. The user also
judged the modifiers as far from the originals. That has not been re-checked
with a working echo, because the echo never sounded.

## Changes requested

1. **The echo works alone, with no cables.** The repeat loop runs inside
   Discipline, as on the original. A fresh Discipline with default controls is
   an audible delay.
2. **Modifiers sit inside the echo loop**, as on the original (IN: applied on
   every repeat; OUT: after the loop), not only on Send.
3. **"Outside" is a choice in the Modifier picker.** Only then do the repeats
   leave through Send and come back through the Return, with the user's own
   Peepo modules in between (this replaces the missing Stability and Filter).
   With any other choice, Send/Return are not needed.
4. **No Pair control.** The Return finds the Discipline of its own patch.
   Drop several pairs per patch if needed to make this simple.
5. **Then re-tune the modifiers by ear against the original**, now that they can
   be heard. Points noted from the code:
   - Speed replays Size-long captured chunks and restarts them each repeat,
     with audible seams. The original sounds like continuous, tape-like pitch.
   - Near noon Speed lands on very slow, chopped pitches.
   - Trimmer repeats a 1/8 to 3/4 slice to fill the echo, which stutters more
     than shortening the echo.
   - Dropper turns into 5 ms grains over the last tenth of the knob.
6. **Harder to silence by accident.** Wipe should not be one tap away on a
   normal page, or should show clearly that it mutes the echo.

## Peepo side, after the plugin changes

Peepo is `C:\Pedal PI`. Its uncommitted Discipline work (build step, inventory,
overlay, `"bypass": "port"`) is there.

- **Overlay** (`config/catalogue/overlay.json`, the Discipline entry): the
  reader refuses the WHOLE file if a group names a control that no longer
  exists. Remove `pair` from the "Stomps and pair" group if Pair goes, and add
  any new controls. Page layout now: Knobs (level, repeats, size, routing),
  Knobs 2 (modify, spread, scan, modifier), Switches, Switches 2, Stomps and
  pair, Tempo and output.
- **Build pin:** update the commit in `tools/ingen-spike/14-more-plugins.sh`.
- **Quick Pi build** (about 15 min, no engine rebuild): build only Discipline in
  the arm64 root, then `tools/pi/build-arm64.sh --skip-engine`; a ready script
  is `C:\Pedal PI\build\scratch\discipline-arm64.sh` (change its commit). Copy
  the package to the Pi, then `sudo ./peepo/install.sh --start`.
- `sudo runuser -u peepo -- /opt/peepo/libexec/peepo/peepo_effect_live show`
  on the Pi prints the running patch with every control value, which is how
  the problem above was found.
- Patches saved with the old ports (pair, etc.) must still load.
