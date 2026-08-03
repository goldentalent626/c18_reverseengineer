// c18ctl.c - SOAI C18 (12c9:2003) control via libusb (LED, polling rate, DPI stage, profile)
// Build:
//   gcc c18ctl.c -o c18ctl $(pkg-config --cflags --libs libusb-1.0)
// Usage:
//   sudo ./c18ctl off
//   sudo ./c18ctl static red [high|mid|low]
//   sudo ./c18ctl neon fast|mid|slow
//   sudo ./c18ctl flowing fast|mid|slow
//   sudo ./c18ctl breathing fast|mid|slow
//   sudo ./c18ctl wave fast|mid|slow
//   sudo ./c18ctl profile <1|2>
//   sudo ./c18ctl polling <125|250|500|1000> [profile]
//   sudo ./c18ctl dpi-stage <1-5> [profile]
//
// See PROTOCOL.md for the full reverse-engineered protocol this is built from.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

static void die(const char *msg) {
  fprintf(stderr, "%s\n", msg);
  exit(1);
}

static int streq(const char *a, const char *b) {
  return a && b && strcmp(a, b) == 0;
}

static int starts_with(const unsigned char *buf, size_t n, const unsigned char *pat, size_t pn) {
  return n >= pn && memcmp(buf, pat, pn) == 0;
}

// Your proven working STATIC template (0f... 64 bytes)
static const unsigned char BASE_PAYLOAD[64] = {
  // 0f001e0a191905010103646401c0f0030101191922395cd0f35c730022395cd0f35c73001fff...
  0x0f,0x00,0x1e,0x0a,0x19,0x19,0x05,0x01,0x01,0x03,0x64,0x64,0x01,0xc0,0xf0,0x03,
  0x01,0x01,0x19,0x19,0x22,0x39,0x5c,0xd0,0xf3,0x5c,0x73,0x00,0x22,0x39,0x5c,0xd0,
  0xf3,0x5c,0x73,0x00,0x1f,0xff,0x00,0x00,
  // Tail (bytes 40..63) will be patched for static colors/intensity.
  0x00,0x00,0x31, 0x00,0x00,0x31, 0x00,0x00,0x31, 0x00,0x00,0x31,
  0x00,0x00,0x31, 0x00,0x00,0x31, 0x00,0x00,0x31, 0x00,0x00,0x31
};

// Tail observed identically across every non-static-color capture (off, breathing,
// flowing, wave, neon at all 3 speeds) — a fixed default palette (red/green/blue/
// yellow/purple/cyan/white/white), not something that varies with mode or speed.
static const unsigned char EFFECT_TAIL_DEFAULT[24] = {
  0x31,0x00,0x00, 0x00,0x31,0x00, 0x00,0x00,0x31, 0x31,0x31,0x00,
  0x31,0x00,0x31, 0x00,0x31,0x31, 0x31,0x31,0x31, 0x31,0x31,0x31
};

// Per-profile factory-default DPI table, script-verified byte-for-byte from a
// real capture (Restore Defaults). Used as the base for every DPI-table write:
// since the device has no readback, a driver must seed from a known-good full
// table and patch only the 2 target bytes for whichever stage is being changed
// (see PROTOCOL.md's "Driver implication"). Bytes 7-9 and 40-63 are still
// undecoded and differ per profile, so they're preserved as-is from here rather
// than reconstructed.
static const unsigned char PROFILE1_DPI_DEFAULT[64] = {
  0x0f,0x00,0x1e,0x0a,0x19,0x19,0x05,0x04,0x06,0x10,0x64,0x64,0x01,0xc0,0xf0,0x03,
  0x01,0x01,0x19,0x19,0x22,0x39,0x5c,0xd0,0xf3,0x5c,0x73,0x00,0x22,0x39,0x5c,0xd0,
  0xf3,0x5c,0x73,0x00,0x1f,0xff,0x00,0x00,0x31,0x00,0x00,0x00,0x31,0x00,0x00,0x00,
  0x31,0x31,0x31,0x00,0x31,0x00,0x31,0x00,0x31,0x31,0x31,0x31,0x31,0x31,0x31,0x31
};
static const unsigned char PROFILE2_DPI_DEFAULT[64] = {
  0x0f,0x00,0x1e,0x0a,0x19,0x19,0x05,0x01,0x01,0x03,0x64,0x64,0x01,0xc0,0xf0,0x03,
  0x01,0x01,0x19,0x19,0x22,0x39,0x5c,0xd0,0xf3,0x5c,0x73,0x00,0x22,0x39,0x5c,0xd0,
  0xf3,0x5c,0x73,0x00,0x1f,0xff,0x00,0x00,0x00,0x00,0x31,0x00,0x00,0x31,0x00,0x00,
  0x31,0x00,0x00,0x31,0x00,0x00,0x31,0x00,0x00,0x31,0x00,0x00,0x31,0x00,0x00,0x31
};

// Byte offsets of each DPI stage's X/Y code within the DPI table packet.
// Confirmed by editing stages 1/3/4/5 individually and diffing each write
// against the previous state: exactly one X/Y byte pair changes per edit, at
// consecutive offsets, 8 bytes apart (X block starts at 20, Y block at 28).
static const struct { int x_off, y_off; } DPI_STAGE_OFFSETS[6] = {
  /* [0] unused */ {0, 0},
  /* stage 1 */    {20, 28},
  /* stage 2 */    {21, 29},
  /* stage 3 */    {22, 30},
  /* stage 4 */    {23, 31},
  /* stage 5 */    {24, 32},
};

// DPI value -> device byte code. Non-linear sensor register encoding; use linear
// interpolation between confirmed points (matches doc guidance and exactly
// reproduces every confirmed table entry, including the below-400 DPI/50 region).
static const struct { int dpi; unsigned char byte; } DPI_CALIB[] = {
  {200,0x04},{300,0x06},{400,0x08},{600,0x0d},{800,0x12},{1200,0x1b},{1600,0x24},
  {2000,0x2e},{2300,0x34},{2400,0x37},{2600,0x3b},{3200,0x49},{4800,0x6f},
  {6400,0xc9},{6600,0xcc},{10000,0xf3},
};
#define DPI_CALIB_N (int)(sizeof(DPI_CALIB) / sizeof(DPI_CALIB[0]))

static unsigned char dpi_to_byte(int dpi) {
  if (dpi < DPI_CALIB[0].dpi || dpi > DPI_CALIB[DPI_CALIB_N - 1].dpi) {
    die("DPI must be in range 200-10000");
  }
  for (int i = 0; i < DPI_CALIB_N - 1; i++) {
    int d0 = DPI_CALIB[i].dpi, d1 = DPI_CALIB[i + 1].dpi;
    if (dpi >= d0 && dpi <= d1) {
      if (d0 == d1) return DPI_CALIB[i].byte;
      int b0 = DPI_CALIB[i].byte, b1 = DPI_CALIB[i + 1].byte;
      return (unsigned char)(b0 + (dpi - d0) * (b1 - b0) / (d1 - d0));
    }
  }
  return DPI_CALIB[DPI_CALIB_N - 1].byte;
}

// Default button table, script-verified byte-for-byte from a real capture. All
// 11 buttons confirmed against the vendor software's default-layout diagram.
// Bytes 28-31 and 48-63 are a fixed/reserved trailer (confirmed unchanged across
// every capture, including a full 11-button sweep). Used as the base for every
// button-table write, since the device protocol has no readback: writing one
// button resets any other previously-customized button back to this default.
static const unsigned char BUTTON_DEFAULT_TABLE[64] = {
  0x01,0x00,0xf0,0x00,0x01,0x00,0xf1,0x00,0x01,0x00,0xf2,0x00,0x01,0x00,0xf4,0x00,
  0x01,0x00,0xf3,0x00,0x07,0x00,0x03,0x00,0x00,0x00,0x2c,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x1a,0x00,0x00,0x00,0x16,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x07,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x00,0x01,0x00,0x04,0x00,0x02,0x00
};

// Byte offset of a button slot's [type LE][value LE] pair within the button table.
// All 11 physical buttons use the identical 4-byte [type][value] format, no
// exceptions (an earlier pass mistook Button 1's own pair for a "marker+tag
// prefix" on Button 2, since Button 1 never changed in those captures). Button
// 11 sits out of numeric sequence, between 6 and 7.
static int button_slot_offset(int slot) {
  switch (slot) {
    case  1: return  0;
    case  2: return  4;
    case  3: return  8;
    case  4: return 12;
    case  5: return 16;
    case  6: return 20;
    case 11: return 24;
    case  7: return 32;
    case  8: return 36;
    case  9: return 40;
    case 10: return 44;
    default: die("Button slot must be 1-11"); return -1;
  }
}

static void set_button_type_value(unsigned char payload[64], int slot, unsigned short type, unsigned short value) {
  int off = button_slot_offset(slot);
  payload[off + 0] = (unsigned char)(type & 0xff);
  payload[off + 1] = (unsigned char)(type >> 8);
  payload[off + 2] = (unsigned char)(value & 0xff);
  payload[off + 3] = (unsigned char)(value >> 8);
}

// All 8-byte control commands share one wrapper: SET_REPORT(Feature), and every
// command's 8 bytes sum to exactly 0xFF, i.e. checksum = 0xFF - sum(byte[0..6]).
// Confirmed against every opcode captured, including across both DPI profiles.
static unsigned char checksum8(const unsigned char b[7]) {
  unsigned int sum = 0;
  for (int i = 0; i < 7; i++) sum += b[i];
  return (unsigned char)(0xFF - (sum & 0xFF));
}

static void build_cmd(unsigned char out[8], unsigned char opcode, unsigned char b1, unsigned char b2) {
  out[0] = opcode;
  out[1] = b1;
  out[2] = b2;
  out[3] = out[4] = out[5] = out[6] = 0x00;
  out[7] = checksum8(out);
}

// Opcodes (see PROTOCOL.md)
#define OP_BEGIN                0x01 // begin transaction: build_cmd(_, 0x01, 0x88, 0x00)
#define OP_SELECT_COMMIT        0x02 // select active profile / commit: build_cmd(_, 0x02, profile, 0x00)
#define OP_SET_POLLING          0x03 // build_cmd(_, 0x03, profile, interval_ms)
#define OP_SET_DPI_STAGE        0x04 // build_cmd(_, 0x04, profile, stage)
#define OP_PREPARE_DPI_TABLE    0x0c // build_cmd(_, 0x0c, profile, 0x80)
#define OP_PREPARE_BUTTON_TABLE 0x0d // build_cmd(_, 0x0d, profile, 0x80)
#define OP_PREPARE_EXT_DATA     0x0f // build_cmd(_, 0x0f, target_id, 0x80) — macro/Armoury content upload trigger

static int set_feature(libusb_device_handle *h, int iface, const unsigned char feat[8]) {
  return libusb_control_transfer(
    h,
    0x21,      // Host->Dev | Class | Interface
    0x09,      // SET_REPORT
    0x0300,    // Feature (3)<<8 | ReportID 0
    (uint16_t)iface,
    (unsigned char*)feat,
    8,
    1000
  );
}

static int out64(libusb_device_handle *h, unsigned char ep_out, const unsigned char buf[64], int *xfer) {
  int transferred = 0;
  int rc = libusb_interrupt_transfer(h, ep_out, (unsigned char*)buf, 64, &transferred, 1000);
  if (xfer) *xfer = transferred;
  return rc;
}

static const int VENDOR_IFACE = 2;

static void send_cmd(libusb_device_handle *h, unsigned char opcode, unsigned char b1, unsigned char b2, const char *label) {
  unsigned char cmd[8];
  build_cmd(cmd, opcode, b1, b2);
  int rc = set_feature(h, VENDOR_IFACE, cmd);
  if (rc < 0) fprintf(stderr, "%s failed: %d\n", label, rc);
  usleep(20000);
}

static unsigned char parse_profile(const char *s) {
  if (!s) return 1;
  if (streq(s, "1")) return 1;
  if (streq(s, "2")) return 2;
  die("Profile must be: 1|2");
  return 0;
}

static unsigned char parse_polling(const char *s) {
  if (streq(s, "1000")) return 0x01;
  if (streq(s, "500"))  return 0x02;
  if (streq(s, "250"))  return 0x04;
  if (streq(s, "125"))  return 0x08;
  die("Polling rate must be: 125|250|500|1000");
  return 0;
}

static unsigned char parse_dpi_stage(const char *s) {
  int v = atoi(s);
  if (v < 1 || v > 5) die("DPI stage must be 1-5");
  return (unsigned char)v;
}

static int find_mode_block(unsigned char payload[64]) {
  // Find 19 19 05 <mode> <speed> <param>
  const unsigned char pat[] = {0x19,0x19,0x05};
  for (int i = 0; i <= 64 - (int)sizeof(pat) - 3; i++) {
    if (memcmp(payload + i, pat, sizeof(pat)) == 0) {
      // ensure there are 3 bytes after 1905
      return i; // points to first 0x19 of the block
    }
  }
  return -1;
}

static void set_tail_rgb8(unsigned char payload[64], unsigned char r, unsigned char g, unsigned char b) {
  // bytes 40..63 = 8 RGB triplets
  for (int i = 0; i < 8; i++) {
    int off = 40 + i * 3;
    payload[off + 0] = r;
    payload[off + 1] = g;
    payload[off + 2] = b;
  }
}

static void parse_color_mask(const char *color, int *mr, int *mg, int *mb) {
  *mr = *mg = *mb = 0;
  if (streq(color, "red"))    { *mr=1; }
  else if (streq(color, "green"))  { *mg=1; }
  else if (streq(color, "blue"))   { *mb=1; }
  else if (streq(color, "yellow")) { *mr=1; *mg=1; }
  else if (streq(color, "purple")) { *mr=1; *mb=1; }
  else if (streq(color, "cyan"))   { *mg=1; *mb=1; }
  else if (streq(color, "white"))  { *mr=1; *mg=1; *mb=1; }
  else if (streq(color, "off"))    { /* all 0 */ }
  else die("Unknown color. Use: red|green|blue|yellow|purple|cyan|white|off");
}

static void parse_level(const char *lvl, unsigned char *tier, unsigned char *onval) {
  // Based on your captures:
  // High: tier=01, ON=0x31
  // Mid:  tier=02, ON=0x20
  // Low:  tier=03, ON=0x10
  if (!lvl || streq(lvl, "high")) { *tier = 0x01; *onval = 0x31; return; }
  if (streq(lvl, "mid") || streq(lvl, "middle")) { *tier = 0x02; *onval = 0x20; return; }
  if (streq(lvl, "low")) { *tier = 0x03; *onval = 0x10; return; }
  die("Unknown level. Use: high|mid|low");
}

static unsigned char speed_neon(const char *s) {
  if (streq(s, "fast")) return 0x04;
  if (streq(s, "mid") || streq(s, "middle")) return 0x08;
  if (streq(s, "slow")) return 0x0c;
  die("Speed must be: fast|mid|slow");
  return 0;
}
static unsigned char speed_breathing(const char *s) { return speed_neon(s); } // same 04/08/0c

static unsigned char speed_wave(const char *s) {
  if (streq(s, "fast")) return 0x03;
  if (streq(s, "mid") || streq(s, "middle")) return 0x06;
  if (streq(s, "slow")) return 0x09;
  die("Speed must be: fast|mid|slow");
  return 0;
}

static unsigned char speed_flowing(const char *s) {
  if (streq(s, "fast")) return 0x3c;
  if (streq(s, "mid") || streq(s, "middle")) return 0x5f;
  if (streq(s, "slow")) return 0x78;
  die("Speed must be: fast|mid|slow");
  return 0;
}

static void build_payload_off(unsigned char payload[64]) {
  memcpy(payload, BASE_PAYLOAD, 64);
  int idx = find_mode_block(payload);
  if (idx < 0) die("Could not locate mode block in base payload");
  payload[idx+3] = 0x00; // mode
  payload[idx+4] = 0x00; // speed/tier
  payload[idx+5] = 0x00; // param
  // Tail doesn't matter but set to 0 to be explicit
  set_tail_rgb8(payload, 0x00, 0x00, 0x00);
}

static void build_payload_static(unsigned char payload[64], const char *color, const char *lvl) {
  memcpy(payload, BASE_PAYLOAD, 64);

  int idx = find_mode_block(payload);
  if (idx < 0) die("Could not locate mode block in base payload");

  unsigned char tier = 0, onv = 0;
  parse_level(lvl, &tier, &onv);

  // Static mode block: 05 01 <tier> 03
  payload[idx+3] = 0x01;   // mode=static/standard
  payload[idx+4] = tier;   // brightness tier (01/02/03)
  payload[idx+5] = 0x03;   // constant for static

  int mr, mg, mb;
  parse_color_mask(color, &mr, &mg, &mb);

  unsigned char r = mr ? onv : 0x00;
  unsigned char g = mg ? onv : 0x00;
  unsigned char b = mb ? onv : 0x00;

  set_tail_rgb8(payload, r, g, b);
}

static void build_payload_mode_speed(unsigned char payload[64], unsigned char mode, unsigned char speed, unsigned char param) {
  memcpy(payload, BASE_PAYLOAD, 64);
  int idx = find_mode_block(payload);
  if (idx < 0) die("Could not locate mode block in base payload");

  payload[idx+3] = mode;
  payload[idx+4] = speed;
  payload[idx+5] = param;

  memcpy(payload + 40, EFFECT_TAIL_DEFAULT, 24);
}

static void build_payload_dpi_stage(unsigned char payload[64], int profile, int stage, int dpi) {
  if (stage < 1 || stage > 5) die("DPI stage must be 1-5");
  memcpy(payload, profile == 2 ? PROFILE2_DPI_DEFAULT : PROFILE1_DPI_DEFAULT, 64);
  unsigned char b = dpi_to_byte(dpi);
  payload[DPI_STAGE_OFFSETS[stage].x_off] = b;
  payload[DPI_STAGE_OFFSETS[stage].y_off] = b;
}

// USB HID Keyboard/Keypad usage page (standard, not device-specific) — matches
// every example byte given in PROTOCOL.md's button type/value table.
static unsigned short parse_key(const char *s) {
  if (strlen(s) == 1) {
    char c = s[0];
    if (c >= 'a' && c <= 'z') return (unsigned short)(0x04 + (c - 'a'));
    if (c >= 'A' && c <= 'Z') return (unsigned short)(0x04 + (c - 'A'));
    if (c >= '1' && c <= '9') return (unsigned short)(0x1e + (c - '1'));
    if (c == '0') return 0x27;
  }
  if ((s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) {
    return (unsigned short)strtol(s, NULL, 16);
  }
  die("key must be a single a-z/0-9 character or a 0xHH HID usage code");
  return 0;
}

static unsigned short parse_media(const char *s) {
  if (streq(s, "volup"))     return 0xe9;
  if (streq(s, "voldown"))   return 0xea;
  if (streq(s, "playpause")) return 0xcd;
  if (streq(s, "stop"))      return 0xb7;
  if (streq(s, "mute"))      return 0xe2;
  if (streq(s, "next"))      return 0xb5;
  if (streq(s, "prev"))      return 0xb6;
  if (streq(s, "mediaplayer")) return 0x0183;
  if (streq(s, "mycomputer"))  return 0x0194;
  if (streq(s, "home"))        return 0x0223;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return (unsigned short)strtol(s, NULL, 16);
  die("media must be one of: volup voldown playpause stop mute next prev "
      "mediaplayer mycomputer home, or 0xHH");
  return 0;
}

// type=0x0001 native mouse-click function. All 5 values confirmed against the
// vendor software's default-button-layout diagram (see PROTOCOL.md).
static unsigned short parse_click(const char *s) {
  if (streq(s, "left"))   return 0xf0;
  if (streq(s, "menu"))   return 0xf1;
  if (streq(s, "middle")) return 0xf2;
  if (streq(s, "back"))   return 0xf3;
  if (streq(s, "forward"))return 0xf4;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return (unsigned short)strtol(s, NULL, 16);
  die("click must be one of: left menu middle back forward, or 0xHH");
  return 0;
}

// USB HID keyboard modifier bitmask, shifted into type's high byte
// (type = modifier_bitmask << 8). Any combination of the letters c/s/a/w,
// or "-"/"none" for no modifier.
static unsigned char parse_mods(const char *s) {
  if (streq(s, "-") || streq(s, "none")) return 0x00;
  unsigned char mask = 0;
  for (const char *p = s; *p; p++) {
    switch (*p) {
      case 'c': case 'C': mask |= 0x01; break; // Ctrl
      case 's': case 'S': mask |= 0x02; break; // Shift
      case 'a': case 'A': mask |= 0x04; break; // Alt
      case 'w': case 'W': mask |= 0x08; break; // Win/GUI
      default: die("mods must be made of c/s/a/w (Ctrl/Shift/Alt/Win), or -/none");
    }
  }
  return mask;
}

static unsigned char parse_macro_mode(const char *s) {
  if (streq(s, "once"))               return 0x00;
  if (streq(s, "until-interrupted"))  return 0x01;
  if (streq(s, "until-released"))     return 0x02;
  die("macro mode must be: once|until-interrupted|until-released");
  return 0;
}

// Only 1-2 packet macros (up to 48 events) have been verified against real
// hardware (see PROTOCOL.md's "Multi-packet macros" section). This cap is
// generous well beyond that to leave room, not a confirmed device limit.
#define MAX_MACRO_KEYS 96

// Builds a macro's full keyboard-event byte stream: `00 01` header followed
// by press-then-release `[event_byte][keycode]` pairs for each key (see
// PROTOCOL.md's Macros section). event_byte's low 7 bits are an uncalibrated
// timing/delay value that real captures show varying with actual typing
// rhythm — meaning/unit unknown, so we always emit 0 here.
static int build_macro_events(unsigned char *buf, int buf_cap, const unsigned short *keys, int n_keys) {
  int len = 2 + n_keys * 4;
  if (len > buf_cap) die("macro too long");
  memset(buf, 0, buf_cap);
  buf[0] = 0x00;
  buf[1] = 0x01;
  int off = 2;
  for (int i = 0; i < n_keys; i++) {
    buf[off++] = 0x00;                        // press, no delay
    buf[off++] = (unsigned char)keys[i];
    buf[off++] = 0x80;                        // release, no delay
    buf[off++] = (unsigned char)keys[i];
  }
  return len;
}

// Uploads macro content as one or more 64-byte writes on endpoint 0x04,
// zero-padding the final packet, followed by a single all-zero 64-byte ack
// packet — replicates PROTOCOL.md's worked example exactly. Only the first
// packet carries the `00 01` header; later packets are a raw continuation
// of the event stream with no header of their own (confirmed in the
// "Multi-packet macros" section).
static void send_macro_content(libusb_device_handle *h, unsigned char ep_out, const unsigned char *buf, int len) {
  int num_packets = (len + 63) / 64;
  if (num_packets > 2) {
    fprintf(stderr,
      "warning: this macro needs %d packets; only 1-2 packet macros have "
      "been verified against real hardware (see PROTOCOL.md open items)\n",
      num_packets);
  }
  for (int i = 0; i < num_packets; i++) {
    unsigned char pkt[64] = {0};
    int chunk = len - i * 64;
    if (chunk > 64) chunk = 64;
    if (chunk > 0) memcpy(pkt, buf + i * 64, chunk);
    int xfer = 0;
    int rc = out64(h, ep_out, pkt, &xfer);
    if (rc || xfer != 64) fprintf(stderr, "macro content packet %d/%d failed: rc=%d xfer=%d\n", i + 1, num_packets, rc, xfer);
    usleep(20000);
  }
  unsigned char zero[64] = {0};
  int xfer = 0;
  int rc = out64(h, ep_out, zero, &xfer);
  if (rc || xfer != 64) fprintf(stderr, "macro content ack failed: rc=%d xfer=%d\n", rc, xfer);
  usleep(40000);
}

static void usage(void) {
  fprintf(stderr,
    "Usage:\n"
    "  sudo ./c18ctl off\n"
    "  sudo ./c18ctl static <color> [high|mid|low]\n"
    "  sudo ./c18ctl neon <fast|mid|slow>\n"
    "  sudo ./c18ctl flowing <fast|mid|slow>\n"
    "  sudo ./c18ctl breathing <fast|mid|slow>\n"
    "  sudo ./c18ctl wave <fast|mid|slow>\n"
    "  sudo ./c18ctl profile <1|2>\n"
    "  sudo ./c18ctl polling <125|250|500|1000> [profile]\n"
    "  sudo ./c18ctl dpi-stage <1-5> [profile]\n"
    "  sudo ./c18ctl dpi-set <stage 1-5> <dpi 200-10000> [profile]\n"
    "  sudo ./c18ctl button <slot 1-11> off [profile]\n"
    "  sudo ./c18ctl button <slot 1-11> key <a-z0-9|0xHH> [profile]\n"
    "  sudo ./c18ctl button <slot 1-11> media <volup|voldown|playpause|stop|mute|next|prev|mediaplayer|mycomputer|home|0xHH> [profile]\n"
    "  sudo ./c18ctl button <slot 1-11> click <left|menu|middle|back|forward|0xHH> [profile]\n"
    "  sudo ./c18ctl button <slot 1-11> dpi-up|dpi-down|dpi-cycle [profile]\n"
    "  sudo ./c18ctl button <slot 1-11> profile-up|profile-down|profile-cycle [profile]\n"
    "  sudo ./c18ctl button <slot 1-11> shortcut <mods> <key> [profile]\n"
    "  sudo ./c18ctl button <slot 1-11> shortcut2 <mods> <key1> <key2> [profile]\n"
    "  sudo ./c18ctl button <slot 1-11> combo <times> <delay_ms> [profile]\n"
    "  sudo ./c18ctl button <slot 1-11> exact-control [profile]\n"
    "  sudo ./c18ctl button <slot 1-11> macro <0xHH id> <once|until-interrupted|until-released> [profile]\n"
    "  sudo ./c18ctl macro-create <slot 1-11> <0xHH id> <once|until-interrupted|until-released> <key1> [key2 ...]\n"
    "\n"
    "Colors: red green blue yellow purple cyan white off\n"
    "mods: any combination of c/s/a/w (Ctrl/Shift/Alt/Win), or -/none.\n"
    "shortcut2's key2 is sent plain (no modifier) right after key1.\n"
    "combo is Double-click/Three-click/Fire-button's shared mechanism; delay_ms\n"
    "is clamped to 127 (device's confirmed max, a signed byte).\n"
    "macro only *references* an existing macro ID created via the vendor\n"
    "software; macro-create uploads new keyboard-only macro content (each key\n"
    "listed is pressed then released, in order) AND assigns it to <slot>, in\n"
    "one transaction, choose any <0xHH id> not already used by another button/\n"
    "Armoury preset on this profile (the device does no allocation itself, see\n"
    "PROTOCOL.md). Keys use the same a-z/0-9/0xHH syntax as `button key`.\n"
    "macro-create always targets profile 1 (not yet exposed as an option).\n"
    "\n"
    "dpi-set writes the whole DPI table, seeded from the profile's factory\n"
    "default (bytes 7-9/40-63, still undecoded, are preserved as-is from\n"
    "there) with only the target stage's 2 bytes patched.\n"
    "Each `button` call writes the WHOLE button table, resetting any other\n"
    "previously-set button back to factory default (no readback exists).\n"
    "See PROTOCOL.md for details and open items.\n"
  );
}

enum cmd_kind { KIND_LED, KIND_PROFILE, KIND_POLLING, KIND_DPI_STAGE, KIND_DPI_TABLE, KIND_BUTTON, KIND_MACRO_CREATE };

int main(int argc, char **argv) {
  if (argc < 2) { usage(); return 2; }

  unsigned char payload[64];
  enum cmd_kind kind;
  unsigned char profile = 1;
  unsigned char arg_byte = 0; // polling interval or dpi stage, depending on kind
  unsigned char macro_id = 0;
  unsigned short macro_keys[MAX_MACRO_KEYS];
  int n_macro_keys = 0;

  const char *cmd = argv[1];

  if (streq(cmd, "off")) {
    kind = KIND_LED;
    build_payload_off(payload);
  } else if (streq(cmd, "static")) {
    if (argc < 3) { usage(); return 2; }
    kind = KIND_LED;
    const char *color = argv[2];
    const char *lvl = (argc >= 4) ? argv[3] : "high";
    build_payload_static(payload, color, lvl);
  } else if (streq(cmd, "neon")) {
    if (argc < 3) { usage(); return 2; }
    kind = KIND_LED;
    build_payload_mode_speed(payload, 0x03, speed_neon(argv[2]), 0x00);
  } else if (streq(cmd, "flowing")) {
    if (argc < 3) { usage(); return 2; }
    kind = KIND_LED;
    build_payload_mode_speed(payload, 0x06, speed_flowing(argv[2]), 0x0a);
  } else if (streq(cmd, "breathing")) {
    if (argc < 3) { usage(); return 2; }
    kind = KIND_LED;
    build_payload_mode_speed(payload, 0x02, speed_breathing(argv[2]), 0x0a);
  } else if (streq(cmd, "wave")) {
    if (argc < 3) { usage(); return 2; }
    kind = KIND_LED;
    build_payload_mode_speed(payload, 0x04, speed_wave(argv[2]), 0x10);
  } else if (streq(cmd, "profile")) {
    if (argc < 3) { usage(); return 2; }
    kind = KIND_PROFILE;
    profile = parse_profile(argv[2]);
  } else if (streq(cmd, "polling")) {
    if (argc < 3) { usage(); return 2; }
    kind = KIND_POLLING;
    arg_byte = parse_polling(argv[2]);
    profile = parse_profile(argc >= 4 ? argv[3] : NULL);
  } else if (streq(cmd, "dpi-stage")) {
    if (argc < 3) { usage(); return 2; }
    kind = KIND_DPI_STAGE;
    arg_byte = parse_dpi_stage(argv[2]);
    profile = parse_profile(argc >= 4 ? argv[3] : NULL);
  } else if (streq(cmd, "dpi-set")) {
    if (argc < 4) { usage(); return 2; }
    kind = KIND_DPI_TABLE;
    profile = parse_profile(argc >= 5 ? argv[4] : NULL);
    build_payload_dpi_stage(payload, profile, atoi(argv[2]), atoi(argv[3]));
  } else if (streq(cmd, "button")) {
    if (argc < 4) { usage(); return 2; }
    int slot = atoi(argv[2]);
    const char *action = argv[3];
    unsigned short btype = 0, bvalue = 0;
    const char *profile_arg = NULL;

    if (streq(action, "off")) {
      btype = 0x0000; bvalue = 0x0000;
      profile_arg = (argc >= 5) ? argv[4] : NULL;
    } else if (streq(action, "key")) {
      if (argc < 5) { usage(); return 2; }
      btype = 0x0000; bvalue = parse_key(argv[4]);
      profile_arg = (argc >= 6) ? argv[5] : NULL;
    } else if (streq(action, "media")) {
      if (argc < 5) { usage(); return 2; }
      btype = 0x0003; bvalue = parse_media(argv[4]);
      profile_arg = (argc >= 6) ? argv[5] : NULL;
    } else if (streq(action, "dpi-up")) {
      btype = 0x0007; bvalue = 0x0001; profile_arg = (argc >= 5) ? argv[4] : NULL;
    } else if (streq(action, "dpi-down")) {
      btype = 0x0007; bvalue = 0x0002; profile_arg = (argc >= 5) ? argv[4] : NULL;
    } else if (streq(action, "dpi-cycle")) {
      btype = 0x0007; bvalue = 0x0003; profile_arg = (argc >= 5) ? argv[4] : NULL;
    } else if (streq(action, "profile-up")) {
      btype = 0x0008; bvalue = 0x0001; profile_arg = (argc >= 5) ? argv[4] : NULL;
    } else if (streq(action, "profile-down")) {
      btype = 0x0008; bvalue = 0x0002; profile_arg = (argc >= 5) ? argv[4] : NULL;
    } else if (streq(action, "profile-cycle")) {
      btype = 0x0008; bvalue = 0x0003; profile_arg = (argc >= 5) ? argv[4] : NULL;
    } else if (streq(action, "click")) {
      if (argc < 5) { usage(); return 2; }
      btype = 0x0001; bvalue = parse_click(argv[4]);
      profile_arg = (argc >= 6) ? argv[5] : NULL;
    } else if (streq(action, "shortcut")) {
      if (argc < 6) { usage(); return 2; }
      unsigned char mods = parse_mods(argv[4]);
      btype = (unsigned short)(mods << 8);
      bvalue = parse_key(argv[5]);
      profile_arg = (argc >= 7) ? argv[6] : NULL;
    } else if (streq(action, "shortcut2")) {
      if (argc < 7) { usage(); return 2; }
      unsigned char mods = parse_mods(argv[4]);
      btype = (unsigned short)(mods << 8);
      unsigned short key1 = parse_key(argv[5]);
      unsigned short key2 = parse_key(argv[6]);
      bvalue = (unsigned short)(key1 | (key2 << 8));
      profile_arg = (argc >= 8) ? argv[7] : NULL;
    } else if (streq(action, "combo")) {
      if (argc < 6) { usage(); return 2; }
      int times = atoi(argv[4]);
      int delay = atoi(argv[5]);
      if (times < 0 || times > 255) die("combo times must be 0-255");
      if (delay < 0) die("combo delay_ms must be >= 0");
      if (delay > 127) {
        fprintf(stderr, "note: delay clamped to 127ms (device's confirmed max, a signed byte)\n");
        delay = 127;
      }
      btype = 0xf00a;
      bvalue = (unsigned short)((delay & 0xff) | ((times & 0xff) << 8));
      profile_arg = (argc >= 7) ? argv[6] : NULL;
    } else if (streq(action, "exact-control")) {
      btype = 0x000b; bvalue = 0x0f0f;
      profile_arg = (argc >= 5) ? argv[4] : NULL;
    } else if (streq(action, "macro")) {
      if (argc < 6) { usage(); return 2; }
      unsigned char mode = parse_macro_mode(argv[5]);
      btype = (unsigned short)(0x0009 | (mode << 8));
      bvalue = (unsigned short)strtol(argv[4], NULL, 16);
      profile_arg = (argc >= 7) ? argv[6] : NULL;
    } else {
      usage();
      return 2;
    }

    kind = KIND_BUTTON;
    profile = parse_profile(profile_arg);
    memcpy(payload, BUTTON_DEFAULT_TABLE, 64);
    set_button_type_value(payload, slot, btype, bvalue);
  } else if (streq(cmd, "macro-create")) {
    if (argc < 6) { usage(); return 2; }
    kind = KIND_MACRO_CREATE;
    int slot = atoi(argv[2]);
    macro_id = (unsigned char)strtol(argv[3], NULL, 16);
    unsigned char mode = parse_macro_mode(argv[4]);

    for (int i = 5; i < argc; i++) {
      if (n_macro_keys >= MAX_MACRO_KEYS) die("macro too long (see MAX_MACRO_KEYS / PROTOCOL.md open items)");
      macro_keys[n_macro_keys++] = parse_key(argv[i]);
    }
    if (n_macro_keys == 0) die("macro-create needs at least one key");

    profile = 1; // macro-create always targets profile 1 (see usage)
    unsigned short btype = (unsigned short)(0x0009 | (mode << 8));
    unsigned short bvalue = macro_id;
    memcpy(payload, BUTTON_DEFAULT_TABLE, 64);
    set_button_type_value(payload, slot, btype, bvalue);
  } else {
    usage();
    return 2;
  }

  // --- USB send sequence (single session) ---
  libusb_context *ctx = NULL;
  libusb_device_handle *h = NULL;

  int rc = libusb_init(&ctx);
  if (rc) { fprintf(stderr, "libusb_init failed: %d\n", rc); return 1; }

  h = libusb_open_device_with_vid_pid(ctx, 0x12c9, 0x2003);
  if (!h) { fprintf(stderr, "Device 12c9:2003 not found\n"); libusb_exit(ctx); return 1; }

  // Detach/claim all 3 interfaces to prevent hid-generic from interfering.
  int ifaces[] = {0, 1, 2};
  for (int i = 0; i < 3; i++) {
    int iface = ifaces[i];
    if (libusb_kernel_driver_active(h, iface) == 1) {
      libusb_detach_kernel_driver(h, iface); // ignore errors
    }
    rc = libusb_claim_interface(h, iface);
    if (rc) {
      fprintf(stderr, "claim interface %d failed: %d\n", iface, rc);
      libusb_close(h);
      libusb_exit(ctx);
      return 1;
    }
  }

  send_cmd(h, OP_BEGIN, 0x88, 0x00, "begin");

  const unsigned char ep_out = 0x04;
  int xfer = 0;

  if (kind == KIND_LED || kind == KIND_DPI_TABLE) {
    // Both LED and DPI-table writes go through the same "prepare table" opcode
    // (0x0c) and 64-byte primer — see PROTOCOL.md's note on the shared header.
    unsigned char primer[64] = {0};
    primer[0] = 0xff;
    primer[1] = 0xff;

    send_cmd(h, OP_PREPARE_DPI_TABLE, profile, 0x80, "prepare table");

    rc = out64(h, ep_out, primer, &xfer);
    if (rc || xfer != 64) fprintf(stderr, "primer out failed: rc=%d xfer=%d\n", rc, xfer);
    usleep(20000);

    rc = out64(h, ep_out, payload, &xfer);
    if (rc || xfer != 64) fprintf(stderr, "payload out failed: rc=%d xfer=%d\n", rc, xfer);
    usleep(40000);
  } else if (kind == KIND_BUTTON) {
    // No primer packet for the button table (per PROTOCOL.md).
    send_cmd(h, OP_PREPARE_BUTTON_TABLE, profile, 0x80, "prepare button table");

    rc = out64(h, ep_out, payload, &xfer);
    if (rc || xfer != 64) fprintf(stderr, "payload out failed: rc=%d xfer=%d\n", rc, xfer);
    usleep(40000);
  } else if (kind == KIND_POLLING) {
    send_cmd(h, OP_SET_POLLING, profile, arg_byte, "set polling rate");
  } else if (kind == KIND_DPI_STAGE) {
    send_cmd(h, OP_SET_DPI_STAGE, profile, arg_byte, "set dpi stage");
  } else if (kind == KIND_MACRO_CREATE) {
    // Replicates PROTOCOL.md's macro worked example step-for-step: button-
    // slot reference write + its own all-zero ack, then the 0x0f-triggered
    // content upload (one or more packets) + its own ack, then the shared
    // OP_SELECT_COMMIT below closes the transaction. Unlike the plain
    // KIND_BUTTON path above, this explicit all-zero ack packet after the
    // button-table write is required to match the captured sequence exactly
    // — see PROTOCOL.md.
    send_cmd(h, OP_PREPARE_BUTTON_TABLE, profile, 0x80, "prepare button table");

    rc = out64(h, ep_out, payload, &xfer);
    if (rc || xfer != 64) fprintf(stderr, "button payload out failed: rc=%d xfer=%d\n", rc, xfer);
    usleep(20000);

    {
      unsigned char zero[64] = {0};
      rc = out64(h, ep_out, zero, &xfer);
      if (rc || xfer != 64) fprintf(stderr, "button ack failed: rc=%d xfer=%d\n", rc, xfer);
      usleep(20000);
    }

    unsigned char macro_buf[2 + MAX_MACRO_KEYS * 4];
    int macro_len = build_macro_events(macro_buf, sizeof(macro_buf), macro_keys, n_macro_keys);

    send_cmd(h, OP_PREPARE_EXT_DATA, macro_id, 0x80, "prepare macro content upload");
    send_macro_content(h, ep_out, macro_buf, macro_len);
  }
  // KIND_PROFILE has no extra command — OP_SELECT_COMMIT below both selects
  // the profile and commits, per PROTOCOL.md's transaction model.

  send_cmd(h, OP_SELECT_COMMIT, profile, 0x00, "commit");

  for (int i = 0; i < 3; i++) {
    libusb_release_interface(h, ifaces[i]);
    // Restore the kernel's own HID driver (hid-generic) so the mouse keeps
    // working normally in the OS after this program exits. Without this,
    // every invocation left the device's interfaces detached indefinitely -
    // cursor/clicks stopped working until a physical unplug/replug forced
    // re-enumeration. The device itself was never actually wedged; this was
    // purely a missing cleanup step here.
    libusb_attach_kernel_driver(h, ifaces[i]);
  }

  libusb_close(h);
  libusb_exit(ctx);
  return 0;
}
