# SOAI C18 Mouse — Linux Control CLI

Reverse-engineered USB HID protocol for the SOAI C18 gaming mouse (USB VID:PID
`12c9:2003`), with a working Linux CLI (`c18ctl`) covering RGB lighting, profile
select, polling rate, DPI active-stage select, DPI stage-2 value, and full
button remapping (all 11 buttons — keys, media keys, native clicks, DPI/profile
functions, keyboard-modifier shortcuts, Combo Key, Exact Control, and macro
references). Long-term goal is a full
[libratbag](https://github.com/libratbag/libratbag) driver so the mouse can be
configured on Linux via Piper.

## Status

- **LED/lighting: fully decoded and working.** off, static color/brightness,
  neon, breathing, flowing, and wave modes.
- **Profile select, polling rate, DPI active-stage select: decoded and working**
  (`c18ctl profile|polling|dpi-stage`).
- **DPI stage-2 value: decoded and working** (`c18ctl dpi-set 2 <dpi>`). Stages
  1/3/4/5 have a claimed-but-unverified offset table — see `PROTOCOL.md` →
  "Open items" for why it's not implemented yet.
- **Button remapping: fully decoded and working for all 11 buttons**
  (`c18ctl button`) — keyboard keys, media keys, native mouse clicks,
  DPI/profile functions, keyboard-modifier shortcuts (single and two-key),
  Combo Key (Double-click/Three-click/Fire Button), Exact Control, and macro
  references. Real limitation: no readback exists, so every call rewrites the
  whole table and resets any other previously-set button back to factory
  default.
- **Macro upload: not implemented** — referencing an existing macro by ID
  works, but the macro-slot-ID assignment mechanism (how the device picks an
  ID for a *new* macro) isn't understood.
- **Armoury (burst-fire presets): not implemented** — preset-index semantics
  unconfirmed.
- **libratbag driver: not started** — blocked on the above.

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
sudo ./c18ctl dpi-set    <stage=2> <dpi 200-10000> [profile]

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
omitted. `dpi-set` only supports stage 2 right now — see `PROTOCOL.md` for an
open question about whether the other 4 stages' offsets are actually located
correctly. Every `button` call rewrites the whole 64-byte button table, so it
resets any other previously-set button on that profile back to factory
default — there's no way to read the device's current table back to preserve
it.

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

DPI stages 1, 3, 4, and 5 are the main gap. A claimed offset table for them
exists but doesn't match the raw bytes shown anywhere else in `PROTOCOL.md` —
see "Per-stage offset claim" and "Open items" there for the specifics. What's
needed: the actual raw before/after packet bytes (not just a claimed offset
table) from a Wireshark + USBPcap capture (filtered to `usb.idVendor ==
0x12c9 && usb.idProduct == 0x2003`) of editing one non-stage-2 DPI stage
through at least two different DPI values, exported as full raw 64-byte
payloads (not hand-transcribed) — the same rigor the stage-2 template already
has. Once that lands, `dpi-set` can be extended past stage 2.
