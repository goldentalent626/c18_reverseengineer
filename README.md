# SOAI C18 Mouse — Linux Control CLI

Reverse-engineered USB HID protocol for the SOAI C18 gaming mouse (USB VID:PID
`12c9:2003`), with a working Linux CLI (`c18ctl`) covering RGB lighting, profile
select, polling rate, DPI (both active-stage select and all 5 stages' DPI
values), and full button remapping (all 11 buttons — keys, media keys, native
clicks, DPI/profile functions, keyboard-modifier shortcuts, Combo Key, Exact
Control, and macro references). Long-term goal is a full
[libratbag](https://github.com/libratbag/libratbag) driver so the mouse can be
configured on Linux via Piper.

## Status

- **LED/lighting: fully decoded and working.** off, static color/brightness,
  neon, breathing, flowing, and wave modes.
- **Profile select, polling rate, DPI active-stage select, DPI value (all 5
  stages): fully decoded and working** (`c18ctl profile|polling|dpi-stage|dpi-set`).
- **Button remapping: fully decoded and working for all 11 buttons**
  (`c18ctl button`) — keyboard keys, media keys, native mouse clicks,
  DPI/profile functions, keyboard-modifier shortcuts (single and two-key),
  Combo Key (Double-click/Three-click/Fire Button), Exact Control, and macro
  references. Real limitation: no readback exists, so every call rewrites the
  whole table and resets any other previously-set button back to factory
  default (same limitation applies to `dpi-set`).
- **Macro creation and upload: fully decoded and working** (`c18ctl
  macro-create <slot> <id> <mode> <key1> [key2 ...]`). Macro creation is
  100% host-side — the device never negotiates or allocates an ID, so a
  driver is free to pick any unused one itself — and content upload goes via
  opcode `0x0f`, including multi-packet macros beyond one 64-byte payload
  (verified up to 2 packets / 48 events). Inter-key wait blocks (deliberate
  multi-second pauses) are structurally decoded but not yet calibrated or
  implemented — see `PROTOCOL.md`.
- **Armoury (burst-fire presets): mostly decoded, not implemented as a CLI
  command** — repeat-mode encoding and the single-projectile fire-parameter
  fields are confirmed, but where projectile count and cycle count are
  actually encoded on the wire was never located (both are real, UI-enforced
  fields — see `PROTOCOL.md`). Not implemented regardless; the
  variable-length packet format wasn't worth the added CLI surface without a
  concrete use case.
- **libratbag driver: not started** — the remaining protocol gaps (macro
  wait-block timing, Armoury projectile/cycle count encoding) are minor
  enough that this is now the practical next step rather than something
  blocked on further RE.

See [`PROTOCOL.md`](PROTOCOL.md) for the full reverse-engineered wire protocol.

## Requirements

- Linux, `gcc`, `pkg-config`
- `libusb-1.0` development package (e.g. `libusb-1.0-0-dev` on Debian/Ubuntu,
  `libusb-1.0-devel` on Fedora/openSUSE)
- `sudo`/root access to the mouse's USB device (raw control + interrupt transfers)
- Run all commands from a real interactive terminal — `sudo` needs a TTY to prompt
  for a password.

## Building

```sh
gcc c18ctl.c -o c18ctl $(pkg-config --cflags --libs libusb-1.0)
gcc c18_usb_seq3.c -o c18_usb_seq3 $(pkg-config --cflags --libs libusb-1.0)
```

Both binaries talk directly to the mouse over USB (claiming all 3 HID interfaces to
keep the kernel driver out of the way), so they need root.

## Usage

### `c18ctl` — main CLI

```sh
sudo ./c18ctl off
sudo ./c18ctl static <color> [high|mid|low]
sudo ./c18ctl neon      <fast|mid|slow>
sudo ./c18ctl flowing   <fast|mid|slow>
sudo ./c18ctl breathing <fast|mid|slow>
sudo ./c18ctl wave      <fast|mid|slow>

sudo ./c18ctl profile    <1|2>
sudo ./c18ctl polling    <125|250|500|1000> [profile]
sudo ./c18ctl dpi-stage  <1-5> [profile]
sudo ./c18ctl dpi-set    <stage 1-5> <dpi 200-10000> [profile]

sudo ./c18ctl button <slot 1-11> off [profile]
sudo ./c18ctl button <slot 1-11> key <a-z0-9|0xHH> [profile]
sudo ./c18ctl button <slot 1-11> media <volup|voldown|playpause|stop|mute|next|prev|mediaplayer|mycomputer|home|0xHH> [profile]
sudo ./c18ctl button <slot 1-11> click <left|menu|middle|back|forward|0xHH> [profile]
sudo ./c18ctl button <slot 1-11> dpi-up|dpi-down|dpi-cycle [profile]
sudo ./c18ctl button <slot 1-11> profile-up|profile-down|profile-cycle [profile]
sudo ./c18ctl button <slot 1-11> shortcut <mods> <key> [profile]
sudo ./c18ctl button <slot 1-11> shortcut2 <mods> <key1> <key2> [profile]
sudo ./c18ctl button <slot 1-11> combo <times> <delay_ms> [profile]
sudo ./c18ctl button <slot 1-11> exact-control [profile]
sudo ./c18ctl button <slot 1-11> macro <0xHH id> <once|until-interrupted|until-released> [profile]
```

Colors: `red green blue yellow purple cyan white off`

`mods` is any combination of `c`/`s`/`a`/`w` (Ctrl/Shift/Alt/Win), or
`-`/`none`. `shortcut2`'s `key2` is sent plain right after `key1` (e.g. the
classic Alt+Space-then-X "Maximize" mnemonic). `combo` drives the shared
Double-click/Three-click/Fire-button mechanism; `delay_ms` is clamped to 127
(the device's confirmed max — it's a signed byte). `macro` only *references*
an existing macro ID created via the vendor software; this tool can't
upload/create macros yet.

Button numbering: 1 is the main/left-click button; 2-8 are the usual extra
buttons; 11 sits physically between 6 and 7 in the wire protocol (there's no
button "9"/"10" gap in the numbering as shown on the mouse — see
`PROTOCOL.md` for the exact byte layout).

All commands default to profile 1 when the optional `[profile]` argument is
omitted. `dpi-set` writes the whole DPI table, seeded from the target
profile's factory-default table with only the requested stage's 2 bytes
patched — bytes elsewhere in that table (still undecoded, see `PROTOCOL.md`)
are preserved as-is from the default rather than from whatever the device's
actual current state is (impossible to know without readback). Every
`button` call similarly rewrites the whole 64-byte button table, resetting
any other previously-set button on that profile back to factory default.

Example:

```sh
sudo ./c18ctl static red high
sudo ./c18ctl breathing slow
sudo ./c18ctl off
```

### Shell wrappers

These call `c18_usb_seq3` (built above) with pre-baked 64-byte payloads instead of
building them at runtime:

```sh
sudo ./c18-led.sh {off|static|breathing|flowing|wave|neon-fast|neon-mid|neon-slow}
sudo ./c18ctl.sh {off|static|breathing|flowing|wave} 
sudo ./c18ctl.sh neon {fast|mid|slow}
sudo ./c18color.sh {red|green|blue|yellow|purple|cyan|white|off}
sudo ./c18static.sh <color> [high|mid|low]
```

`c18color.sh` and `c18static.sh` patch a captured static-mode payload with a new RGB
tail via a small Python heredoc before sending it — useful as a reference for how the
64-byte payload's color tail is laid out (bytes 40–63, 8 RGB triplets).

## Repo layout

| Path | What it is |
|---|---|
| `PROTOCOL.md` | Full reverse-engineered wire protocol reference (opcodes, checksums, DPI/button tables, open items). |
| `c18ctl.c` / `c18ctl` | Main CLI — builds payloads/commands at runtime and drives the full USB begin→prepare→primer→payload→commit sequence. |
| `c18_usb_seq3.c` / `c18_usb_seq3` | Generic backend: takes a raw 64-byte hex payload and runs the same USB sequence. Used by the shell scripts. |
| `c18*.sh` | Shell wrappers around `c18_usb_seq3` with hardcoded known-good payloads. |
| `soai-rdesc-000B/C/D.hex` | Captured USB HID report descriptors for the mouse's 3 interfaces (standard mouse, vendor 64-byte I/O, vendor feature). |
| `docs/vendor-software/*.png` | Screenshots of the vendor Windows configuration software's 4 tabs (Button, Performance, Lighting, Armoury) — visual reference for the UI these captures were taken against; cross-referenced from `PROTOCOL.md`. |
| `attic/` | Retired prototype tools from the reverse-engineering process (`c18_usb`, `c18_usb_seq`, `c18_usb_seq2`, `c18_feat`, `c18_feat8`), kept for reference, not built by default. |

## Protocol summary

Full detail — opcode table, checksum algorithm, DPI calibration table, button
type/value table, macro format, and everything not yet decoded — lives in
[`PROTOCOL.md`](PROTOCOL.md). Short version:

- 3 USB HID interfaces; the vendor control interface is interface 2, interrupt OUT
  endpoint `0x04`.
- Every config change (LED, DPI, buttons, polling, profile) is wrapped in the same
  transaction: `0x01` begin (8-byte SET_REPORT) → command/data packet(s) → `0x02`
  select/commit (8-byte SET_REPORT). 64-byte table writes (LED payload, DPI table)
  are additionally preceded by an `0x0c`/`0x0d` "prepare table" command and, for the
  DPI table, a 64-byte primer packet (`ff ff 00...`).
- All 8-byte control commands are `SET_REPORT` (`bmRequestType=0x21`,
  `bRequest=0x09`, `wValue=0x0300`) and checksum as `0xFF - sum(byte[0..6])`.
- LED 64-byte payload starts `0f 00 1e 0a ...`; the mode block is at bytes 4–9
  (`19 19 05 <mode> <speed> <param>`); for static color mode, 8 RGB triplets fill
  bytes 40–63. Decoded modes: off (`0x00`), static (`0x01`), breathing (`0x02`),
  neon (`0x03`), wave (`0x04`), flowing (`0x06`).

Some speed/brightness constants (breathing/wave/flowing fast & slow, static mid/low
brightness) were only captured at one data point on real hardware and are
extrapolated in code — see comments in `c18ctl.c` for details on which values are
confirmed vs. inferred.

## Contributing captures

Macro upload is resolved; the remaining gaps are smaller and more targeted:

- **Macro inter-key wait blocks**: the 4-byte `[00 03][00 <value>]` pause
  marker and its `0x7f` (127) ceiling are confirmed, but the exact
  ms-per-unit scale isn't calibrated. Needs a stopwatch-precise single-key
  recording with one long, precisely-timed pause (not counted out loud),
  isolating one clean unclamped data point.
- **Armoury projectile/cycle count encoding**: the vendor UI enforces
  `1-8` projectiles and `1-255` cycles (see `docs/vendor-software/
  armoury-tab.png`), but neither field's byte offset has been located —
  only ruled out of the multi-projectile chunk's fixed 2-byte header. Needs
  captures isolating projectile count (1 vs 2 vs 3+) and cycle count
  independently, the same way the per-projectile interval/X/Y fields were
  isolated.

See `PROTOCOL.md` → "Open items" for the full list (DPI-table bytes 7-9/40-63
and the button-remap trailer are also technically undecoded, but both are
confirmed fixed/inert, so they don't block anything).
