#!/usr/bin/env bash
# Each box its own AES67 group (merging-aes67.sh). Two boxes on the shared
# 239.1.0.1 doubled every packet a receiver heard; the installer and the player
# must agree on the address, so the player's own examples are checked here too.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source-path=SCRIPTDIR source=../scripts/player/aes67-address.sh
. "$here/../scripts/player/aes67-address.sh"

fails=0
check() {  # name, got, want
  if [ "$2" = "$3" ]; then echo "ok    $1"
  else echo "FAIL  $1"; echo "        got:  $2"; echo "        want: $3"; fails=$((fails + 1)); fi
}
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
SYSFS_NET="$tmp"
card() {  # mac -> the address for an interface with that MAC
  mkdir -p "$tmp/eth9"
  printf '%s\n' "$1" > "$tmp/eth9/address"
  IFACE=eth9 box_mcast_address
}

check "from the last two bytes (as the player: test_aes67)" "$(card 02:00:00:00:00:e0)" "239.1.0.224"
check "hex in either case" "$(card D8:3A:DD:12:AB:CD)" "239.1.171.205"
check "two boxes, two groups" \
      "$([ "$(card 02:00:00:00:00:e0)" != "$(card d8:3a:dd:12:ab:cd)" ] && echo yes)" "yes"
check "never the old shared group" "$(card 00:11:22:33:00:01)" "239.1.0.2"
check "not a MAC: the old group, as a last resort" "$(card zz:00:00:00:00:e0)" "239.1.0.1"
check "no such card: likewise" "$(IFACE=nope box_mcast_address)" "239.1.0.1"
check "the installer uses it for the stream and the daemon's base" \
      "$(grep -c 'box_mcast_address)' "$here/../scripts/player/merging-aes67.sh")" "3"
check "and no longer writes 239.1.0.1 into the daemon's configuration" \
      "$(grep -c '"rtp_mcast_base": "239.1.0.1"' "$here/../scripts/player/merging-aes67.sh")" "0"

[ "$fails" -eq 0 ] && echo "aes67 address: all passed" || echo "aes67 address: $fails failed"
exit "$fails"
