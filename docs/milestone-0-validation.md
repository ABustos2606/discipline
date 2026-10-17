# Milestone 0 validation

Recorded 16 October 2026. The scaffold is implemented. These checks cover its
ABI, build, lifecycle and dry passthrough; they do not establish delay behaviour
or performance on a physical Pi.

| Check | Result |
| --- | --- |
| x86-64 `make -j4 test`, Debian 13 under WSL | PASS; GCC 14.2, LV2 1.18.10 |
| `make -j4 test SAN=1` | PASS; AddressSanitizer and UndefinedBehaviorSanitizer |
| arm64 `make -j4 test BUILD_DIR=build/arm64 OPT="-O2 -mcpu=cortex-a76"` | PASS; Debian 12 root, GCC 12, QEMU user emulation |
| `make install DESTDIR="$PWD/build/stage" PREFIX=/opt/peepo` | PASS; binary, all three Turtle files and MIT license staged under `opt/peepo/lib/lv2/discipline.lv2/` |
| `lv2info` on the staged bundle | PASS; both identities discovered, one shared binary, 35 and 4 ports |
| `nm -D --defined-only discipline.so` | Only `lv2_descriptor` exported |
| `lv2lint` on both installed identities | No errors; warnings documented below |
| jalv 1.6.8 | Both plugins load and run; main passes 0.125 left and -0.25 right unchanged, both Sends silent |
| Peepo's patched Ingen, 1 and 4 processing threads | Both plugins load; Send-to-Return stereo cables compile; main passes both channels unchanged |
| Peepo catalogue exporter and canvas | Input-only Return is accepted by the metadata exporter; source review confirms the canvas renders each declared input without requiring an output |

The arm64 tests ran the actual AArch64 shared object and test executable under
emulation. They were not native Pi runs. The temporary workspace bind mount in
the existing arm64 root was removed after the check.

Offline coverage includes all port indices, symbols, kinds, ASCII names, ranges,
enum properties, licenses and features; descriptor order and termination;
required-feature rejection; 44.1/48/96 kHz instantiation; exact passthrough under
random slices and in-place buffers; silent Sends and clock; zero-frame calls;
bounded output writes; both pair run orders; duplicate claims; Pair changes;
deactivation, reactivation and cleanup on either side; invalid Pair controls;
and Return running on another thread while main instances are deleted.

The host check is reproducible with `make test-hosts`, with Ingen and its shared
libraries on the environment's search paths. It uses its own named dummy JACK
server at 48 kHz and its own engine socket. Both the default 1024-frame period
and Peepo's shorter 128-frame period passed; `--period 128` selects the latter.
Host logs are in `build/host-smoke/`. The
non-realtime WSL server and Python probe produced scheduling xruns; this is a
functional host check and no xrun-performance claim is made.

The two LV2 plugins share their registry only within one process. Loading them
in separate jalv processes checks each descriptor individually, not pairing.
Ingen and the offline tests exercise the shared-object arrangement together.

`lv2lint` was built from upstream revision
`9661902c922650de99bd68e69ee0380d299aed3e` (0.17.151). Online, ELF and X11 tests
were disabled; symbol visibility was checked separately with `nm`. Two warnings
remain for each identity:

- **Plugin Author Email:** no `foaf:mbox` is supplied. The author name and GitHub
  homepage are present; an email address has not been invented or published.
- **Plugin Options Feature:** `options:options` is declared as the plan requires,
  but there are no supported or required options yet. Audio block-length
  handling arrives in Milestone 1; the scaffold accepts this optional feature
  without using it.

No implementation of memory, echo, CV modulation, routing, bypass or a return
audio ring is included in this milestone. Pair status currently means both
registry owners are present; stopped-peer detection is Milestone 1 work.

Release follow-up remains: perform the project's required physical Pi check and
record that evidence in Peepo, then create the `v0.0.1` release tag on the
reviewed commit. No release tag or remote publication is part of this local
validation record.
