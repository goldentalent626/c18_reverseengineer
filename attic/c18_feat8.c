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
      "  %s /dev/hidraw11 feature8 <16-hex-chars>\n"
      "  %s /dev/hidraw11 write64  <128-hex-chars>\n", argv[0], argv[0]);
    return 2;
  }

  int fd = open(argv[1], O_RDWR);
  if(fd < 0){ perror("open"); return 1; }

  unsigned char buf[256];
  int len = hex2bytes(argv[3], buf, sizeof(buf));
  if(len < 0){ fprintf(stderr, "Bad hex\n"); return 1; }

  if(strcmp(argv[2], "feature8") == 0){
    if(len != 8){ fprintf(stderr, "feature8 needs exactly 8 bytes, got %d\n", len); return 2; }
    if(ioctl(fd, HIDIOCSFEATURE(len), buf) < 0){ perror("HIDIOCSFEATURE"); return 1; }
    puts("Sent 8-byte feature report");
  } else if(strcmp(argv[2], "write64") == 0){
    if(len != 64){ fprintf(stderr, "write64 needs exactly 64 bytes, got %d\n", len); return 2; }
    if(write(fd, buf, 64) < 0){ perror("write"); return 1; }
    puts("Wrote 64-byte OUT report");
  } else {
    fprintf(stderr, "Unknown mode\n"); return 2;
  }

  close(fd);
  return 0;
}
