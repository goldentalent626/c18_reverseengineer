# SOAI C18 Mouse — Linux LED Control

Reverse-engineered USB HID protocol for the SOAI C18 gaming mouse (USB VID:PID
`12c9:2003`), with a working Linux CLI for controlling its RGB lighting. Long-term
goal is a full [libratbag](https://github.com/libratbag/libratbag) driver so the
mouse can be configured on Linux via Piper, covering LEDs, DPI, and button remapping.

## Status

- **LED/lighting protocol: fully decoded and working.** off, static color/brightness,
  neon, breathing, flowing, and wave modes are all implemented in `c18ctl`.
- **Profile select, polling rate, and DPI active-stage select: decoded and working**
  (`c18ctl profile|polling|dpi-stage`).
- **DPI value table (per-stage DPI numbers) and button remapping: protocol partially
  decoded, not yet implemented** — blocked on incomplete/inconsistent capture data.
  See `PROTOCOL.md` → "Open items" for exactly what's missing.
- **Macro upload: not implemented** — the macro-slot-ID assignment mechanism isn't
  understood yet.
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
```

Colors: `red green blue yellow purple cyan white off`

`profile`/`polling`/`dpi-stage` default to profile 1 when the optional
`[profile]` argument is omitted.

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

DPI value-table and button-remap writes are the main gap — the protocol is
partially decoded (see `PROTOCOL.md` → "Open items") but the captured default-table
dumps are incomplete/inconsistent (wrong byte counts, and the button table's bytes
40–63 were never recorded). What's needed: a fresh Wireshark + USBPcap capture
(filtered to `usb.idVendor == 0x12c9 && usb.idProduct == 0x2003`) of the DPI table
write for each of the 5 stages, and of a button-remap write, with the full raw
64-byte payloads exported (not hand-transcribed) so byte counts can be verified.
