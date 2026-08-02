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
| `0x0c` | Prepare to write DPI table | `0c <profile#> 80 00 00 00 00 <checksum>` — precedes the DPI-table primer+data writes on endpoint `0x04` | Implemented — this is the same "prepare table" step the LED command sequence already used (previously named `FEAT_ARM`). The 64-byte DPI table write itself is implemented for **stage 2 only** (`c18ctl dpi-set 2 <dpi>`) — see Open items for stages 1/3/4/5. |
| `0x0d` | Prepare to write button table | `0d <profile#> 80 00 00 00 00 <checksum>` — precedes the button-table write on endpoint `0x04` (also seen ahead of macro data uploads — likely a generic "prepare next table write" trigger, not table-specific) | Implemented (`c18ctl button`) |

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
64-byte header (`0f 00 1e 0a 19 19 05 ...`) as the LED payload, **and both are
preceded by the same `0x0c` "prepare table" opcode** — confirmed, not just
similar-looking. This is one generic 64-byte "config packet" format/pipeline
reused for at least two subsystems, with the meaning of the trailing bytes
depending on context (mode block for LED, X/Y DPI code + unknown region for
DPI).

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

### DPI value table (implemented for stage 2 only — `c18ctl dpi-set 2 <dpi>`)
Written as a 64-byte packet on interrupt endpoint `0x04`, preceded by a
64-byte primer packet (`ff ff 00 00 ... 00`) in the same transaction — same
primer already used by the LED write.

Table packet layout, captured while editing DPI stage 2 through multiple real
DPI values — only bytes 21/29 changed across those captures, everything else
(including bytes 7-9 and 40-63) stayed fixed, which is why this specific
template is trusted for stage 2 (`DPI_STAGE2_TEMPLATE` in `c18ctl.c`):
```
0f 00 1e 0a 19 19 05 00 00 00 64 64 01 c0 f0 03
01 01 19 19 27 <DPI_BYTE> 5c d0 f3 5c 73 00 27 <DPI_BYTE> 5c d0
f3 5c 73 00 1f ff 00 00 31 00 00 00 31 00 00 00
31 31 31 00 31 00 31 00 31 31 31 31 31 31 31 31
```
- Byte offset 21 = X-DPI code, byte offset 29 = Y-DPI code — both `<DPI_BYTE>`,
  looked up from the calibration table below (linear interpolation between
  known points).
- Bytes 7–9 and 40–63 are **not fixed in general** — they differ between
  Profile 1 and Profile 2's *default* tables (see Restore Defaults below)
  even though bytes 21/29 (the stage-2 DPI value) are identical between
  profiles. Since default-table bytes 7-9/40-63 aren't `00 00 00`/the pattern
  above, and a Restore-Defaults capture always resets *all 5 stages* at once
  (not just stage 2), the most likely explanation is that bytes 7-9/40-63
  encode the other 4 stages' data, while offsets 21/29 are hardwired to
  "stage 2" regardless of which stage is currently active via opcode `0x04`.
  **`c18ctl dpi-set` only ever writes the fixed bytes shown above (proven
  safe for stage-2-only edits); it does not attempt to preserve or touch
  whatever the other 4 stages' current values are** — so using it will reset
  stages 1/3/4/5 to whatever's baked into this template, not leave them
  alone. Not safe to extend to other stages without their own captures — see
  Open items.

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

## Button remapping (implemented — `c18ctl button <slot> <action> ...`)

Written as a 64-byte packet on interrupt endpoint `0x04` (no primer packet,
unlike the DPI table). One data-carrying submit + one zero-length completion
IRP.

**Caveat**: the device protocol has no readback, so every `c18ctl button`
call sends the *entire* 64-byte table built from the factory-default template
(`BUTTON_DEFAULT_TABLE` in `c18ctl.c`) with only the target slot patched —
meaning it resets every other button on that profile back to factory default
each time it's run. A real driver (or a future CLI revision) would need to
track the full current table client-side across calls to avoid this.

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
| 40–63 | (fixed trailer/reserved — not mapped to a button; confirmed identical across both profiles' factory defaults, see Default button table below) | |

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

### Default button table (byte-identical across both profiles, script-verified — this is `BUTTON_DEFAULT_TABLE` in `c18ctl.c`)

```
byte[ 0-15]: 01 00 f0 00 01 00 f1 00 01 00 f2 00 01 00 f4 00
byte[16-31]: 01 00 f3 00 07 00 03 00 00 00 2c 00 00 00 00 00
byte[32-47]: 00 00 1a 00 00 00 16 00 00 00 04 00 00 00 07 00
byte[48-63]: 00 00 00 00 00 00 00 00 04 00 01 00 04 00 02 00
```

| Bytes | Field | Value |
|---|---|---|
| 0-7 | Button 2 | `01 00 f0 00 01 00 f1 00` → marker=1, tag=0x00f0, type=0x0001, value=0x00f1 (241) |
| 8-11 | Button 3 | `01 00 f2 00` → type=0x0001, value=0x00f2 (242) |
| 12-15 | Button 4 | `01 00 f4 00` → type=0x0001, value=0x00f4 (244) |
| 16-19 | Button 5 | `01 00 f3 00` → type=0x0001, value=0x00f3 (243) |
| 20-23 | Button 6 | `07 00 03 00` → type=0x0007 (DPI function), value=0x0003 (DPI Cycle) |
| 24-31 | (fixed trailer) | `00 00 2c 00 00 00 00 00` |
| 32-35 | Button 7 | `00 00 1a 00` → type=0x0000 (keyboard), value=0x001a ("w") |
| 36-39 | Button 8 | `00 00 16 00` → type=0x0000 (keyboard), value=0x0016 ("s") |
| 40-63 | (fixed trailer) | `00 00 04 00 00 00 07 00 00 00 00 00 00 00 00 00 04 00 01 00 04 00 02 00` — confirmed identical across both profiles' factory defaults |

Buttons 2-5 all default to the not-yet-fully-decoded `type=0x0001` native-click
category (values 241-244). Buttons 7/8 defaulting to "w"/"s" (adjacent WASD
keys) is a nice sanity confirmation.

### Default DPI table — raw comparison (profile 2 vs profile 1, both script-verified as exactly 64 bytes)

Both profiles' default table has the same value (`0x39`) at byte offsets
21/29 (the stage-2 DPI position), but differ elsewhere:

```
Profile 2:
byte[ 0-15]: 0f 00 1e 0a 19 19 05 01 01 03 64 64 01 c0 f0 03
byte[16-31]: 01 01 19 19 22 39 5c d0 f3 5c 73 00 22 39 5c d0
byte[32-47]: f3 5c 73 00 1f ff 00 00 00 00 31 00 00 31 00 00
byte[48-63]: 31 00 00 31 00 00 31 00 00 31 00 00 31 00 00 31

Profile 1:
byte[ 0-15]: 0f 00 1e 0a 19 19 05 04 06 10 64 64 01 c0 f0 03
byte[16-31]: 01 01 19 19 22 39 5c d0 f3 5c 73 00 22 39 5c d0
byte[32-47]: f3 5c 73 00 1f ff 00 00 31 00 00 00 31 00 00 00
byte[48-63]: 31 31 31 00 31 00 31 00 31 31 31 31 31 31 31 31
```

Differences: bytes 7-9 (`01 01 03` vs `04 06 10`) and most of bytes 40-63.
Both profiles agree on bytes 21/29 (`0x39`), so the *active/stage-2* DPI
value is the same factory default for both profiles — the differing bytes
are something else per-profile (possibly per-stage flags, or all-5-stage
values, given each profile can have its own independent 5-stage DPI ladder).
**Still not decoded** — see Open items. Note this default table does *not*
match `DPI_STAGE2_TEMPLATE` (the one `c18ctl dpi-set` uses) at bytes 7-9/40-63
— that template came from a live stage-2-only edit capture, this one from a
full Restore Defaults reset; they agree only at offsets 21/29.

## Acceleration

**Confirmed to be entirely host-side/software.** Cycling the acceleration
slider through its full 0–10 range produced **zero** device-specific traffic —
only the fixed begin/commit transaction pair, byte-identical regardless of
slider value. No firmware involvement; nothing to implement in a device
driver for this setting.

## Open items / not yet fully mapped

Resolved this round: the earlier byte-count issues in the default-table
captures (61/58 bytes instead of 64, and the button table's missing bytes
40-63) were transcription gaps in the first capture pass, not real protocol
gaps — the corrected, script-verified 64-byte dumps above unblocked
`c18ctl button` (fully implemented) and `c18ctl dpi-set` (stage 2 only).
Remaining open items:

- **DPI stages 1, 3, 4, 5**: no template captured for editing these — only
  stage 2 has a validated multi-value capture (`DPI_STAGE2_TEMPLATE`). Needs
  the same kind of test done for stage 2 (edit one stage through several DPI
  values via the real UI, capture each, diff) repeated for the other 4
  stages, ideally also diffing against a stage-2 edit done *in the same
  session* to see whether editing one stage changes another stage's bytes
  elsewhere in the table (which would confirm/refute the "table encodes all
  5 stages at once" theory below).
- Bytes 7–9 and 40–63 of the **DPI table** packet: confirmed *not* fixed
  (differ between profiles' Restore-Defaults tables, and differ from the
  stage-2-edit template) — likely encode the other 4 stages' data and/or
  per-stage enable flags, not just the active stage-2 value at offset 21/29.
  Still not decoded; `c18ctl dpi-set` does not attempt to preserve whatever
  the device's current values are here (it can't — no readback), so it will
  reset stages 1/3/4/5 to whatever's baked into the stage-2-edit template
  rather than leave them untouched. Flagged loudly in `c18ctl`'s usage text
  and in `PROTOCOL.md`'s DPI table section above.
- Bytes 40–63 of the **button-remap** packet: confirmed fixed/identical
  across both profiles' factory defaults, so likely genuinely reserved/unused
  rather than buttons 9+ — but not proven definitively (no test was done
  where a UI action was expected to touch this region).
- `c18ctl button` writes the whole 64-byte table from the factory-default
  template every time (no readback exists to preserve other buttons' current
  state) — so setting one button resets every other button on that profile
  back to factory default. A real driver needs to track full table state
  client-side to avoid this.
- The DPI-table primer packet (`ff ff 00 00 ...`) purpose.
- Exact analytic formula (if any) for the DPI byte encoding above ~4800 DPI.
- `type=0x0001` (native mouse-click function) values `0xf0`-`0xf4`: category
  identified via factory defaults, but exact value→action mapping (Left/
  Right/Middle/Forward/Back click) not yet confirmed — needs a test explicitly
  assigning each native click action via the software UI.
- Macro-slot-ID assignment mechanism (how the device picks the `value` used
  by `type=0x0009`) — not understood, blocks macro upload implementation.
