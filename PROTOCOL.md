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
| `0x0c` | Prepare to write DPI table | `0c <profile#> 80 00 00 00 00 <checksum>` — precedes the DPI-table primer+data writes on endpoint `0x04` | Implemented — this is the same "prepare table" step the LED command sequence already used (previously named `FEAT_ARM`). The 64-byte DPI table write itself is implemented for all 5 stages (`c18ctl dpi-set <stage> <dpi>`). |
| `0x0d` | Prepare to write button table | `0d <profile#> 80 00 00 00 00 <checksum>` — precedes the button-table write on endpoint `0x04` | Implemented (`c18ctl button`) |
| `0x0f` | Prepare to upload extended per-reference data | `0f <target_ID> 80 00 00 00 00 <checksum>` — second byte is the **macro or Armoury preset ID** being uploaded to (not a profile number, and not a generic "table type" as an earlier version of this doc claimed), followed by that ID's full content on endpoint `0x04`. Confirmed three-for-three across independent macro-ID tests — see Macros below for the full worked example, including a multi-packet (>1 write) case. | Reference-only write (`c18ctl button <slot> macro <id>`) and macro content upload (`c18ctl macro-create`) are both implemented; Armoury fire-parameter upload via this opcode is not — see Open items |

**Note on `SET_REPORT (0x09)`**: every command in this table is sent as a USB
HID `SET_REPORT` class request — `bRequest=0x09` in the USB control transfer
itself. This is a **USB-level wrapper, not a payload opcode** — there is no
"opcode `0x09`" in the payload sense. An earlier version of this doc
mistakenly listed `0x09` here as a macro-content payload opcode; that
confusion came from conflating it with `type=0x0009`, the unrelated
button-slot macro-reference field (see Type/value table below). Macro
content is actually uploaded via opcode `0x0f` above.

Byte1 of opcodes `0x03`/`0x04`/`0x0c`/`0x0d` is the **target profile number**,
confirmed by a Restore Defaults capture that exercised these opcodes on
Profile 2 and got byte1=`02` throughout. (`0x0f`'s second byte is a *target
ID* — a macro or Armoury preset number — not a profile number; different
convention.)

### LED / RGB — cross-reference to previously-used names

The LED implementation in `c18ctl.c` was built and verified against real
hardware before this opcode table was decoded from Windows captures. Both
describe the same wire protocol; here's the mapping (verified by recomputing
each fixed packet's checksum with the formula above — all match). See
`docs/vendor-software/lighting-tab.png` for the vendor UI these modes/colors
come from.

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

See `docs/vendor-software/performance-tab.png` for the vendor UI — DPI
slider (200–10000), the 5 "Configuration sensitivity level" checkboxes
(ruled out as an explanation for the DPI table's undecoded bytes 7-9/40-63,
see Open Items), acceleration slider (0–10), and the Rate of return / polling
rate checkboxes (125/250/500/1000 Hz).

### Active stage select
Opcode `0x04`, byte2 = stage number `1`-`5`. e.g. `04 01 02 00 00 00 00 f8` = select stage 2.

### DPI value table (implemented for all 5 stages — `c18ctl dpi-set <stage> <dpi>`)
Written as a 64-byte packet on interrupt endpoint `0x04`, preceded by a
64-byte primer packet (`ff ff 00 00 ... 00`) in the same transaction — same
primer already used by the LED write.

Each stage occupies one byte for X and a mirrored byte for Y, 8 bytes apart,
at consecutive offsets:

| Stage | X byte offset | Y byte offset |
|---|---|---|
| 1 | 20 | 28 |
| 2 | 21 | 29 |
| 3 | 22 | 30 |
| 4 | 23 | 31 |
| 5 | 24 | 32 |

**Confirmed with raw before/after packet bytes**, not just a claimed table —
independently re-verified byte-for-byte for this document. Source: a
baseline capture (Profile 1 factory default) followed by 4 sequential edits
(stage1→1000, stage3→3000, stage4→5000, stage5→7000, each applied before the
next):

```
baseline:      0f001e0a191905040610646401c0f0030101191922395cd0f35c730022395cd0f35c73001fff0000310000003100000031313100310031003131313131313131
stage1=1000:   0f001e0a191905040610646401c0f0030101191916395cd0f35c730016395cd0f35c73001fff0000310000003100000031313100310031003131313131313131
stage3=3000:   0f001e0a191905040610646401c0f00301011919163945d0f35c7300163945d0f35c73001fff0000310000003100000031313100310031003131313131313131
stage4=5000:   0f001e0a191905040610646401c0f0030101191916394573f35c730016394573f35c73001fff0000310000003100000031313100310031003131313131313131
stage5=7000:   0f001e0a191905040610646401c0f0030101191916394573d05c730016394573d05c73001fff0000310000003100000031313100310031003131313131313131
```

Diffed step-by-step: `baseline→stage1=1000` changes only bytes 20/28
(`0x22→0x16`); `→stage3=3000` changes only 22/30 (`0x5c→0x45`);
`→stage4=5000` changes only 23/31 (`0xd0→0x73`); `→stage5=7000` changes only
24/32 (`0xf3→0xd0`). Exactly one byte pair per step, nothing else in the
64-byte payload moves at any step, and each change lands exactly where the
offset table predicts. Byte values also cross-check against the calibration
table below via linear interpolation: exact at 1000 DPI and 7000 DPI; 3000
DPI comes out 1 byte low and 5000 DPI comes out 7 bytes low, both consistent
with the calibration table's own pre-existing "not exact"/"not fully
characterized" caveats for those DPI ranges (see below) — not a sign the
offset table is wrong.

**Bytes 7–9 and 40–63: exact semantic meaning still unknown, but
operationally resolved.** They differ between Profile 1 and Profile 2's
default tables (see Restore Defaults below), so they're not a hardcoded
constant across the whole device — but within a given profile, they've been
shown **deterministic and unaffected by every testable UI control**:
confirmed unchanged across a stage1→1200→800→1200 DPI sequence (the two
1200-DPI writes came back byte-for-byte identical, ruling out a
counter/timestamp/nonce), and ruled out as encoding the "Configuration
sensitivity level" checkboxes, the profile name/description text, or
per-stage data (the real per-stage offsets are 20-24/28-32, confirmed
separately above and don't overlap this region). No UI action found across
either profile ever touches these bytes. Conclusion: **a fixed, per-profile
constant** — safe to cache from the factory-default table and always resend
unchanged, which is exactly what `c18ctl dpi-set` already does: it seeds
every write from the *target profile's* factory-default table
(`PROFILE1_DPI_DEFAULT`/`PROFILE2_DPI_DEFAULT` in `c18ctl.c`) and patches
only the 2 target-stage bytes, leaving bytes 7-9/40-63 exactly as they are
in that default table. No code change needed here — this capture confirms
the existing implementation's approach was already correct.

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
(11/11 matched) — see `docs/vendor-software/button-tab.png` for the
diagram itself.

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
| `0x0c \| (repeat_mode<<8)` | Armoury (burst-fire preset) reference | Same 3-mode repeat enum as macros. `value` is a fixed `0x0400` marker, not a preset index — confirmed identical across two different named presets ("Armoury1"/"Armoury2"); the device has no persistent per-preset ID, see Armoury section. | **Not implemented** — protocol fully decoded, just not built as a CLI command, see Armoury section |

**Combo Key field ranges confirmed** (merged from the Windows capture box):
Fire Button left at its popup defaults produced `byte6=0x02` (2ms)/`byte7=0x00`
(0 times), matching the popup's own shown defaults exactly. Requesting a
300ms delay produced `byte6=0x7f` (127) — the max positive value a signed
8-bit field can hold — not a wraparound/truncation to 44 (`300 mod 256`),
confirming `Delay` really is silently clamped at its real maximum rather
than just "capped ~127" as an estimate. `Times=50` in the same test came
back exact (`0x32`), confirming `Times` has no such ceiling.

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
- Each keyboard event = 2 bytes: `[event_byte][HID_keycode]`
  - `event_byte` bit 7 (`0x80`): `0`=press, `1`=release
  - `event_byte` bits 0–6: **short** timing value — appears to represent how
    long the key was physically held (naturally brief, tens of ms), not
    inter-key pause time. Unit not calibrated.
  - **Correction, confirmed via real hardware testing (libratbag driver
    bring-up)**: a value of exactly `0` here is **not** a safe/harmless
    default as earlier versions of this doc claimed — it makes the device's
    macro player either silently no-op the button press (`repeat_mode=0x00`)
    or hang the device outright on press (`repeat_mode=0x01`/`0x02`), even
    though the upload transaction itself always completes and is
    acknowledged cleanly (confirmed via the device's `GET_REPORT` status
    echo, see the Feature-report note below). A small non-zero value that
    isn't identical across every event works correctly — e.g. the
    "no delay" recording mode's own real captured bytes (see "Two distinct
    macro recording modes" below) use `1`/`2`, never `0` and never a single
    repeated constant. **`c18ctl macro-create` and the libratbag driver now
    emit small incrementing values instead of zero** — exact required
    precision beyond "small, non-zero, non-repeating" is still unconfirmed.
  - **The device's 8-byte Feature report (report ID 0) is not write-only.**
    A `GET_REPORT` (`bRequest=0x01`, same `wValue=0x0300`) returns a live,
    monotonically-increasing status/counter value (bytes 3-4 of the reply)
    that tracks every command sent, including through a full macro-content
    upload and commit — contradicting this doc's long-standing "the device
    has zero readback" assumption for *this* channel specifically (DPI/
    button/profile *table contents* still have no readback — this is a
    generic command-echo/counter, not a way to read back configuration).

### Two distinct macro recording modes — previously undocumented

The vendor software's Macro Manager has a **"The delay time between the
record keys" checkbox** in its Recording Options panel. Macros recorded
different ways produce **structurally different byte payloads**, not just
different timing values — a driver/parser that assumes every macro has the
same shape (with or without wait-blocks, and with meaningfully-varied vs.
near-minimal per-event timing bytes) will mishandle whichever kind it
wasn't built for:

- **No captured timing** (a plain 2-key press-'1'-press-'2' macro, "today"):
  the Macro Manager's own "Key sequence" display shows **no timing icons at
  all** between steps. Raw content bytes: `0001021e811e011f811f00...` —
  decodes to header `00 01`, then press '1' (timing=`2`), release '1'
  (timing=`1`), press '2' (timing=`1`), release '2' (timing=`1`), **no
  wait-blocks at all** even though real time obviously passed during
  recording. Timing values are small (`1`-`2`) but never exactly `0`.
- **Full captured timing** (a 3-key macro with real pauses, "delay2"): the
  Key sequence display shows explicit millisecond values for every step
  (`93 ms`, `2282 ms`, `78 ms`, `1953 ms`, `94 ms`). Raw bytes include real
  wait-blocks for the two long gaps (see calibrated formula below) plus
  non-trivial, non-repeating per-event timing bytes (`46`, `16`, `39`, `2`,
  `47`, `1`) for the short hold times — see the wait-block worked example
  immediately below, which is this exact macro.

**Caveat on the checkbox's exact causal role**: the checkbox was observed
unchecked when *viewing* either existing macro afterward (viewing a saved
macro doesn't seem to reflect what it was originally recorded with — it
likely only governs the *next new* recording), so the checkbox's precise
mechanism wasn't confirmed with full certainty in one unambiguous test. What
*is* confirmed: **two structurally different macro forms exist and must
both be handled** — one with no wait-blocks and near-minimal per-event
timing, one with real wait-blocks and naturally-varied timing. Driver
implication: prefer the "no captured timing" shape for synthesized macros
(no wait-blocks, small non-zero non-repeating timing bytes) — this is the
shape confirmed to actually play back correctly, whereas a synthesized
macro using literal `0` for every timing byte does not (see the
Feature-report readback note above).

### Inter-key wait blocks (long pauses)

A real, multi-second pause between keystrokes during recording is encoded
as a distinct **4-byte block** inserted before the next key's press/release
pair (not present before the very first key). Confirmed via two live tests:

```
[00 03] [00 <value>]   <- fixed marker (00 03), then a value byte
```

- The `[00 03]` marker is constant. **Correction**: an earlier version of
  this note claimed it "coincidentally matches the byte pattern of
  `type=0x0003`" — that was wrong. Per this doc's own stated convention
  (multi-byte fields are little-endian), `type=0x0003` is encoded on the
  wire as `03 00` (confirmed by e.g. button 6's `07 00 03 00` in the
  Restore Defaults dump — type `07 00`, value `03 00`). The wait-block
  marker `00 03` is the **reverse** byte order — same two hex digits, not
  the same field or a real collision. Unrelated, and not actually similar
  once byte order is accounted for.
- **Raw evidence (independently re-verifiable)**: an earlier version of this
  section presented the derived values below with no raw bytes to check
  them against; both source payloads are reproduced here in full instead.

  Source `macro_delay_calibration.pcapng` frame 12961 (counted-out
  "2, 3, 4, 5 second" waits before the 2nd/3rd/4th/5th keystrokes — digits
  '2'/'3'/'4'/'5' respectively), full 64-byte payload:
  ```
  0001371e861e0003001a271f8d1f000300202f208a200003002c2f21932100030030372281220000000000000000000000000000000000000000000000000000
  ```
  Source `macro_delay_calibration2.pcapng` frame 8101 (one precisely
  stopwatch-timed 9-second wait before the 2nd keystroke — digit '9',
  chosen as a mnemonic for the 9-second test, not literal key "2"), full
  64-byte payload:
  ```
  00011f1e801e0003007f272681260000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
  ```
  Decoded: payload 1 is `press/release '1'` (no wait before the first key,
  confirming that claim), then `WAIT(0x1a=26) → press/release '2'`,
  `WAIT(0x20=32) → '3'`, `WAIT(0x2c=44) → '4'`, `WAIT(0x30=48) → '5'`.
  Payload 2 is `press/release '1'`, then `WAIT(0x7f=127) → press/release
  '9'`.
- The value byte **maxes out at `0x7f` (127)** — confirmed by clamping: the
  precisely-timed 9-second wait (payload 2 above) produced exactly `0x7f`,
  the maximum value a 7-bit-range byte can hold (same clamping behavior
  seen with the Combo-Key Delay field elsewhere in this doc).
- The counted-out test (payload 1 above) produced values `26, 32, 44, 48`
  for nominal "2, 3, 4, 5 second" waits — increasing with wait length, but
  not cleanly linear against the stated seconds. Given the 9-second test's
  precise, clamped result, this is most likely explained by counting
  imprecision (a person counting seconds aloud is commonly ±10-20% off real
  time) rather than a non-linear encoding — but this reasoning, unlike the
  clamp finding itself, can't be independently re-verified from the bytes
  alone; it rests on trusting the stated real-world timing of each test.
- **Calibrated: `raw = round(delay_ms / 50)`** — confirmed exactly against
  the vendor software's own displayed timing values (its "Key sequence"
  view shows the real millisecond delay for each step), which is more
  reliable ground truth than the earlier stopwatch/counted-out attempts.
  Source: a 3-key macro ("delay2") with two real pauses, full 64-byte
  content payload:
  ```
  00012e1e901e0003002d271f821f000300272f208120000000000000000000000000000000000000000000000000000000000000000000000000000000000000
  ```
  Decodes as header `00 01`, press '1' (`2e`=46), release '1' (`90`&0x7f=16),
  **wait-block `00 03 00 2d`** (`0x2d`=45 → `45×50=2250ms`, UI showed
  `2282ms` — 1.4% off), press '2' (`27`=39), release '2' (`82`&0x7f=2),
  **wait-block `00 03 00 27`** (`0x27`=39 → `39×50=1950ms`, UI showed
  `1953ms` — essentially exact), press '3' (`2f`=47), release '3'
  (`81`&0x7f=1). Independently re-verified by direct computation, not
  transcribed from the original write-up. This retroactively confirms the
  `0x7f` (127) ceiling found earlier (`127×50=6350ms` max representable
  wait) and explains the older counted-out test's `26,32,44,48` values as
  counting imprecision (`26×50=1300ms` etc., all shorter than the counted
  seconds — a known, common effect of counting seconds aloud), not a
  different formula.
- **Driver implication**: a driver can now compute exact wait-block values
  for any requested pause up to 6350ms using `round(ms / 50)`, clamped to
  `0x7f` above that. **Implemented in both `c18ctl macro-create` (`wait:<ms>`
  token) and the libratbag driver** — hardware-confirmed. The per-event hold-duration field
  (separate from wait-blocks) is a different matter — see the correction
  above; it must be small and non-zero, not simply "safe to leave at 0."

- The assigning button's slot gets `type = 0x0009 | (repeat_mode<<8), value=<macro ID>` — confirmed via a direct test (existing macro ID `0x0004`) and via 3 isolated repeat-mode tests. **`c18ctl button <slot> macro <id> <mode>` implements this reference side** (type/value construction, for an ID you already know).
- **Content-upload mechanism — resolved** (previously the one open protocol
  gap in this doc): macro content is uploaded via opcode `0x0f <target_ID>
  80 ...` followed by one or more 64-byte writes on endpoint `0x04` — the ID
  in the `0x0f` command matches the macro ID written into the button slot
  exactly. This is the *same* mechanism used for Armoury fire-parameter
  uploads (see Armoury below); `0x0f` is a general "upload extended data for
  reference ID=X" signal, not macro-specific. See the full worked example
  below. **Implemented** — `c18ctl macro-create <slot> <0xHH id> <mode>
  <key1> [key2 ...]` sends the button-slot write, its ack, the `0x0f`
  trigger, and the multi-packet content upload in one transaction, choosing
  the ID as described in "Macro ID assignment" below. **Upload is
  hardware-confirmed working** (the transaction always completes and is
  acknowledged, see the Feature-report readback note above) but **playback
  is still being worked out**: a uniform non-zero `event_byte` timing value
  is not sufficient on its own (confirmed: reproduced the same failure as
  `0`), while small, non-repeating, incrementing values have shown correct
  playback in isolated testing (see the correction above). `c18ctl.c`'s
  current implementation has not yet been updated past a uniform value and
  needs the same fix applied and verified. Always targets profile 1 (not
  yet exposed as a CLI option). Verified only up to 2 content packets (48
  events) — see Open items for the 3+ packet caveat.

### Complete worked example (raw bytes, full transaction)

Source: assigning a "1, 2, 3" keyboard macro (ID `0x1f`) to a button. All 8
packets in order, exactly as captured:

```
1. 01 88 00 00 00 00 00 76        begin transaction

2. 0d 01 80 00 00 00 00 71        prepare button table write (profile 1)

3. [endpoint 0x04, 64 bytes] button table write — the assigned button's
   slot reads: 09 00 1f 00  ->  type=0x0009, value=0x001f (macro ID 31)
   Full payload:
   0100f000000000000300e900000006000300ea000300b70000002c0000000000070001000800030009001f000000070000000000000000000400010004000200

4. [endpoint 0x04, 64 bytes, all zero] commit/ack for the button-table write

5. 0f 1f 80 00 00 00 00 51        prepare extended-data upload for
                                    TARGET ID = 0x1f — matches the macro ID
                                    from step 3 exactly. This is how the
                                    device knows which macro slot the next
                                    payload belongs to: the ID is named
                                    explicitly in this control command, not
                                    embedded in the content packet itself.

6. [endpoint 0x04, 64 bytes] the actual macro content:
   0001461ed61e271fc61f3e2081200000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
   Decoded: header `00 01` (constant — not a length/event-count field),
   then 6 event pairs = press "1", release "1", press "2", release "2",
   press "3", release "3":
     (46,1e) press '1'   (d6,1e) release '1'
     (27,1f) press '2'   (c6,1f) release '2'
     (3e,20) press '3'   (81,20) release '3'
   Remaining 50 bytes are zero padding.

7. [endpoint 0x04, 64 bytes, all zero] commit/ack for the content write

8. 02 01 00 00 00 00 00 fc        commit / select profile 1 (closes the
                                    transaction)
```

Cross-verified against an independent capture assigning an *existing* macro
(ID `0x0004`), which produced the trigger `0f 04 80 00 00 00 00 6c` — target
ID matching the button-slot reference exactly, correctly checksummed
(`0f+04+80+0x6c = 0xFF`).

- **Association with macro ID**: not embedded in the content packet at all
  — established entirely by the preceding `0x0f <target_ID> 80 ...` control
  command.
- **Length/count prefix**: none identified. The `00 01` header is constant
  (verified identical across a 6-event macro and a 12-event macro) — not an
  event count. Events pack sequentially from byte 2 onward, zero-padded to
  fill the packet.
- **Events per packet**: a single packet holds ~31 events (62 bytes after
  the 2-byte header) — see "Multi-packet macros" below for longer macros.
- **Ordering**: button-table write (with the macro reference) happens
  first, then its own commit/ack, then the `0x0f`-triggered content upload,
  then its own commit/ack, then the final transaction commit — replicate
  this exact order.

### Multi-packet macros — resolved

Confirmed with a 24-character macro (`1234567890-=1234567890-=`, 48 events /
96 bytes of event data — too large for one 64-byte packet):

```
0f 04 80 00 00 00 00 6c        prepare extended-data upload, target ID=4
                                 (checksum verified: 0x0f+0x04+0x80+0x6c = 0xFF)

[endpoint 0x04, packet 1 of 2]:
0001021e811e011f811f0120812001218121012281220123812301248124012581250126812601278127012d812d012e812e011e811e011f811f012081200121

[endpoint 0x04, packet 2 of 2]:
8121012281220123812301248124012581250126812601278127012d812d012e812e000000000000000000000000000000000000000000000000000000000000
```

- **Only one `0x0f` trigger fires for the whole macro**, not one per packet
  — the device just expects however many consecutive 64-byte writes follow
  on endpoint `0x04`.
- **Packet 2 has no header** — it's a raw continuation of the event stream
  exactly where packet 1 left off (mid-event: packet 1 ends with the
  *press* half of the 16th character, packet 2 opens with that character's
  *release*). The `00 01` header only appears once, at the start of packet 1.
- Packet 1 is filled completely (31 events, all 62 content bytes used);
  packet 2 carries the remaining 17 events and is zero-padded after that —
  consistent with "keep writing consecutive packets until the content is
  exhausted, then zero-pad the last one" rather than any explicit
  length/continuation field.
- This single test doesn't establish the exact upper limit (e.g. whether a
  macro needing 3+ packets works the same way — see Open items), but it
  confirms the mechanism generalizes cleanly beyond one packet.

### Macro ID assignment — resolved: no device-side negotiation exists

**Macro creation is entirely host-side software.** Confirmed by isolating
the exact create → record → stop-recording → save sequence (a genuinely new
macro, keys "1" and "2") with **zero button assignment** and **zero
Apply/Save on the main window** — the resulting capture had **no USB traffic
whatsoever** beyond the generic device-descriptor read at time 0. The device
does not learn a macro exists, and has no concept of ID allocation, until
the moment it's assigned to a button (which is exactly when content +
reference always get uploaded together).

The IDs seen in earlier tests (`0x0004`, `0x0007` for two vendor-software-
created macros, non-consecutive — the gap matches Armoury presets created in
the same session, since Armoury references use the identical
`base_type | (mode<<8)` pattern) reflect the **vendor software's own
internal counter**, not anything the device tracks or validates.

**Combined with the resolved content-upload mechanism above**, a driver has
everything it needs: pick any ID not already in use by that profile's
buttons/Armoury presets (tracked in the driver's own state), then send the
button-slot reference write followed by the `0x0f`-triggered content
upload, exactly as shown in the worked example above. `c18ctl button <slot>
macro` only *references* an existing ID (useful for an already-known macro
from the vendor software); `c18ctl macro-create` does the full
create-and-assign flow, generating its own event data and choosing the ID
you pass it — see the Macros section above.

## Macro Playback (previously undocumented — merged from Windows capture box)

**Everything above this point describes uploading a macro's *definition* to
the device. Nothing in this document previously covered what happens when a
macro actually *plays back*** (i.e. you physically press the button it's
assigned to). This is a completely separate mechanism, confirmed via three
live tests on the vendor software/hardware: a no-delay "1,2,3" macro, the
same macro re-recorded *with* real timing, and a no-delay macro with cycle
count set to 3.

### A new endpoint: `0x82`

Macro playback happens entirely autonomously on the device — **no host
software involvement is needed** once the button is physically pressed. It
sends real, standard-format USB HID keyboard reports on **endpoint
`0x82`**, an interrupt-IN endpoint never seen anywhere else in this
document (everything else in this protocol uses `0x81`, `0x04`, or control
transfer `0x00`). This is presumably tied to interface `MI_01` (previously
described only as "system controller / consumer control") — not
independently confirmed via the USB descriptor, but the endpoint number is
consistent with that interface based on standard descriptor ordering.

### Report format

Each keystroke is a 16-byte interrupt-IN report:

```
[ReportID=0x01][Modifier=0x00][HID_keycode][13 bytes padding, zero]
```

Confirmed identical structure across all three tests — e.g. press "1":
`01 00 1e 00 00 00 00 00 00 00 00 00 00 00 00 00`. A **release** is the
same 16-byte shape with the keycode zeroed out: `01 00 00 00 ...` (i.e.
"no keys currently down", not a distinct "release" report type — matches
a standard USB HID boot-keyboard report exactly, which has no explicit
release event, just a change in which keys are listed as down).

### Markers (corrected)

**Correction**: an earlier version of this section called the second 4-byte
report an "end of playback" marker, based on a test where the whole macro
finished in ~41ms and the two markers looked adjacent to "start" and "end."
A follow-up test (3-key macro, real delays, 3 cycles) disproved that: the
second marker fired **early** — immediately after the very *first* key's
press+release, before 8 more keystrokes and two multi-second waits even
happened. Corrected model:

- **`06 09 01 <X>`** — sent once, right before the first keystroke of the
  entire playback (confirmed: fires once regardless of cycle count).
  `<X>` varies (`0x04` and `0x07` observed) — meaning still unconfirmed.
- **`06 00 00 00`** — sent once, **immediately after the first key's
  press+release specifically** — not after the whole macro or all cycles.
  True meaning still unclear (possibly "setup/handshake complete, entering
  normal playback" rather than "macro finished") — flagged as open, but
  "end of playback" is now disproven, not just uncertain.

Both use `ReportID=0x06`, distinct from the `0x01` keystroke reports.

### Real timing is reproduced during playback, not just displayed in the UI

Confirmed with a macro recorded with real ~2-second pauses (delay checkbox
checked): playback showed genuine multi-second gaps between keystrokes on
endpoint `0x82` — **~1485ms and ~2376ms** in the first (1-cycle) test,
closely matching the recorded pauses. This confirms the wait-block
mechanism isn't just a UI/storage artifact — the device's firmware
genuinely times its own autonomous playback using the encoded wait values.

### Cycle repeat behavior (fully confirmed, including wait-block replay)

Two cycle tests, confirming different things:

- **No-delay "1,2,3" macro, cycles=3**: the complete 3-key press/release
  sequence repeats 3 full times **back-to-back with no extra pause between
  cycles** — entire playback (all 3 cycles) completed in ~41ms.
- **3-key macro with real ~1.6s and ~2.4s waits, cycles=3**: both
  wait-block pauses **replay with high precision on every single cycle**,
  not just the first pass. Measured gaps across all 3 repetitions:

  | Gap | Cycle 1 | Cycle 2 | Cycle 3 |
  |---|---|---|---|
  | After key 1 (before key 2) | 1.577s | 1.577s | 1.578s |
  | After key 2 (before key 3) | 2.376s | 2.375s | 2.376s |

  Under 1.5ms variance across repetitions — the firmware reproduces the
  encoded wait values with real timer precision on every cycle, not an
  approximation that drifts or only applies once.

The start marker fires once at the very beginning regardless of cycle
count; the `06 00 00 00` marker (see above) fires once, early, also
regardless of cycle count — neither repeats per-cycle.

### Relevance to this repo's own driver debugging

This section describes device-autonomous behavior — there is nothing here
for `c18ctl`/the libratbag driver to *write*, since playback happens
entirely in firmware once a macro is uploaded and its button is pressed.
It's documented for diagnostic value: if a button assigned to a macro
doesn't behave as expected, capturing endpoint `0x82` during a physical
press (rather than re-inspecting the upload transaction, which was already
hardware-confirmed to complete and acknowledge cleanly via the Feature-report
readback — see the Macros section above) is the way to see what the device
actually did, independent of whatever the upload side looked like.

### Open items from this section

- The start marker's 4th byte (`0x04` vs `0x07` observed) — meaning
  unconfirmed.
- The true meaning of the `06 00 00 00` marker — confirmed to fire early
  (after key 1), not at the end, but its actual purpose is still unknown.
- Exact interface ownership of endpoint `0x82` not confirmed via USB
  descriptor inspection, only inferred.
- Mouse-click playback (if macros can contain clicks) was not observed in
  any playback test, since none of the test macros contained clicks.

## Armoury (burst-fire macro, "N Key") — fully decoded, not implemented in `c18ctl`

A separate, unrelated sub-protocol from the keyboard-macro system above:
burst-fire configurator (projectile count, per-projectile strike interval,
X/Y recoil correction, repeat mode, cycle count), managed as named presets
("guns"). "N Key" (not "Close the N Key", a different menu item that just
re-exposes the `type=0x0001` click submenu) opens a popup to assign either an
Armoury preset or a macro to a button. See `docs/vendor-software/armoury-tab.png`
for the vendor UI — the screenshot's own values (X=50, Y=77, interval=900ms)
match the single-projectile worked example below exactly — it's the actual
capture session's UI state.

- **Button-slot reference**: `type = 0x0c | (repeat_mode<<8)`, same 3-mode
  enum as macros. **No persistent preset index exists** — confirmed by
  assigning two different presets ("Armoury1" then "Armoury2") to the same
  button with the same repeat mode both times: the button-slot reference
  (`type=0x020c, value=0x0400`) came back byte-for-byte identical for both.
  The reference is just a fixed "this is an Armoury assignment, mode=X"
  marker; the actual preset identity lives entirely in the freshly
  re-uploaded fire-parameter data (below) — there's no ID because the full
  parameter set is always resent on assignment.
- **Fire-parameter packet**: triggered by opcode `0x0f` (`target_ID=0x04`)
  + 64-byte write. **Single-projectile**, confirmed via an isolated
  one-field-at-a-time test:
  ```
  byte[0]:   00 (constant in every test so far — role still unclear)
  byte[1]:   cycle count, raw byte value 1-255 (see "Projectile/cycle
             count" below — earlier passes mistook this for part of a
             fixed header; it just defaulted to 0x01 until varied)
  byte[2-3]: 02 f0 (fixed)
  byte[4]:   0x81 (no X/Y correction) or 0x82 (X/Y correction active)
  byte[5]:   f0 (constant)
  [if active] X offset:  LE 16-bit, unsigned (50 -> 0x0032)
  [if active] Y correction: LE 16-bit, SIGNED, sign-inverted from the UI
              value (entering 77 -> stored as -77, 0xffb3)
  strike interval: LE 16-bit = 0x0200 + (interval_ms / 20)
                   confirmed: 500ms -> 0x0219, 900ms -> 0x022d
  ```
  Variable-length, presence-based layout (fields only present when active),
  not fixed offsets like the DPI/button tables.
- **Multi-projectile**: uses a *different* layout, not a repeat of the
  single-projectile format — confirmed via a clean 2-projectile test with
  fully distinct per-projectile values:
  ```
  byte[0]:   00 (same role as single-projectile)
  byte[1]:   cycle count (see below) — 0x01 (default) in this test
  Then one 10-byte chunk per projectile with custom interval/X/Y, or a
  shorter 4-byte chunk per projectile left at default (see below):
    byte[0-3]: 02 f0 82 f0             <- fixed chunk header (customized)
    byte[4-5]: strike interval, LE 16-bit = 0xf901 + (interval_ms / 2)
               confirmed: 40ms -> 0xf915, 80ms -> 0xf929
    byte[6-7]: X offset, LE 16-bit, unsigned (11 -> 0x000b, 33 -> 0x0021)
    byte[8-9]: Y correction, LE 16-bit, signed/sign-inverted (22 -> 0xffea, 44 -> 0xffd4)
  ```
  Distinct interval formula from single-projectile mode (`0xf901 + ms/2` vs
  `0x0200 + ms/20`) — not an approximation of it.
- **Projectile count and cycle count — both resolved.** Earlier passes
  called `byte[0-1]` a "fixed header" (`00 01`) — it was never fixed, `01`
  was just the default cycle count until deliberately varied.
  - **Cycle count = `byte[1]`**, raw 1-255. Confirmed by an isolated test:
    a 1-projectile preset at cycles=1 gave `byte[1]=0x01`; changing *only*
    cycles to 255 gave `byte[1]=0xff` — the **only** byte that changed
    anywhere in the 64-byte packet:
    ```
    cycles=1:   000102f0b2f000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
    cycles=255: 00ff02f0b2f000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
    ```
  - **Projectile count is purely implicit — no explicit count field.**
    Confirmed by setting 8 projectiles (UI max) at default interval/X/Y,
    cycles held at 1:
    ```
    000102f0b2f002f0b2f002f0b2f002f0b2f002f0b2f002f0b2f002f0b2f002f0b2f0000000000000000000000000000000000000000000000000000000000000
    ```
    Decodes as header `00 01` (cycles=1) followed by **exactly 8**
    repetitions of a **4-byte "default projectile" chunk** (`02 f0 b2 f0`)
    — a shorter form than the 10-byte customized chunk above: when a
    projectile isn't customized, its chunk shrinks to just the 4-byte
    header/flag portion, no interval/X/Y fields at all. No byte anywhere
    encodes "8" — the device counts projectiles by how many chunks are in
    the packet, the same "no length field, infer from content" pattern
    already confirmed for macro event data.
  - **Driver implication**: to set N projectiles, emit N chunks (4-byte
    default form, or 10-byte form for a customized projectile) — no count
    field to compute. To set cycle count, write it directly into `byte[1]`.
- **Not implemented in `c18ctl`** — the protocol is now fully decoded, but
  the variable-length/presence-based packet format (different for 0/1/2+
  projectiles) is enough added complexity that it wasn't worth building a
  CLI command for without a concrete use case. Could be added later using
  the encoding above.

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
Both profiles agree on bytes 21/29 (`0x39`), so the *stage-2* DPI value is
the same factory default for both profiles — the differing bytes are
something else per-profile, still not decoded (they are *not* other DPI
stages' data — the per-stage offsets are now confirmed at 20/22/23/24 and
28/30/31/32, see the DPI section above, and those don't overlap bytes
7-9/40-63). These two tables (`PROFILE1_DPI_DEFAULT`/`PROFILE2_DPI_DEFAULT`
in `c18ctl.c`) are what `c18ctl dpi-set` seeds every write from.

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
1's own type/value pair, unchanged in earlier captures); buttons 9-11 were
simply never tested before; and the DPI per-stage offset table (which this
document previously flagged as an unverified claim contradicted by every raw
byte dump then available) is now backed by actual raw before/after packet
bytes, independently re-verified — see the DPI section above. All 11
buttons, the native-click value table, keyboard-shortcut modifier encoding,
Combo Key, Exact Control, and all 5 DPI stages are now implemented in
`c18ctl`. Remaining open items:

**Closed out this round**: bytes 7-9/40-63 of the DPI table packet (exact
semantic meaning still unknown, but confirmed deterministic/unaffected by
every testable UI control across dozens of captures — a fixed per-profile
constant, safe to cache-and-resend, exactly what `c18ctl dpi-set` already
does — see the DPI section above); the DPI-table primer packet's *content*
(fixed/identical in every capture — a driver just always sends this exact
packet, even though *why* the firmware needs it isn't knowable without
source access); the macro/Armoury ID *allocation pattern* (sequential,
shared pool between macros and Armoury presets, not driver-controllable);
and the **macro content-upload packet framing** — previously the one
remaining protocol gap in this doc, now fully resolved with a concrete
worked example (a 6-event macro) plus a multi-packet example (a
48-event/24-character macro spanning 2 packets) — see the Macros section
above. Opcode `0x0f` carries the content, keyed by the target ID named in
the command itself; the earlier "opcode `0x09`" theory was a misreading of
the generic `SET_REPORT` USB class request (see the note under Opcode
summary).

Genuinely still open:

- Whether a macro needing **3+ packets** (more than ~48 events) follows the
  same "keep writing consecutive packets, zero-pad the last one" pattern —
  only tested up to 2 packets (48 events). Low risk given the mechanism
  generalized cleanly from 1→2 packets, but not proven beyond that.
  `c18ctl macro-create` allows longer macros anyway but prints a warning
  when a macro would need 3+ packets, since that combination hasn't been
  run against real hardware.
- Bytes 28–31 and 48–63 of the **button-remap** packet: confirmed
  fixed/identical across both profiles' factory defaults and unchanged
  through the full 11-button sweep, so likely genuinely reserved — but not
  proven via a test that specifically targeted this region.
- `c18ctl button` and `c18ctl dpi-set` both write their whole table from a
  factory-default template every time (no readback of actual DPI/button
  *table contents* exists to preserve current state — see the note above
  about the Feature report's status/counter readback, which is a different,
  unrelated thing) — setting one button/stage resets every other
  button/stage on that profile back to factory default. The libratbag
  driver fixes this by tracking full table state client-side across calls
  within one session; `c18ctl` itself still doesn't.
- Exact analytic formula (if any) for the DPI byte encoding above ~4800 DPI
  (cosmetic only — the lookup table with interpolation already covers it).
- Whether "Sniper Mode" (hold-to-lower-DPI) or a "DPI Reset" function exists
  distinct from Up/Down/Cycle — not checked.
- Whether "Disabled" is genuinely just `type=0x0000, value=0x00`, or a
  distinct explicit disable flag exists elsewhere.
- **Macro Playback** (new section, merged from the Windows capture box —
  see "Macro Playback" above): the wire-level mechanics of autonomous
  device-side macro playback (endpoint `0x82`, marker reports, wait-block
  replay) are now documented, but three sub-items remain unconfirmed: the
  start marker's 4th byte (`0x04` vs `0x07`), the true meaning of the
  `06 00 00 00` marker (confirmed to fire early, not at the end, but
  purpose still unknown), and endpoint `0x82`'s exact interface ownership
  (inferred, not confirmed via USB descriptor).
- **Armoury**: fully resolved — repeat-mode encoding, confirmed no
  persistent preset index, single-/multi-projectile per-projectile fields
  (interval, X/Y correction, distinct interval formulas per mode), and both
  previously-open count fields: cycle count (`byte[1]` of the fire-parameter
  packet, raw 1-255) and projectile count (purely implicit — one 4-or-10-byte
  chunk per projectile, no explicit count field) — see the Armoury section
  for the isolated tests confirming each. Not implemented as a `c18ctl`
  command — the variable-length/presence-based packet format wasn't worth
  the added CLI surface without a concrete use case, not because anything's
  still unknown.

**Not yet implemented in `c18ctl` (protocol is understood — this is pure CLI
work, not blocked on RE)**:
- Armoury preset upload, including multi-projectile and cycle count
  (fire-parameter packet via `0x0f`) — see the Armoury section.
- `c18ctl macro-create` always targets profile 1 — a driver wanting
  profile-2 macros would need to expose that.

~~Inter-key wait blocks~~ — **implemented**. Both `c18ctl macro-create`
(`wait:<ms>` token in the key list) and the libratbag driver now emit real
wait-blocks using the calibrated `raw = round(ms / 50)` formula, instead of
skipping wait events. Hardware-confirmed end-to-end through both.

**DPI table X/Y independence — resolved, not possible on this hardware.**
The vendor software's own UI has no separate X/Y DPI control at all (a
single slider per sensitivity level, see `docs/vendor-software/
performance-tab.png`) — every capture this project has ever taken set the
X-DPI and Y-DPI bytes to the same value simply because the software never
offers a way to set them differently. Directly tested by bypassing the
vendor software: `c18ctl dpi-set-xy <stage> <dpi_x> <dpi_y>` writes
deliberately different values to the two byte slots (confirmed via extreme
values, X=10000/Y=200 on stage 1) and the mouse was moved to compare
horizontal vs. vertical cursor speed. **Both directions moved at the same
speed** — the device does not apply the two DPI byte slots independently,
despite the wire format having two distinct offsets for them. A driver
should treat DPI as a single per-stage value, not attempt to expose
separate X/Y controls.
