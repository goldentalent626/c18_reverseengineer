// c18_usb_seq3.c
// Send C18 vendor LED/profile command sequence in ONE session.
// Detach + claim interfaces 0/1/2, then:
//   SET_REPORT unlock (0188..)
//   SET_REPORT arm    (0c01..)
//   OUT 64 primer     (ffff00..)
//   OUT 64 payload    (0f00.. blob)
//   SET_REPORT final  (0201..)
// Includes small delays to match Windows timing.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
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
  // HID SET_REPORT (Feature), ReportID=0, wValue=0x0300, wIndex=iface, len=8
  return libusb_control_transfer(
      h,
      0x21,          // bmRequestType: Host-to-device | Class | Interface
      0x09,          // bRequest: SET_REPORT
      0x0300,        // wValue: Feature (3) << 8 | ReportID (0)
      (uint16_t)iface,
      (unsigned char*)feat,
      8,
      1000
  );
}

static int out64(libusb_device_handle *h, unsigned char ep_out, const unsigned char buf[64], int *xfer){
  int transferred=0;
  int rc = libusb_interrupt_transfer(h, ep_out, (unsigned char*)buf, 64, &transferred, 1000);
  if(xfer) *xfer = transferred;
  return rc; // 0 on success
}

int main(int argc, char **argv){
  if(argc != 2){
    fprintf(stderr,
      "Usage: %s <out64_hex>\n\n"
      "Example:\n"
      "  sudo %s 0f001e0a191905010103...<64 bytes total>...\n",
      argv[0], argv[0]
    );
    return 2;
  }

  unsigned char payload[64];
  int olen = hex2bytes(argv[1], payload, 64);
  if(olen != 64){
    fprintf(stderr, "Error: payload must be exactly 64 bytes (128 hex chars). Got %d bytes.\n", olen);
    return 2;
  }

  // Feature reports observed in your Windows capture
  const unsigned char FEAT_UNLOCK[8] = {0x01,0x88,0x00,0x00,0x00,0x00,0x00,0x76};
  const unsigned char FEAT_ARM[8]    = {0x0c,0x01,0x80,0x00,0x00,0x00,0x00,0x72};
  const unsigned char FEAT_FINAL[8]  = {0x02,0x01,0x00,0x00,0x00,0x00,0x00,0xfc};

  // Primer packet observed: ffff0000... padded to 64 bytes
  unsigned char primer[64] = {0};
  primer[0] = 0xff;
  primer[1] = 0xff;

  libusb_context *ctx=NULL;
  libusb_device_handle *h=NULL;

  int rc = libusb_init(&ctx);
  if(rc){
    fprintf(stderr, "libusb_init failed: %d\n", rc);
    return 1;
  }

  h = libusb_open_device_with_vid_pid(ctx, 0x12c9, 0x2003);
  if(!h){
    fprintf(stderr, "Device 12c9:2003 not found.\n");
    libusb_exit(ctx);
    return 1;
  }

  // Vendor interface is 2, but we detach+claim 0/1/2 to avoid kernel HID interference.
  const int ifaces[] = {0,1,2};
  const int nifaces = 3;
  for(int i=0;i<nifaces;i++){
    int iface = ifaces[i];
    int active = libusb_kernel_driver_active(h, iface);
    if(active == 1){
      // Ignore detach errors; some systems return errors even though claim works.
      libusb_detach_kernel_driver(h, iface);
    }
    rc = libusb_claim_interface(h, iface);
    if(rc){
      fprintf(stderr, "claim interface %d failed: %d\n", iface, rc);
      libusb_close(h);
      libusb_exit(ctx);
      return 1;
    }
  }

  // Endpoint 0x04 OUT is on interface 2
  unsigned char ep_out = 0x04;

  int xfer=0;

  rc = set_feature(h, 2, FEAT_UNLOCK);
  printf("unlock: %s (%d)\n", (rc>=0?"ok":"fail"), rc);
  usleep(20000);

  rc = set_feature(h, 2, FEAT_ARM);
  printf("arm:    %s (%d)\n", (rc>=0?"ok":"fail"), rc);
  usleep(20000);

  rc = out64(h, ep_out, primer, &xfer);
  printf("primer: %s (%d) transferred=%d\n", (rc==0?"ok":"fail"), rc, xfer);
  usleep(20000);

  rc = out64(h, ep_out, payload, &xfer);
  printf("out64:  %s (%d) transferred=%d\n", (rc==0?"ok":"fail"), rc, xfer);
  usleep(40000);

  rc = set_feature(h, 2, FEAT_FINAL);
  printf("final:  %s (%d)\n", (rc>=0?"ok":"fail"), rc);
  usleep(20000);

  for(int i=0;i<nifaces;i++){
    libusb_release_interface(h, ifaces[i]);
  }

  libusb_close(h);
  libusb_exit(ctx);
  return 0;
}
