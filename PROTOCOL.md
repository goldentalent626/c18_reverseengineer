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
| `0x0d` | Prepare to write button table | `0d <profile#> 80 00 00 00 00 <checksum>` — precedes the button-table write on endpoint `0x04` | Implemented (`c18ctl button`) |
| `0x0f` | Prepare to upload extended per-reference data | `0f <table_type> 80 00 00 00 00 <checksum>` — general-purpose, not tied to one feature. `table_type=0x04` seen preceding both Armoury fire-parameter uploads and macro keyboard-event-data uploads (assigning any macro, even an unchanged existing one, re-triggers this and re-sends the macro's full data) | Not implemented (macro upload / Armoury fire-parameter upload out of scope — see Open items) |

Byte1 of opcodes `0x03`/`0x04`/`0x0c`/`0x0d` is the **target profile number**,
confirmed by a Restore Defaults capture that exercised these opcodes on
Profile 2 and got byte1=`02` throughout. (`0x0f`'s second byte is a *table
type* selector, not a profile number — different convention.)

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

### Per-stage offset claim (⚠️ not yet trusted — see Open items)

A later capture pass claimed all 5 stages' X/Y offsets are located, packed
consecutively at bytes 20-24 (X) and 28-32 (Y), one byte per stage:

| Stage | X byte offset | Y byte offset |
|---|---|---|
| 1 | 20 | 28 |
| 2 | 21 | 29 |
| 3 | 22 | 30 |
| 4 | 23 | 31 |
| 5 | 24 | 32 |

**This contradicts every raw byte dump given elsewhere in this document.**
Offsets 20, 22, 23, 24 (and their +8 mirrors 28, 30, 31, 32) have never once
been shown to vary across any capture in this doc — in both the
Profile-1/Profile-2 default-table dumps *and* the stage-2 multi-value edit
template, they're always exactly `22 5c d0 f3` (or `27 5c d0 f3` in the edit
template), i.e. they look like the fixed header/suffix of the `<marker>
<DPI_BYTE> 5c d0 f3 5c 73 00` 8-byte chunk, not five independent per-stage
values. Only offset 21/29 has ever been shown to change in a captured byte
dump. No raw before/after bytes were included to substantiate the "confirmed
by diffing stages 1/3/4/5" claim, so this table is **not implemented** —
`c18ctl dpi-set` stays restricted to stage 2 until this is resolved with
actual raw packet bytes for at least one non-stage-2 edit. See Open items.

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

**All 11 buttons confirmed** via a full sweep — every button assigned a
unique test key and diffed against the previous state, then cross-checked
byte-for-byte against the vendor software's default-button-layout diagram
(11/11 matched).

**Correction from an earlier capture pass**: Button 1 (the main left-click
button) **is remappable** and occupies its own slot — this wasn't tested
until later. What was originally read as "Button 2's marker+tag prefix"
(`[marker=0x0001][tag=0x00f0]`) was actually **Button 1's own `[type][value]`
pair** — it simply never changed in earlier captures (stuck at its factory
default, Left Click) so it looked like a constant prefix rather than live
data. There is no marker/tag concept — every button uses the identical
`[type][value]` 4-byte format. (`c18ctl.c`'s offsets for buttons 2-8 didn't
actually need to change from the earlier, differently-reasoned version — they
came out numerically identical either way — but slots 1, 9, 10, 11 were
entirely missing before this correction.)

**Caveat**: the device protocol has no readback, so every `c18ctl button`
call sends the *entire* 64-byte table built from the factory-default template
(`BUTTON_DEFAULT_TABLE` in `c18ctl.c`) with only the target slot patched —
meaning it resets every other button on that profile back to factory default
each time it's run. A real driver (or a future CLI revision) would need to
track the full current table client-side across calls to avoid this.

### Packet layout (byte offsets)

| Bytes | Button | Format |
|---|---|---|
| 0–3 | Button 1 (main click) | `[type LE][value LE]` |
| 4–7 | Button 2 | `[type LE][value LE]` |
| 8–11 | Button 3 | `[type LE][value LE]` |
| 12–15 | Button 4 | `[type LE][value LE]` |
| 16–19 | Button 5 | `[type LE][value LE]` |
| 20–23 | Button 6 | `[type LE][value LE]` |
| 24–27 | **Button 11** | `[type LE][value LE]` — out of numeric sequence, sits here rather than after button 10 |
| 28–31 | (fixed trailer) | `00 00 00 00` |
| 32–35 | Button 7 | `[type LE][value LE]` |
| 36–39 | Button 8 | `[type LE][value LE]` |
| 40–43 | Button 9 | `[type LE][value LE]` |
| 44–47 | Button 10 | `[type LE][value LE]` |
| 48–63 | (fixed trailer/reserved, 16 bytes) | unchanged in every capture, including the full 11-button sweep |

All multi-byte fields are little-endian 16-bit words. Every button uses the
identical `[type][value]` format — no exceptions. Untouched slots retain
stale/leftover bytes from whatever was last written (or factory default) —
not meaningful until first written.

### Confirmed factory defaults for all 11 buttons

Cross-verified against the vendor software's default-layout diagram — every
one matches:

| Button | Bytes | Default | type/value |
|---|---|---|---|
| 1 | 0-3 | Left Click | `type=0x0001, value=0xf0` |
| 2 | 4-7 | Menu | `type=0x0001, value=0xf1` |
| 3 | 8-11 | Wheel Click | `type=0x0001, value=0xf2` |
| 4 | 12-15 | IE Forward | `type=0x0001, value=0xf4` |
| 5 | 16-19 | IE Backward | `type=0x0001, value=0xf3` |
| 6 | 20-23 | DPI Cycle | `type=0x0007, value=0x0003` |
| 11 | 24-27 | Space | `type=0x0000, value=0x2c` |
| 7 | 32-35 | "w" | `type=0x0000, value=0x1a` |
| 8 | 36-39 | "s" | `type=0x0000, value=0x16` |
| 9 | 40-43 | "a" | `type=0x0000, value=0x04` |
| 10 | 44-47 | "d" | `type=0x0000, value=0x07` |

### Type/value table

| Type | Category | Value meaning | Implemented as |
|---|---|---|---|
| `0x0000` | Keyboard key | USB HID keyboard usage code. `0x00`=disabled/no-op, `0x1E`="1", `0x1F`="2", `0x04`="a"..`0x1D`="z". Matches the standard USB HID Keyboard/Keypad usage page — not device-specific. | `button <slot> key <a-z0-9\|0xHH>` |
| `0x0003` | Consumer/media control | Standard USB HID Consumer Page usage code. `0xE9`=Volume Up, `0xEA`=Volume Down, `0xCD`=Play/Pause, `0xB7`=Stop, `0xE2`=Mute, `0xB5`=Next Track, `0xB6`=Previous Track, `0x0183`=Open Player, `0x0194`=My Computer, `0x0223`=www Home. | `button <slot> media <name\|0xHH>` |
| `0x0001` | Native/built-in mouse-click function | `0xf0`=Left Click, `0xf1`=Menu (context menu), `0xf2`=Wheel Click (Middle), `0xf3`=Back, `0xf4`=Forward. Confirmed via live explicit assignment plus cross-reference against the factory-default diagram. | `button <slot> click <left\|menu\|middle\|back\|forward\|0xHH>` |
| `0x0007` | DPI function | `0x0001`=DPI Up, `0x0002`=DPI Down, `0x0003`=DPI Cycle | `button <slot> dpi-up\|dpi-down\|dpi-cycle` |
| `0x0008` | Profile function | `0x0001`=Profile Up, `0x0002`=Profile Down, `0x0003`=Profile Cycle | `button <slot> profile-up\|profile-down\|profile-cycle` |
| `(modifier<<8)` | Keyboard shortcut | `type`'s high byte is a USB HID keyboard modifier bitmask (low byte `0x00`): Ctrl=`0x01`, Shift=`0x02`, Alt=`0x04`, Win=`0x08`, freely OR'd together. `value`'s low byte is the primary key (held with the modifier); high byte is an optional second plain keystroke sent right after (`0x00` = none). See "Modifier encoding" below. | `button <slot> shortcut <mods> <key>` / `shortcut2 <mods> <key1> <key2>` |
| `0x0009 \| (repeat_mode<<8)` | Macro reference | `value` = macro slot ID assigned when the macro was created (e.g. `0x1f`, `0x22`) — not a fixed/predictable ID. `repeat_mode`: `0x00`="repeat N or interrupt", `0x01`="repeat until interrupted", `0x02`="repeat until released". This *references* an existing macro; this tool cannot create/upload one (see Macros below). | `button <slot> macro <0xHH id> <once\|until-interrupted\|until-released>` |
| `0xf00a` | Combo Key (Double-click / Three Click / Fire Button — all the same mechanism) | `value` = `[byte6: Delay ms, signed, capped ~127][byte7: Times, repeat count 0-255]`. Double-click = Delay 50ms/Times 2, Three Click = Delay 22ms/Times 3. | `button <slot> combo <times> <delay_ms>` |
| `0x000b` | Exact Control | Fixed `value=0x0f0f` — no settings dialog exists, this is the full encoding. Functional meaning unclear, but the wire encoding is settled. | `button <slot> exact-control` |
| `0x0c \| (repeat_mode<<8)` | Armoury (burst-fire preset) reference | Same 3-mode repeat enum as macros. `value` presumably a preset index, but not cross-checked against more than one preset. | **Not implemented** — preset-index semantics unconfirmed, see Open items |

### Modifier encoding (confirmed)

| Modifier | Bit | `type` value |
|---|---|---|
| (none) | — | `0x0000` |
| Ctrl | bit0 (`0x01`) | `0x0100` |
| Shift | bit1 (`0x02`) | `0x0200` |
| Alt | bit2 (`0x04`) | `0x0400` |
| Win/GUI | bit3 (`0x08`) | `0x0800` |

Combinations OR together as a real HID modifier byte (e.g. Ctrl+Shift =
`0x0300`), confirmed via the vendor software's "Assign a shortcut" dialog
across all 16 combinations with the same test key. Two-key sequences
confirmed via Maximize (`type=0x0400, value=0x1b2c` → Alt+Space, then "x")
and Minimize (`type=0x0400, value=0x112c` → Alt+Space, then "n") — classic
Windows system-menu mnemonics.

## Macros

- **Keyboard-only** — mouse movement and mouse-button clicks recorded during
  "Start Recording" do **not** appear to be captured into the macro data at
  all, only keyboard keystrokes. This matches libratbag's own macro model (an
  array of `(type, value)` tuples), so this is not a limitation for a
  libratbag driver.
- Uploaded via opcode `0x09` `SET_REPORT` control transfers (8 bytes each),
  wrapped in the standard begin/commit transaction. Assigning a macro to a
  button (even an *existing*, unchanged one) also re-transmits its full
  keyboard-event data via opcode `0x0f`, `table_type=0x04`.
- Each keyboard event = 2 bytes: `[event_byte][HID_keycode]`
  - `event_byte` bit 7 (`0x80`): `0`=press, `1`=release
  - `event_byte` bits 0–6: timing/delay value, unit not calibrated (varies with real typing rhythm)
- The assigning button's slot gets `type = 0x0009 | (repeat_mode<<8), value=<macro ID>` — confirmed via a direct test (existing macro ID `0x0004`) and via 3 isolated repeat-mode tests. **`c18ctl button <slot> macro <id> <mode>` implements the reference side of this** (type/value construction) but **cannot create/upload a macro** — the macro-slot-ID assignment mechanism (how the device picks an ID when a macro is first created) still isn't understood, so this only works for an ID you already know from having created the macro via the vendor software.

## Armoury (burst-fire macro, "N Key") — not implemented

A separate, unrelated sub-protocol from the keyboard-macro system above:
burst-fire configurator (projectile count, per-projectile strike interval,
X/Y recoil correction, repeat mode, cycle count), managed as named presets
("guns"). "N Key" (not "Close the N Key", a different menu item that just
re-exposes the `type=0x0001` click submenu) opens a popup to assign either an
Armoury preset or a macro to a button.

- **Button-slot reference**: `type = 0x0c | (repeat_mode<<8)`, same 3-mode
  enum as macros. `value` presumably a preset index — not cross-checked
  against more than one active preset, so not implemented.
- **Fire-parameter packet**: triggered by opcode `0x0f` (`table_type=0x04`)
  + 64-byte write. Confirmed for a single-projectile preset via an isolated
  one-field-at-a-time test:
  ```
  byte[0-3]: 00 01 02 f0     <- header (byte1 may be projectile count = 1)
  byte[4]:   0x81 (no X/Y correction) or 0x82 (X/Y correction active)
  byte[5]:   f0 (constant)
  [if active] X offset:  LE 16-bit, unsigned (50 -> 0x0032)
  [if active] Y correction: LE 16-bit, SIGNED, sign-inverted from the UI
              value (entering 77 -> stored as -77, 0xffb3)
  strike interval: LE 16-bit = 0x0200 + (interval_ms / 20)
                   confirmed: 500ms -> 0x0219, 900ms -> 0x022d
  ```
  Variable-length, presence-based layout (fields only present when active),
  not fixed offsets like the DPI/button tables. Multi-projectile presets
  showed a repeating per-projectile chunk structurally, but weren't
  cross-verified byte-for-byte against this single-projectile formula.
  **Not implemented** — out of scope without the preset-index question
  resolved and multi-projectile support confirmed.

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

See "Button remapping" above for the authoritative, corrected byte-offset
table (all 11 buttons). Applied to this dump: buttons 1-5 default to
`type=0x0001` native-click values (Left/Menu/Wheel/Forward/Back), button 6 =
DPI Cycle, button 11 (offset 24-27) = Space, buttons 7-10 = "w"/"s"/"a"/"d" —
a complete match to the vendor software's own default-layout diagram, all
11/11 confirmed live.

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

Resolved across this round of captures: the earlier byte-count issues in the
default-table captures (61/58 bytes instead of 64, and the button table's
missing bytes 40-63) were transcription gaps, not real protocol gaps;
Button 1's "marker+tag" was a misreading, not a real quirk (it's just Button
1's own type/value pair, unchanged in earlier captures); and buttons 9-11
were simply never tested before. All 11 buttons, the native-click value
table, keyboard-shortcut modifier encoding, Combo Key, and Exact Control are
now implemented in `c18ctl button`. Remaining open items:

- **⚠️ DPI per-stage offset claim needs raw-byte verification before
  implementation.** A capture pass claims stages 1/3/4/5's X/Y DPI bytes live
  at offsets 20/22/23/24 (X) and 28/30/31/32 (Y), packed consecutively next
  to the known stage-2 offset 21/29. But every raw dump actually shown in
  this document — both profile defaults and the stage-2 multi-value edit
  template — has those exact offsets fixed at `22 5c d0 f3` / `27 5c d0 f3`
  in every single capture, never varying. That pattern looks like the fixed
  header+suffix of the `<header> <DPI_BYTE> 5c d0 f3 5c 73 00` chunk, not
  five independent per-stage values, and no raw before/after bytes were
  included to substantiate the "confirmed by diffing stages 1/3/4/5" claim.
  **Need**: the actual raw packet bytes (before/after) from editing DPI stage
  1 (or any of 3/4/5) through at least two different DPI values, the same
  rigor already applied to stage 2. Until then `c18ctl dpi-set` stays
  restricted to stage 2. See the "Per-stage offset claim" callout in the DPI
  section above.
- Bytes 7–9 and 40–63 of the **DPI table** packet: confirmed *not* fixed
  (differ between profiles' Restore-Defaults tables, and differ from the
  stage-2-edit template) — still not decoded, possibly related to the
  per-stage question above. `c18ctl dpi-set` does not attempt to preserve
  whatever the device's current values are here (it can't — no readback), so
  it will reset whatever's encoded there to the stage-2-edit template's
  values rather than leave them untouched.
- Bytes 28–31 and 48–63 of the **button-remap** packet: confirmed
  fixed/identical across both profiles' factory defaults and unchanged
  through the full 11-button sweep, so likely genuinely reserved — but not
  proven via a test that specifically targeted this region.
- `c18ctl button` writes the whole 64-byte table from the factory-default
  template every time (no readback exists to preserve other buttons' current
  state) — so setting one button resets every other button on that profile
  back to factory default. A real driver needs to track full table state
  client-side to avoid this.
- The DPI-table primer packet (`ff ff 00 00 ...`) purpose.
- Exact analytic formula (if any) for the DPI byte encoding above ~4800 DPI.
- Whether "Sniper Mode" (hold-to-lower-DPI) or a "DPI Reset" function exists
  distinct from Up/Down/Cycle — not checked.
- Whether "Disabled" is genuinely just `type=0x0000, value=0x00`, or a
  distinct explicit disable flag exists elsewhere.
- Macro-slot-ID assignment mechanism (how the device picks an ID when a
  macro is first created) — not understood, blocks macro *upload*
  (referencing an already-known ID is implemented, see Button remapping).
- **Armoury**: repeat-mode encoding resolved (shares the macro's 3-mode
  enum), but whether `value` encodes a preset index when multiple presets
  exist isn't cross-checked, and multi-projectile fire-parameter encoding
  isn't confirmed byte-for-byte. Not implemented.
