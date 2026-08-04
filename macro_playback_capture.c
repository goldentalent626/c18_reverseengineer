// macro_playback_capture.c - captures endpoint 0x82 traffic (autonomous
// macro playback, see PROTOCOL.md's "Macro Playback" section) during a
// physical button press, instead of inferring what happened from the
// upload transaction alone (which is known to complete/ack cleanly
// regardless of whether playback itself works).
//
// Built to diagnose why a 20-key/2-packet macro assigned via
// `c18ctl macro-create` only typed "123" instead of the full string -
// this shows exactly what HID reports the device actually emitted on
// endpoint 0x82, so we can tell whether it silently stopped sending
// keystrokes, sent garbage, or sent everything correctly but something
// downstream (focus, OS-level key handling) ate the rest.
//
// Build:
//   gcc macro_playback_capture.c -o macro_playback_capture $(pkg-config --cflags --libs libusb-1.0)
// Run (only claims interface 1, unlike the other libusb tools here, so it
// can safely run alongside a live ratbagd - press the
// macro-assigned button any time within the capture window):
//   sudo ./macro_playback_capture [seconds, default 20]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <libusb-1.0/libusb.h>

static const unsigned char ep_in_playback = 0x82;

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
  int seconds = (argc >= 2) ? atoi(argv[1]) : 20;
  if (seconds <= 0) seconds = 20;

  libusb_context *ctx = NULL;
  libusb_device_handle *h = NULL;
  int rc = libusb_init(&ctx);
  if (rc) { fprintf(stderr, "libusb_init failed: %d\n", rc); return 1; }

  h = libusb_open_device_with_vid_pid(ctx, 0x12c9, 0x2003);
  if (!h) { fprintf(stderr, "Device 12c9:2003 not found\n"); libusb_exit(ctx); return 1; }

  // Only interface 1 (MI_01) is claimed - that's where endpoint 0x82 lives
  // (see PROTOCOL.md's Macro Playback section). Earlier versions of this
  // tool claimed all 3 interfaces, including interface 2 (the vendor config
  // channel, MI_02) - which ratbagd holds open via hidraw whenever it's
  // running. Detaching that interface out from under a live ratbagd caused
  // exactly the "hidraw open failed, retrying"/device-recreation cycles
  // seen in its log during driver testing, corrupting whatever test was in
  // progress. Claiming only interface 1 lets this run alongside ratbagd
  // without disrupting it.
  const int iface = 1;
  int detached = 0;
  if (libusb_kernel_driver_active(h, iface) == 1) {
    if (libusb_detach_kernel_driver(h, iface) == 0) detached = 1;
  }
  rc = libusb_claim_interface(h, iface);
  if (rc) { fprintf(stderr, "claim interface %d failed: %d\n", iface, rc); return 1; }

  printf("Capturing endpoint 0x82 for %d seconds - press the macro button now...\n", seconds);
  double t0 = now_ms();
  double deadline = t0 + seconds * 1000.0;
  int n_reports = 0;

  while (now_ms() < deadline) {
    unsigned char buf[16] = {0};
    int transferred = 0;
    rc = libusb_interrupt_transfer(h, ep_in_playback, buf, sizeof(buf), &transferred, 200);
    if (rc == LIBUSB_ERROR_TIMEOUT) continue;
    if (rc) {
      fprintf(stderr, "interrupt_transfer error: %d (%s)\n", rc, libusb_error_name(rc));
      continue;
    }
    double t = now_ms() - t0;
    n_reports++;
    printf("[%8.2fms] len=%d raw=", t, transferred);
    for (int i = 0; i < transferred; i++) printf("%02x ", buf[i]);
    if (transferred == 16 && buf[0] == 0x01) {
      printf(" -> keystroke: keycode=0x%02x (%s)", buf[2], buf[2] ? "press" : "release/none");
    } else if (transferred >= 4 && buf[0] == 0x06) {
      printf(" -> marker: %02x %02x %02x %02x", buf[0], buf[1], buf[2], buf[3]);
    }
    printf("\n");
  }

  printf("Done. %d report(s) captured.\n", n_reports);

  libusb_release_interface(h, iface);
  if (detached) libusb_attach_kernel_driver(h, iface);
  libusb_close(h);
  libusb_exit(ctx);
  return 0;
}
