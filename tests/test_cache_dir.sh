#!/usr/bin/env bash
# Where the player installer keeps the downloaded event (obs-multisite#35).
# It must take a separate drive, and never a bind mount of a directory, which
# /proc/self/mounts shows exactly like one: on Armbian that was /var/log.hdd,
# copied into a 50 MB RAM disk at every boot until logging stopped.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
fixtures="$here/fixtures/mountinfo"
# shellcheck source-path=SCRIPTDIR source=../scripts/player/cache-dir.sh
. "$here/../scripts/player/cache-dir.sh"

fails=0
check() {  # name, got, want
  if [ "$2" = "$3" ]; then echo "ok    $1"
  else echo "FAIL  $1"; echo "        got:  $2"; echo "        want: $3"; fails=$((fails + 1)); fi
}
state=/var/lib/multisite-player
there() { if [ -e "$1" ]; then echo yes; else echo no; fi; }
choose() { MOUNTINFO="$fixtures/$1" choose_cache_dir "$state"; }

check "Armbian on NVMe with ramlog: the root's own cache, not /var/log.hdd" \
      "$(choose armbian-nvme-ramlog.txt)" "$state/cache"
check "a USB SSD mounted on its own is still chosen" \
      "$(choose pi-sd-usb-ssd.txt)" "/mnt/ssd/multisite-player/cache"
check "a bind of a directory on the SSD is not a drive; the SSD's own mount is" \
      "$(choose sd-root-ssd-log-bind.txt)" "/srv/media/multisite-player/cache"
check "a whole drive mounted at /var/log is still not somewhere to keep it" \
      "$(choose ssd-at-var-log.txt)" "$state/cache"
check "no mountinfo to read: the root's own cache" \
      "$(MOUNTINFO=/nonexistent choose_cache_dir "$state")" "$state/cache"

# Re-running over a config an earlier installer wrote.
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
config="$tmp/config.json"
old=/var/log.hdd/multisite-player/cache
mkdir -p "$tmp$old"
echo segment > "$tmp$old/seg-1.m4s"
printf '{\n  "room_id": "main",\n  "cache_dir": "%s"\n}\n' "$old" > "$config"
moved="$(cache_repair_config "$config" "$state/cache")"
check "a cache_dir under /var/log* is reported as moved" "$moved" "$old"
check "  the config now points at the new one, and keeps the rest" \
      "$(python3 -c 'import json,sys; c=json.load(open(sys.argv[1])); print(c["cache_dir"], c["room_id"])' "$config")" \
      "$state/cache main"
CACHE_FS_ROOT="$tmp" cache_remove_old "$moved"
check "  and the old directory is removed" "$(there "$tmp$old") $(there "$tmp/var/log.hdd/multisite-player")" "no no"
check "  leaving what else was beside it" "$(there "$tmp/var/log.hdd")" "yes"

printf '{"cache_dir": "/mnt/ssd/multisite-player/cache"}\n' > "$config"
check "a cache_dir elsewhere is left alone" "$(cache_repair_config "$config" "$state/cache")" ""
check "  unchanged" "$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["cache_dir"])' "$config")" \
      "/mnt/ssd/multisite-player/cache"

mkdir -p "$tmp/var/log.hdd/other"
CACHE_FS_ROOT="$tmp" cache_remove_old /var/log.hdd/other
check "nothing but an installer's …/multisite-player/cache is ever removed" "$(there "$tmp/var/log.hdd/other")" "yes"

[ "$fails" -eq 0 ] && echo "ALL PASS" || echo "FAILURES: $fails"
exit "$((fails > 0))"
