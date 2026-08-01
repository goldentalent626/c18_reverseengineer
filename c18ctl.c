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
    "\n"
    "Colors: red green blue yellow purple cyan white off\n"
    "\n"
    "DPI value table and button remapping are not implemented yet — see\n"
    "PROTOCOL.md for what's decoded and what's still missing.\n"
  );
}

enum cmd_kind { KIND_LED, KIND_PROFILE, KIND_POLLING, KIND_DPI_STAGE };

int main(int argc, char **argv) {
  if (argc < 2) { usage(); return 2; }

  unsigned char payload[64];
  enum cmd_kind kind;
  unsigned char profile = 1;
  unsigned char arg_byte = 0; // polling interval or dpi stage, depending on kind

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

  if (kind == KIND_LED) {
    // Primer 64 bytes: ff ff 00... (Windows sends this)
    unsigned char primer[64] = {0};
    primer[0] = 0xff;
    primer[1] = 0xff;

    const unsigned char ep_out = 0x04;
    int xfer = 0;

    send_cmd(h, OP_PREPARE_DPI_TABLE, profile, 0x80, "prepare table");

    rc = out64(h, ep_out, primer, &xfer);
    if (rc || xfer != 64) fprintf(stderr, "primer out failed: rc=%d xfer=%d\n", rc, xfer);
    usleep(20000);

    rc = out64(h, ep_out, payload, &xfer);
    if (rc || xfer != 64) fprintf(stderr, "payload out failed: rc=%d xfer=%d\n", rc, xfer);
    usleep(40000);
  } else if (kind == KIND_POLLING) {
    send_cmd(h, OP_SET_POLLING, profile, arg_byte, "set polling rate");
  } else if (kind == KIND_DPI_STAGE) {
    send_cmd(h, OP_SET_DPI_STAGE, profile, arg_byte, "set dpi stage");
  }
  // KIND_PROFILE has no extra command — OP_SELECT_COMMIT below both selects
  // the profile and commits, per PROTOCOL.md's transaction model.

  send_cmd(h, OP_SELECT_COMMIT, profile, 0x00, "commit");

  for (int i = 0; i < 3; i++) {
    libusb_release_interface(h, ifaces[i]);
  }

  libusb_close(h);
  libusb_exit(ctx);
  return 0;
}
