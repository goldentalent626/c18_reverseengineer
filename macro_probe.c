// macro_probe.c - diagnostic tool built to investigate why a button mapped
// to a macro reference (type=0x0009) doesn't behave as PROTOCOL.md's
// worked example implies it should. Replicates c18ctl's macro-create
// transaction for button 7 / macro ID 0x20 / keys 1,2,3, but:
//   - inserts a GET_REPORT poll after every single step, which revealed
//     the device DOES have a working status/readback channel on the
//     8-byte Feature report (bytes 3-4 of the reply are a live,
//     monotonically-increasing counter) - contrary to PROTOCOL.md's
//     long-standing "zero readback exists" assumption. Every step of a
//     full macro-content upload + commit acknowledges cleanly.
//   - takes repeat_mode (0x00/0x01/0x02) as an optional first CLI arg, to
//     test each of the three documented modes independently.
//
// Findings so far (see PROTOCOL_MEMORY / project notes for full detail):
// the write itself always completes and is acknowledged correctly for all
// 3 modes, but on an actual button press: mode 0x00 ("once") silently does
// nothing, while 0x01 ("until interrupted") and 0x02 ("until released")
// both hang the device. Recovery in all cases: run
// `c18ctl button 7 key w` - no physical unplug/replug needed as of the
// kernel-driver-reattach fix in c18ctl.c.
//
// Build:
//   gcc macro_probe.c -o macro_probe $(pkg-config --cflags --libs libusb-1.0)
// Run:
//   sudo ./macro_probe [repeat_mode]   # repeat_mode defaults to 0x00

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

static const int VENDOR_IFACE = 2;
static const unsigned char ep_out = 0x04;

static unsigned char checksum8(const unsigned char b[7]) {
  unsigned int sum = 0;
  for (int i = 0; i < 7; i++) sum += b[i];
  return (unsigned char)(0xFF - (sum & 0xFF));
}

static void build_cmd(unsigned char out[8], unsigned char opcode, unsigned char b1, unsigned char b2) {
  out[0] = opcode; out[1] = b1; out[2] = b2;
  out[3] = out[4] = out[5] = out[6] = 0x00;
  out[7] = checksum8(out);
}

static int set_feature(libusb_device_handle *h, const unsigned char feat[8]) {
  return libusb_control_transfer(h, 0x21, 0x09, 0x0300, (uint16_t)VENDOR_IFACE,
                                  (unsigned char*)feat, 8, 1000);
}

static void poll_status(libusb_device_handle *h, const char *label) {
  unsigned char buf[8] = {0};
  int rc = libusb_control_transfer(h, 0xa1, 0x01, 0x0300, (uint16_t)VENDOR_IFACE,
                                    buf, 8, 1000);
  printf("[GET_REPORT after %-28s] rc=%d bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
         label, rc, buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7]);
}

static int out64(libusb_device_handle *h, const unsigned char buf[64]) {
  int transferred = 0;
  int rc = libusb_interrupt_transfer(h, ep_out, (unsigned char*)buf, 64, &transferred, 1000);
  if (rc || transferred != 64) fprintf(stderr, "  ! out64 failed: rc=%d xfer=%d\n", rc, transferred);
  return rc;
}

static void send_cmd(libusb_device_handle *h, unsigned char opcode, unsigned char b1,
                      unsigned char b2, const char *label) {
  unsigned char cmd[8];
  build_cmd(cmd, opcode, b1, b2);
  int rc = set_feature(h, cmd);
  if (rc < 0) fprintf(stderr, "  ! %s failed: %d\n", label, rc);
  usleep(20000);
  poll_status(h, label);
}

// Same default button table as c18ctl.c, slot7 patched to macro ref (ID 0x20).
static unsigned char BUTTON_TABLE[64] = {
  0x01,0x00,0xf0,0x00,0x01,0x00,0xf1,0x00,0x01,0x00,0xf2,0x00,0x01,0x00,0xf4,0x00,
  0x01,0x00,0xf3,0x00,0x07,0x00,0x03,0x00,0x00,0x00,0x2c,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x1a,0x00,0x00,0x00,0x16,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x07,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x00,0x01,0x00,0x04,0x00,0x02,0x00
};

int main(int argc, char **argv) {
  unsigned char mode = 0x00;
  unsigned char macro_id = 0x20;
  if (argc >= 2) mode = (unsigned char)strtol(argv[1], NULL, 0);
  if (argc >= 3) macro_id = (unsigned char)strtol(argv[2], NULL, 0);
  printf("Using repeat_mode=0x%02x macro_id=0x%02x\n", mode, macro_id);

  libusb_context *ctx = NULL;
  libusb_device_handle *h = NULL;
  int rc = libusb_init(&ctx);
  if (rc) { fprintf(stderr, "libusb_init failed: %d\n", rc); return 1; }

  h = libusb_open_device_with_vid_pid(ctx, 0x12c9, 0x2003);
  if (!h) { fprintf(stderr, "Device 12c9:2003 not found\n"); libusb_exit(ctx); return 1; }

  int ifaces[] = {0, 1, 2};
  for (int i = 0; i < 3; i++) {
    if (libusb_kernel_driver_active(h, ifaces[i]) == 1)
      libusb_detach_kernel_driver(h, ifaces[i]);
    rc = libusb_claim_interface(h, ifaces[i]);
    if (rc) { fprintf(stderr, "claim interface %d failed: %d\n", ifaces[i], rc); return 1; }
  }

  // patch slot 7 (offset 32) to macro ref: type=0x0009|(mode<<8), value=macro_id
  BUTTON_TABLE[32] = 0x09; BUTTON_TABLE[33] = mode;
  BUTTON_TABLE[34] = macro_id; BUTTON_TABLE[35] = 0x00;

  poll_status(h, "(nothing yet, baseline)");

  send_cmd(h, 0x01, 0x88, 0x00, "BEGIN");

  send_cmd(h, 0x0d, 0x01, 0x80, "prepare button table");
  out64(h, BUTTON_TABLE);
  usleep(20000);
  poll_status(h, "button table payload out64");

  unsigned char zero[64] = {0};
  out64(h, zero);
  usleep(20000);
  poll_status(h, "button table ack out64");

  send_cmd(h, 0x0f, macro_id, 0x80, "prepare macro content");

  // Testing Ctrl+C as a real chord: press ctrl, press c, release c,
  // release ctrl - NOT the per-key press/release-before-next-key pattern
  // c18ctl macro-create is limited to. HID usage 0xe0 = Left Ctrl, 0x06 = 'c'.
  unsigned char content[64] = {0};
  content[0] = 0x00; content[1] = 0x01;
  unsigned char ev_keys[4]    = {0xe0, 0x06, 0x06, 0xe0};
  unsigned char ev_release[4] = {0,    0,    1,    1   }; // 0=press 1=release
  int off = 2;
  unsigned char counter = 0x01;
  for (int i = 0; i < 4; i++) {
    content[off++] = ev_release[i] ? (0x80 | counter) : counter;
    counter = (counter >= 0x7f) ? 1 : counter + 1;
    content[off++] = ev_keys[i];
  }
  out64(h, content);
  usleep(20000);
  poll_status(h, "macro content payload out64");

  out64(h, zero);
  usleep(40000);
  poll_status(h, "macro content ack out64 (40ms)");

  send_cmd(h, 0x02, 0x01, 0x00, "COMMIT");

  printf("\nDone. If nothing froze, physically test button 7 now.\n");

  for (int i = 0; i < 3; i++) {
    libusb_release_interface(h, ifaces[i]);
    libusb_attach_kernel_driver(h, ifaces[i]);
  }
  libusb_close(h);
  libusb_exit(ctx);
  return 0;
}
