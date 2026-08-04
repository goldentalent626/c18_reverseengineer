// restore_defaults.c - replicates PROTOCOL.md's captured "Restore Defaults"
// transaction exactly (see "Restore Defaults" section), for both profiles.
// Written to test whether a full documented factory-reset sequence clears
// whatever device-side state has been causing macro playback to fail
// during this session's testing (multi-packet framing, delay length,
// macro ID freshness, and even a physical unplug/replug were all ruled
// out individually - this is the next concrete thing to try before
// concluding anything is unresolvable).
//
// Build:
//   gcc restore_defaults.c -o restore_defaults $(pkg-config --cflags --libs libusb-1.0)
// Run:
//   sudo ./restore_defaults

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

static void send_cmd(libusb_device_handle *h, unsigned char opcode, unsigned char b1,
                      unsigned char b2, const char *label) {
  unsigned char cmd[8];
  build_cmd(cmd, opcode, b1, b2);
  int rc = set_feature(h, cmd);
  if (rc < 0) fprintf(stderr, "  ! %s failed: %d\n", label, rc);
  usleep(20000);
}

static int out64(libusb_device_handle *h, const unsigned char buf[64]) {
  int transferred = 0;
  int rc = libusb_interrupt_transfer(h, ep_out, (unsigned char*)buf, 64, &transferred, 1000);
  if (rc || transferred != 64) fprintf(stderr, "  ! out64 failed: rc=%d xfer=%d\n", rc, transferred);
  usleep(20000);
  return rc;
}

// Factory-default button table, byte-identical across both profiles
// (PROTOCOL.md "Default button table").
static const unsigned char BUTTON_DEFAULT_TABLE[64] = {
  0x01,0x00,0xf0,0x00,0x01,0x00,0xf1,0x00,0x01,0x00,0xf2,0x00,0x01,0x00,0xf4,0x00,
  0x01,0x00,0xf3,0x00,0x07,0x00,0x03,0x00,0x00,0x00,0x2c,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x1a,0x00,0x00,0x00,0x16,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x07,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x00,0x01,0x00,0x04,0x00,0x02,0x00
};

// Per-profile factory-default DPI tables (PROTOCOL.md "Default DPI table").
static const unsigned char DPI_DEFAULT[2][64] = {
  { // Profile 1
    0x0f,0x00,0x1e,0x0a,0x19,0x19,0x05,0x04,0x06,0x10,0x64,0x64,0x01,0xc0,0xf0,0x03,
    0x01,0x01,0x19,0x19,0x22,0x39,0x5c,0xd0,0xf3,0x5c,0x73,0x00,0x22,0x39,0x5c,0xd0,
    0xf3,0x5c,0x73,0x00,0x1f,0xff,0x00,0x00,0x31,0x00,0x00,0x00,0x31,0x00,0x00,0x00,
    0x31,0x31,0x31,0x00,0x31,0x00,0x31,0x00,0x31,0x31,0x31,0x31,0x31,0x31,0x31,0x31
  },
  { // Profile 2
    0x0f,0x00,0x1e,0x0a,0x19,0x19,0x05,0x01,0x01,0x03,0x64,0x64,0x01,0xc0,0xf0,0x03,
    0x01,0x01,0x19,0x19,0x22,0x39,0x5c,0xd0,0xf3,0x5c,0x73,0x00,0x22,0x39,0x5c,0xd0,
    0xf3,0x5c,0x73,0x00,0x1f,0xff,0x00,0x00,0x00,0x00,0x31,0x00,0x00,0x31,0x00,0x00,
    0x31,0x00,0x00,0x31,0x00,0x00,0x31,0x00,0x00,0x31,0x00,0x00,0x31,0x00,0x00,0x31
  },
};

static void restore_profile(libusb_device_handle *h, unsigned char profile) {
  unsigned char idx = profile - 1;
  printf("Restoring profile %d...\n", profile);

  send_cmd(h, 0x01, 0x88, 0x00, "begin");
  send_cmd(h, 0x02, profile, 0x00, "select profile");
  send_cmd(h, 0x03, profile, 0x02, "set polling rate 500Hz");
  send_cmd(h, 0x04, profile, 0x01, "set active DPI stage 1");

  send_cmd(h, 0x0c, profile, 0x80, "prepare DPI table write");
  unsigned char primer[64] = {0};
  primer[0] = 0xff; primer[1] = 0xff;
  out64(h, primer);
  out64(h, DPI_DEFAULT[idx]);

  send_cmd(h, 0x0d, profile, 0x80, "prepare button table write");
  out64(h, BUTTON_DEFAULT_TABLE);

  send_cmd(h, 0x02, profile, 0x00, "commit");

  send_cmd(h, 0x01, 0x88, 0x00, "begin (second)");
  send_cmd(h, 0x02, profile, 0x00, "commit (second)");
}

int main(void) {
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

  restore_profile(h, 1);
  restore_profile(h, 2);

  printf("Done - both profiles restored to factory defaults.\n");

  for (int i = 0; i < 3; i++) {
    libusb_release_interface(h, ifaces[i]);
    libusb_attach_kernel_driver(h, ifaces[i]);
  }
  libusb_close(h);
  libusb_exit(ctx);
  return 0;
}
