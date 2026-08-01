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
static int set_feature(libusb_device_handle *h, int iface, const unsigned char feat[8]){
  return libusb_control_transfer(h, 0x21, 0x09, 0x0300, (uint16_t)iface,
                                 (unsigned char*)feat, 8, 1000);
}
static int out64(libusb_device_handle *h, unsigned char ep_out, const unsigned char buf[64]){
  int transferred=0;
  int rc = libusb_interrupt_transfer(h, ep_out, (unsigned char*)buf, 64, &transferred, 1000);
  if(rc==0 && transferred==64) return 0;
  return (rc!=0)? rc : -999;
}

int main(int argc, char **argv){
  if(argc != 2){
    fprintf(stderr, "Usage: %s <out64_hex>\n", argv[0]);
    return 2;
  }

  unsigned char out_blob[64];
  if(hex2bytes(argv[1], out_blob, 64) != 64){
    fprintf(stderr, "out must be 64 bytes\n");
    return 2;
  }

  // Windows-captured feature reports
  const unsigned char FEAT_UNLOCK[8] = {0x01,0x88,0x00,0x00,0x00,0x00,0x00,0x76};
  const unsigned char FEAT_ARM[8]    = {0x0c,0x01,0x80,0x00,0x00,0x00,0x00,0x72};
  const unsigned char FEAT_FINAL[8]  = {0x02,0x01,0x00,0x00,0x00,0x00,0x00,0xfc};

  // Windows "primer" OUT packet (ffff0000... padded)
  unsigned char primer[64] = {0};
  primer[0]=0xff; primer[1]=0xff; // rest already 0x00

  libusb_context *ctx=NULL;
  libusb_device_handle *h=NULL;
  int rc = libusb_init(&ctx);
  if(rc){ fprintf(stderr, "libusb_init: %d\n", rc); return 1; }

  h = libusb_open_device_with_vid_pid(ctx, 0x12c9, 0x2003);
  if(!h){ fprintf(stderr, "Device 12c9:2003 not found\n"); libusb_exit(ctx); return 1; }

  int iface = 2;
  unsigned char ep_out = 0x04;

  if(libusb_kernel_driver_active(h, iface) == 1){
    rc = libusb_detach_kernel_driver(h, iface);
    if(rc){ fprintf(stderr, "detach kernel driver: %d\n", rc); }
  }
  rc = libusb_claim_interface(h, iface);
  if(rc){ fprintf(stderr, "claim interface %d: %d\n", iface, rc); return 1; }

  printf("unlock: %d\n", set_feature(h, iface, FEAT_UNLOCK));
  printf("arm:    %d\n", set_feature(h, iface, FEAT_ARM));
  printf("primer: %d\n", out64(h, ep_out, primer));
  printf("out64:  %d\n", out64(h, ep_out, out_blob));
  printf("final:  %d\n", set_feature(h, iface, FEAT_FINAL));

  libusb_release_interface(h, iface);
  libusb_close(h);
  libusb_exit(ctx);
  return 0;
}
