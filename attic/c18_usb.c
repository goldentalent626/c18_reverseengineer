#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <libusb-1.0/libusb.h>

static int hexval(int c){
  if(c>='0'&&c<='9') return c-'0';
  if(c>='a'&&c<='f') return 10+c-'a';
  if(c>='A'&&c<='F') return 10+c-'A';
  return -1;
}
static int hex2bytes(const char *hex, unsigned char *out, int max){
  int n=0;
  while(*hex){
    while(*hex && isspace((unsigned char)*hex)) hex++;
    if(!*hex) break;
    int hi=hexval(*hex++); if(hi<0) return -1;
    int lo=hexval(*hex++); if(lo<0) return -1;
    if(n>=max) return -2;
    out[n++] = (unsigned char)((hi<<4)|lo);
  }
  return n;
}

int main(int argc, char **argv){
  if(argc != 3){
    fprintf(stderr, "Usage: %s <feature8_hex> <out64_hex>\n", argv[0]);
    return 2;
  }

  unsigned char feat[8], out[64];
  int flen = hex2bytes(argv[1], feat, sizeof(feat));
  int olen = hex2bytes(argv[2], out, sizeof(out));
  if(flen != 8){ fprintf(stderr, "feature must be 8 bytes\n"); return 2; }
  if(olen != 64){ fprintf(stderr, "out must be 64 bytes\n"); return 2; }

  libusb_context *ctx = NULL;
  libusb_device_handle *h = NULL;
  int rc = libusb_init(&ctx);
  if(rc){ fprintf(stderr, "libusb_init: %d\n", rc); return 1; }

  h = libusb_open_device_with_vid_pid(ctx, 0x12c9, 0x2003);
  if(!h){ fprintf(stderr, "Device 12c9:2003 not found\n"); libusb_exit(ctx); return 1; }

  int iface = 2;                 // vendor interface
  unsigned char ep_out = 0x04;   // interrupt OUT endpoint

  if(libusb_kernel_driver_active(h, iface) == 1){
    rc = libusb_detach_kernel_driver(h, iface);
    if(rc){ fprintf(stderr, "detach kernel driver: %d\n", rc); }
  }

  rc = libusb_claim_interface(h, iface);
  if(rc){ fprintf(stderr, "claim interface %d: %d\n", iface, rc); libusb_close(h); libusb_exit(ctx); return 1; }

  // USB HID SET_REPORT (Feature): bmRequestType=0x21, bRequest=0x09, wValue=0x0300 (Feature, ReportID 0), wIndex=2, data=8
  rc = libusb_control_transfer(h, 0x21, 0x09, 0x0300, 0x0002, feat, 8, 1000);
  if(rc < 0){ fprintf(stderr, "control SET_REPORT failed: %d\n", rc); }
  else { printf("SET_REPORT(feature) sent (%d bytes)\n", rc); }

  int transferred = 0;
  rc = libusb_interrupt_transfer(h, ep_out, out, 64, &transferred, 1000);
  if(rc){ fprintf(stderr, "interrupt OUT failed: %d\n", rc); }
  else { printf("interrupt OUT sent (%d bytes)\n", transferred); }

  libusb_release_interface(h, iface);
  libusb_close(h);
  libusb_exit(ctx);
  return 0;
}
