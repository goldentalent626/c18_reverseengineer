#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/hidraw.h>

static int hexval(char c){
  if(c>='0'&&c<='9') return c-'0';
  if(c>='a'&&c<='f') return 10+c-'a';
  if(c>='A'&&c<='F') return 10+c-'A';
  return -1;
}

static int hex2bytes(const char *hex, unsigned char *out, int max){
  int n=0;
  while(*hex){
    while(*hex==' '||*hex=='\n'||*hex=='\t') hex++;
    if(!*hex) break;
    int hi=hexval(*hex++); if(hi<0) return -1;
    int lo=hexval(*hex++); if(lo<0) return -1;
    if(n>=max) return -2;
    out[n++] = (unsigned char)((hi<<4)|lo);
  }
  return n;
}

int main(int argc, char **argv){
  if(argc < 4){
    fprintf(stderr, "Usage:\n"
      "  %s /dev/hidraw11 feature <hexbytes>\n"
      "  %s /dev/hidraw11 write64  <hexbytes>\n", argv[0], argv[0]);
    return 2;
  }

  const char *path = argv[1];
  const char *mode = argv[2];
  const char *hex  = argv[3];

  int fd = open(path, O_RDWR);
  if(fd < 0){ perror("open"); return 1; }

  unsigned char buf[1024];
  int len = 0;

  if(strcmp(mode, "feature") == 0){
    // For report ID 0, HIDIOCSFEATURE wants a leading 0x00.
    // Your Windows capture shows 8 bytes of payload with report id 0.
    unsigned char payload[256];
    int plen = hex2bytes(hex, payload, (int)sizeof(payload));
    if(plen < 0){ fprintf(stderr, "Bad hex\n"); return 1; }

    buf[0] = 0x00; // report id 0
    memcpy(buf+1, payload, plen);
    len = 1 + plen;

    int res = ioctl(fd, HIDIOCSFEATURE(len), buf);
    if(res < 0){ perror("HIDIOCSFEATURE"); close(fd); return 1; }
    printf("Sent feature report (%d bytes payload, %d total)\n", plen, len);
  }
  else if(strcmp(mode, "write64") == 0){
    len = hex2bytes(hex, buf, (int)sizeof(buf));
    if(len != 64){
      fprintf(stderr, "write64 expects exactly 64 bytes, got %d\n", len);
      close(fd);
      return 2;
    }
    int w = write(fd, buf, 64);
    if(w < 0){ perror("write"); close(fd); return 1; }
    printf("Wrote %d bytes to interrupt OUT\n", w);
  }
  else {
    fprintf(stderr, "Unknown mode: %s\n", mode);
    close(fd);
    return 2;
  }

  close(fd);
  return 0;
}
