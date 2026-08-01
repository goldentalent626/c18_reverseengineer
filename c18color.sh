#!/usr/bin/env bash
set -euo pipefail

BIN="${BIN:-./c18_usb_seq3}"

BASE_STATIC="0f001e0a191905010103646401c0f0030101191922395cd0f35c730022395cd0f35c73001fff0000000031000031000031000031000031000031000031000031"

color="${1:-}"
case "$color" in
  red)    RGB="31 00 00" ;;
  green)  RGB="00 31 00" ;;
  blue)   RGB="00 00 31" ;;
  yellow) RGB="31 31 00" ;;
  purple) RGB="31 00 31" ;;
  cyan)   RGB="00 31 31" ;;
  white)  RGB="31 31 31" ;;
  off)    RGB="00 00 00" ;;
  *) echo "Usage: $0 {red|green|blue|yellow|purple|cyan|white|off}"; exit 2 ;;
esac

python3 - <<PY
base = bytes.fromhex("$BASE_STATIC")
b = bytearray(base)
trip = bytes(int(x,16) for x in "$RGB".split())
# bytes 40..63 = 8 RGB triplets
for i in range(8):
    b[40 + i*3 : 40 + i*3 + 3] = trip
print(b.hex())
PY | sudo "$BIN" "$(cat)"
