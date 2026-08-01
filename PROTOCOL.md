# C18 Gaming Mouse — Reverse-Engineered USB Protocol

Reverse-engineered via live USB capture (USBPcap/tshark on Windows) against the
vendor configuration software, plus captures against real hardware for the LED
subsystem. Reference for the Linux tools in this repo and, eventually, a
libratbag driver.

## Device identification

- **VID:PID**: `12C9:2003`
- **USB product string**: "USB Gaming Mouse"
- **Composite device, 3 HID interfaces**:
  - `MI_00` — boot mouse (standard cursor/button HID reports)
  - `MI_01` — system controller / consumer control (media keys passthrough)
  - `MI_02` — **vendor-defined interface** — this is the configuration channel; everything below happens here

## Endpoints (interface MI_02)

| Endpoint | Direction | Type | Purpose |
|---|---|---|---|
| `0x81` | IN | Interrupt | Standard live HID reports (mouse movement/buttons, 7 bytes; also carries MI_01 reports) — not part of the config protocol |
| `0x04` | OUT | Interrupt | Large 64-byte config writes: LED payloads, DPI/resolution table, button-profile table |
| `0x00` | — | Control | Compact 8-byte commands via `SET_REPORT` (Feature report, `wValue=0x0300`, `wIndex=2`, `wLength=8`); also macro data upload |

## Transaction model

Every configuration change — LED, DPI, buttons, polling rate, profile — follows
the same wrapper:

```
[0x01]  Begin transaction   (fixed: 01 88 00 00 00 00 00 76)
[ ... ] The actual command/data packet(s)
[0x02]  Select/commit active profile   (02 <profile#> 00 00 00 00 00 <checksum>)
```

The trailing `0x02` packet is **not a no-op/handshake** — it's a real "select
active profile" command that also serves as the commit/apply signal.

### Checksum algorithm (all 8-byte control packets)

```
checksum = 0xFF - sum(byte[0..6])
```

Equivalently: all 8 bytes of every command packet sum to exactly `0xFF` (255).
Verified against every opcode below, including the already-implemented LED
commands (see "LED / RGB" below) — same formula, same transaction wrapper.

## Opcode summary (8-byte control commands, `SET_REPORT`, `wIndex=2`)

| Opcode (byte0) | Command | Format | Status |
|---|---|---|---|
| `0x01` | Begin transaction | `01 88 00 00 00 00 00 76` (fixed) | Implemented (`c18ctl`) |
| `0x02` | Select/commit active profile | `02 <profile#> 00 00 00 00 00 <checksum>` | Implemented (`c18ctl profile`, and as the commit step of every command) |
| `0x03` | Set polling rate | `03 <profile#> <interval_ms> 00 00 00 00 <checksum>` | Implemented (`c18ctl polling`) |
| `0x04` | Set active DPI stage | `04 <profile#> <stage 1-5> 00 00 00 00 <checksum>` | Implemented (`c18ctl dpi-stage`) |
| `0x09` | Macro event data | `09 <event> <keycode> ...` (see Macros below) | Not implemented — macro-slot-ID assignment isn't understood (see Open items) |
| `0x0c` | Prepare to write DPI table | `0c <profile#> 80 00 00 00 00 <checksum>` — precedes the DPI-table primer+data writes on endpoint `0x04` | Implemented — this is the same "prepare table" step the LED command sequence already used (previously named `FEAT_ARM`); the 64-byte DPI *table* write itself is not implemented (see Open items) |
| `0x0d` | Prepare to write button table | `0d <profile#> 80 00 00 00 00 <checksum>` — precedes the button-table write on endpoint `0x04` (also seen ahead of macro data uploads — likely a generic "prepare next table write" trigger, not table-specific) | Not implemented (see Open items) |

Byte1 of opcodes `0x03`/`0x04`/`0x0c`/`0x0d` is the **target profile number**,
confirmed by a Restore Defaults capture that exercised these opcodes on
Profile 2 and got byte1=`02` throughout.

### LED / RGB — cross-reference to previously-used names

The LED implementation in `c18ctl.c` was built and verified against real
hardware before this opcode table was decoded from Windows captures. Both
describe the same wire protocol; here's the mapping (verified by recomputing
each fixed packet's checksum with the formula above — all match):

| Old name in `c18ctl.c` | Bytes | = Opcode |
|---|---|---|
| `FEAT_UNLOCK` | `01 88 00 00 00 00 00 76` | `0x01` begin transaction |
| `FEAT_ARM` | `0c 01 80 00 00 00 00 72` | `0x0c` prepare table write, profile 1 |
| `FEAT_FINAL` | `02 01 00 00 00 00 00 fc` | `0x02` select/commit, profile 1 |

`c18ctl.c` now builds these generically via `build_cmd()`/`checksum8()`
instead of using fixed byte arrays, which is what let `profile`/`polling`/
`dpi-stage` reuse the same code path.

**LED payload** (sent on endpoint `0x04` after the primer, between "prepare
table" and "commit"): 64 bytes starting `0f 00 1e 0a ...`; mode block at bytes
4–9 (`19 19 05 <mode> <speed> <param>`); for static color mode, 8 RGB triplets
fill bytes 40–63. Decoded modes: off (`0x00`), static (`0x01`), breathing
(`0x02`), neon (`0x03`), wave (`0x04`), flowing (`0x06`). Some speed/brightness
constants (breathing/wave/flowing fast & slow, static mid/low brightness) were
only captured at one data point on real hardware and are extrapolated — see
comments in `c18ctl.c`.

Interesting overlap: the DPI table packet (below) shares the exact same
64-byte header (`0f 00 1e 0a 19 19 05 ...`) as the LED payload. This looks
like one generic 64-byte "config packet" format reused across subsystems,
with the meaning of the trailing bytes depending on which "prepare" opcode
(`0x0c` DPI vs. presumably something else for LED) preceded it.

## Profiles

- **2 profiles total** (confirmed).
- Selected via opcode `0x02`: `01`=Profile 1, `02`=Profile 2.
- Buttons, DPI table, and macros all appear to be per-profile — every write
  captured was implicitly scoped to whichever profile was active via the
  trailing `0x02` commit.
- LED settings are also likely per-profile (Restore Defaults showed different
  RGB defaults per profile — see below), though this hasn't been directly
  tested; `c18ctl`'s LED commands default to profile 1 (matching how they were
  originally captured/verified) but now accept the same profile-targeting
  machinery as the other commands.

## Polling rate

Opcode `0x03`. Byte2 = **USB polling interval in milliseconds**
(`interval_ms = 1000 / Hz`) — standard HID `bInterval` semantics, not
device-specific.

| Rate | Byte |
|---|---|
| 1000 Hz | `0x01` |
| 500 Hz | `0x02` |
| 250 Hz | `0x04` |
| 125 Hz | `0x08` |

## DPI

### Active stage select
Opcode `0x04`, byte2 = stage number `1`-`5`. e.g. `04 01 02 00 00 00 00 f8` = select stage 2.

### DPI value table (not implemented — see Open items)
Written as a 64-byte packet on interrupt endpoint `0x04`, preceded by a
64-byte primer packet (`ff ff 00 00 ... 00`) in the same transaction — same
primer already used by the LED write.

Table packet layout (captured for DPI stage 2 only):
```
0f 00 1e 0a 19 19 05 <b7> <b8> <b9> 64 64 01 c0 f0 03 01 01 19 19
27 <X_DPI> 5c d0 f3 5c 73 00
27 <Y_DPI> 5c d0 f3 5c 73 00
1f ff 00 00 <bytes 40-63, not fixed — differ per profile, not decoded>
```
- Byte offset 21 = X-DPI code, byte offset 29 = Y-DPI code.
- Bytes 7–9 and 40–63 are **not fixed** — they differ between Profile 1 and
  Profile 2's default tables even though bytes 21/29 (the stage-2 DPI value)
  are identical between profiles. This means the full 64-byte table probably
  encodes all 5 DPI stages at once (or per-stage enable flags), not just
  stage 2 in isolation. **Not decoded.**

### DPI calibration table (stage 2, confirmed range 200–10000)

Not a single linear formula — non-linear/multi-region sensor register
encoding. Use as a direct lookup table with interpolation for untested
values.

| DPI | Byte | | DPI | Byte |
|---|---|---|---|---|
| 200 | `0x04` | | 2400 | `0x37` |
| 300 | `0x06` | | 2600 | `0x3b` |
| 400 | `0x08` | | 3200 | `0x49` |
| 600 | `0x0d` | | 4800 | `0x6f` |
| 800 | `0x12` | | 6400 | `0xc9` (reproduced) |
| 1200 | `0x1b` | | 6600 | `0xcc` |
| 1600 | `0x24` | | 10000 (max) | `0xf3` (reproduced) |
| 2000 | `0x2e` | | | |
| 2300 | `0x34` | | | |

Known region behavior:
- ≤400 DPI: exact `byte = DPI / 50`
- 600–4800 DPI: roughly `byte ≈ round(DPI / 42.86) − 1`, not exact — real per-step firmware rounding
- 4800+: steepens further, not fully characterized

## Button remapping (not implemented — see Open items)

Written as a 64-byte packet on interrupt endpoint `0x04` (no primer packet,
unlike the DPI table). One data-carrying submit + one zero-length completion
IRP.

### Packet layout (byte offsets)

| Bytes | Button | Format |
|---|---|---|
| 0–7 | Button 2 | `[marker=0x0001 LE][tag=0x00f0 LE][type LE][value LE]` — always full 4-word form |
| 8–11 | Button 3 | `[type LE][value LE]` |
| 12–15 | Button 4 | `[type LE][value LE]` |
| 16–19 | Button 5 | `[type LE][value LE]` |
| 20–23 | Button 6 | `[type LE][value LE]` |
| 24–31 | (fixed trailer, not a button) | |
| 32–35 | Button 7 | `[type LE][value LE]` |
| 36–39 | Button 8 | `[type LE][value LE]` |
| 40–63 | (fixed trailer/reserved — not yet mapped to a button, exact bytes unknown, see Open items) | |

All multi-byte fields are little-endian 16-bit words. Button 2 is the sole
exception with an explicit marker+tag prefix; all others are addressed purely
by position. Untouched slots retain stale/leftover bytes from whatever was
last written (or factory default) — not meaningful until first written.

### Type/value table

| Type | Category | Value meaning |
|---|---|---|
| `0x0000` | Keyboard key | USB HID keyboard usage code. `0x00`=disabled/no-op, `0x1E`="1", `0x1F`="2", `0x04`="a", `0x05`="b", `0x06`="c", `0x21`="4", `0x22`="5", `0x23`="6". Numpad number keys get normalized to the same codes as top-row numbers. (These match the standard USB HID Keyboard/Keypad usage page — not device-specific.) |
| `0x0003` | Consumer/media control | Standard USB HID Consumer Page usage code. `0xE9`=Volume Up, `0xEA`=Volume Down, `0xCD`=Play/Pause, `0xB7`=Stop. |
| `0x0007` | DPI function | `0x0001`=DPI Up, `0x0002`=DPI Down, `0x0003`=DPI Cycle |
| `0x0008` | Profile function | `0x0001`=Profile Up, `0x0002`=Profile Down, `0x0003`=Profile Cycle |
| `0x0009` | Macro reference | `value` = macro slot ID assigned when the macro was created (e.g. `0x1f`, `0x22`) — not a fixed/predictable ID |
| `0x0001` | Native/built-in mouse-click function (discovered via factory defaults) | `value` in range `0xf0`-`0xf4` seen so far (240-244). Not precisely mapped to specific actions yet — plausible guess is Left/Right/Middle/Forward/Back click, since these are the factory defaults for buttons 2-5. Needs a dedicated test (explicitly assign "Left Click" etc. via the software UI and observe the byte) to confirm exact mapping. |

## Macros

- **Keyboard-only** — mouse movement and mouse-button clicks recorded during
  "Start Recording" do **not** appear to be captured into the macro data at
  all, only keyboard keystrokes. This matches libratbag's own macro model (an
  array of `(type, value)` tuples), so this is not a limitation for a
  libratbag driver.
- Uploaded via opcode `0x09` `SET_REPORT` control transfers (8 bytes each),
  wrapped in the standard begin/commit transaction.
- Each keyboard event = 2 bytes: `[event_byte][HID_keycode]`
  - `event_byte` bit 7 (`0x80`): `0`=press, `1`=release
  - `event_byte` bits 0–6: timing/delay value, unit not calibrated (varies with real typing rhythm)
- The assigning button's slot in the button-profile packet gets `type=0x0009, value=<macro ID>`.
- **Not implemented**: the macro-slot-ID assignment mechanism isn't understood (see Open items).

## Restore Defaults (raw capture, for reference)

Clicking "Restore Default" (and applying) runs a full-table reset for
whichever profile is currently active. Captured on both profiles; sequence
for Profile 2 shown (Profile 1 identical apart from the profile-number byte):

```
01 88 00 00 00 00 00 76        begin transaction
02 02 00 00 00 00 00 fb        select profile 2
03 02 02 00 00 00 00 f8        set polling rate -> 02 (500 Hz)
04 02 01 00 00 00 00 f8        set active DPI stage -> 01 (stage 1)
0c 02 80 00 00 00 00 71        prepare DPI table write
  [endpoint 0x04] ff ff 00 00 ... (primer)
  [endpoint 0x04] <full 64-byte DPI table>
0d 02 80 00 00 00 00 70        prepare button table write
  [endpoint 0x04] <full 64-byte button table>
02 02 00 00 00 00 00 fb        commit
01 88 00 00 00 00 00 76        begin (second small transaction)
02 02 00 00 00 00 00 fb        commit
```

**Defaults confirmed:**
- Polling rate: **500 Hz** (both profiles)
- Active DPI stage: **stage 1** (both profiles)
- RGB (per user observation in software, not decoded from packets): Profile 1 = **Red, Wave mode (medium brightness)**; Profile 2 = **Blue, Standard mode (high brightness)**

### Default button table — bytes 0–39 confirmed, 40–63 missing

```
01 00 f0 00 01 00 f1 00   Button 2: marker=1, tag=0x00f0, type=0x0001, value=0x00f1 (241)
01 00 f2 00                Button 3: type=0x0001, value=0x00f2 (242)
01 00 f4 00                Button 4: type=0x0001, value=0x00f4 (244)
01 00 f3 00                Button 5: type=0x0001, value=0x00f3 (243)
07 00 03 00                Button 6: type=0x0007 (DPI function), value=0x0003 (DPI Cycle)
00 00 2c 00 00 00 00 00    (fixed trailer)
00 00 1a 00                Button 7: type=0x0000 (keyboard), value=0x001a ("w")
00 00 16 00                Button 8: type=0x0000 (keyboard), value=0x0016 ("s")
[bytes 40-63: not given numerically in this doc — see Open items]
```

Buttons 2-5 all default to the not-yet-fully-decoded `type=0x0001` native-click
category (values 241-244). Buttons 7/8 defaulting to "w"/"s" (adjacent WASD
keys) is a nice sanity confirmation.

### Default DPI table — raw comparison (profile 2 vs profile 1)

Both profiles' default table has the same value (`0x39`) at byte offsets
21/29 (the stage-2 DPI position), but differ elsewhere. **The raw hex given
for this comparison in the source capture notes does not parse to 64 bytes
per profile (61 and 58 bytes respectively) and is not usable as-is — see
Open items.** What's reliably known: bytes 7-9 and most of bytes 40-63 differ
between profiles; bytes 21/29 (`0x39`) agree.

## Acceleration

**Confirmed to be entirely host-side/software.** Cycling the acceleration
slider through its full 0–10 range produced **zero** device-specific traffic —
only the fixed begin/commit transaction pair, byte-identical regardless of
slider value. No firmware involvement; nothing to implement in a device
driver for this setting.

## Open items / not yet fully mapped

- **Blocking DPI-table-write implementation**: the raw default-DPI-table hex
  dumps captured this session don't parse to 64 bytes (61 bytes for Profile 2,
  58 for Profile 1 — see "Default DPI table" above) and don't even agree with
  each other in length, despite allegedly being two instances of the same
  64-byte structure. This needs a re-capture/re-export with exact byte counts
  before a table-write command can be safely implemented — writing a
  fabricated 64-byte table to real hardware risks corrupting the DPI ladder
  in ways that are hard to diagnose.
- **Blocking button-table-write implementation**: bytes 40–63 of the default
  button table are referenced ("matches previously mapped constant region
  exactly") but never given numerically in this capture. Bytes 0–39 (buttons
  2–8 + the 24-31 trailer) are fully specified and internally consistent, but
  a full 64-byte write needs all 64 bytes.
- Bytes 40–63 of the **button-remap** packet: likely reserved/unused rather
  than buttons 9+, but not proven definitively.
- Bytes 7–9 and 40–63 of the **DPI table** packet: confirmed *not* fixed
  (differ between profiles) — likely encode all 5 DPI stages and/or
  per-stage enable flags, not just the active stage-2 value at offset 21/29.
  Needs targeted testing (e.g., edit stage 1, 3, 4, 5 individually and diff)
  to map.
- The DPI-table primer packet (`ff ff 00 00 ...`) purpose.
- Exact analytic formula (if any) for the DPI byte encoding above ~4800 DPI.
- `type=0x0001` (native mouse-click function) values `0xf0`-`0xf4`: category
  identified via factory defaults, but exact value→action mapping (Left/
  Right/Middle/Forward/Back click) not yet confirmed — needs a test explicitly
  assigning each native click action via the software UI.
- Macro-slot-ID assignment mechanism (how the device picks the `value` used
  by `type=0x0009`) — not understood, blocks macro upload implementation.
