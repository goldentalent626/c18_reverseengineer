#!/usr/bin/env bash
set -euo pipefail

BIN="${BIN:-./c18_usb_seq3}"

# Start from a known STATIC template (we'll patch brightness tier + RGB block)
BASE="0f001e0a191905010103646401c0f0030101191922395cd0f35c730022395cd0f35c73001fff0000000031000031000031000031000031000031000031000031"

color="${1:-}"
level="${2:-high}"

case "$level" in
  high) ON=0x31; TIER=0x01 ;;
  mid|middle) ON=0x20; TIER=0x02 ;;
  low) ON=0x10; TIER=0x03 ;;
  *) echo "Level: high|mid|low"; exit 2 ;;
esac

case "$color" in
  red)    MASK="1 0 0" ;;
  green)  MASK="0 1 0" ;;
  blue)   MASK="0 0 1" ;;
  yellow) MASK="1 1 0" ;;
  purple) MASK="1 0 1" ;;
  cyan)   MASK="0 1 1" ;;
  white)  MASK="1 1 1" ;;
  off)    MASK="0 0 0" ;;
  *) echo "Color: red|green|blue|yellow|purple|cyan|white|off"; exit 2 ;;
esac

python3 - <<PY | sudo "$BIN" "$(cat)"
base = bytearray.fromhex("$BASE")

# Patch brightness tier in mode block: ... 19 19 05 01 <TIER> 03 ...
# We know this is at bytes 4..??; easiest: locate the pattern 19 19 05 01 ?? 03
pat = bytes([0x19,0x19,0x05,0x01])
idx = base.find(pat)
if idx < 0:
    raise SystemExit("Could not locate mode block")
base[idx+4] = int("$TIER", 16)  # tier byte (01/02/03)
# idx+5 stays 0x03

# Patch the last 24 bytes (8 RGB triplets) with ON value
on = int("$ON", 16)
m = list(map(int, "$MASK".split()))
trip = bytes([(on if m[0] else 0), (on if m[1] else 0), (on if m[2] else 0)])
for i in range(8):
    base[40+i*3:40+i*3+3] = trip

print(base.hex())
PY
