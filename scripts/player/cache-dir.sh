# shellcheck shell=bash
#
# cache-dir.sh — where the player keeps the event it downloads.
#
# Sourced by install.sh, and by tests/test_cache_dir.sh, which drives it from
# fixture files. It only decides and reports; nothing here runs on sourcing.
#
# The cache writes roughly 3 GB an hour. On an SD card that is a wear-out
# problem, so a separate USB SSD or NVMe drive is looked for and used. The
# trap (obs-multisite#35): /proc/self/mounts shows a bind mount of one
# directory exactly like a drive. Armbian's ramlog bind-mounts the root
# filesystem's own /var/log at /var/log.hdd, and copies that into a 50 MB RAM
# disk at every boot; the installer took it for a drive, and the cache filled
# /var/log until logging stopped. Only /proc/self/mountinfo says which part of
# a filesystem a mount shows, so that is what is read.

# Overridable, so the tests can hand in a fixture instead of this machine's.
MOUNTINFO="${MOUNTINFO:-/proc/self/mountinfo}"
# Prefixed to a path before anything is removed, so the tests remove only
# inside their own directory.
CACHE_FS_ROOT="${CACHE_FS_ROOT:-}"

# /var/log and anything beside it: Armbian's ramlog and log2ram on Raspberry
# Pi OS keep it in RAM, or copy it there at boot.
cache_path_is_log() {
  case "$1" in
    /var/log|/var/log/*|/var/log.*) return 0 ;;
  esac
  return 1
}

# A separate drive to keep the cache on: prints its mount point, or nothing.
#
# A mountinfo line is: id parent major:minor root mountpoint options
# [optional fields...] - fstype source superoptions. A mount is a whole
# filesystem only when its root (field 4) is "/"; anything else is a bind of a
# directory inside one.
cache_drive_mount() {
  local line pre post dev root target fstype src root_dev=""
  [ -r "$MOUNTINFO" ] || return 0
  # The root filesystem itself, so that another mount of it (a bind of the
  # whole root, say) is not taken for a second drive.
  while IFS= read -r line; do
    pre="${line%% - *}"
    read -r _ _ dev root target _ <<<"$pre"
    if [ "$target" = "/" ]; then root_dev="$dev"; fi
  done < "$MOUNTINFO"

  while IFS= read -r line; do
    pre="${line%% - *}"
    post="${line#* - }"
    read -r _ _ dev root target _ <<<"$pre"
    read -r fstype src _ <<<"$post"
    [ "$root" = "/" ] || continue
    [ "$dev" != "$root_dev" ] || continue
    case "$src" in /dev/sd*|/dev/nvme*) ;; *) continue ;; esac
    case "$target" in /|/boot*) continue ;; esac
    [ "$fstype" != "vfat" ] || continue
    if cache_path_is_log "$target"; then continue; fi
    printf '%s\n' "$target"
    return 0
  done < "$MOUNTINFO"
}

# The cache directory to use. With no separate drive it stays in the state
# directory on the root filesystem: on an NVMe or SSD root there is nothing to
# wear out, and on an SD card install.sh says so.
choose_cache_dir() {
  local state_dir="$1" mount
  mount="$(cache_drive_mount)"
  if [ -n "$mount" ]; then
    printf '%s/multisite-player/cache\n' "$mount"
  else
    printf '%s/cache\n' "$state_dir"
  fi
}

# An existing config whose cache_dir is under /var/log* is moved to $2. Prints
# the old directory when it moved, so the caller can remove it once the player
# has been restarted away from it; prints nothing otherwise.
cache_repair_config() {
  local config="$1" new_dir="$2" current
  [ -f "$config" ] || return 0
  command -v python3 >/dev/null 2>&1 || return 0
  current="$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1])).get("cache_dir", ""))' \
             "$config" 2>/dev/null || true)"
  cache_path_is_log "$current" || return 0
  python3 - "$config" "$new_dir" <<'PY'
import json, sys
path, new_dir = sys.argv[1], sys.argv[2]
with open(path) as f:
    cfg = json.load(f)
cfg["cache_dir"] = new_dir
with open(path, "w") as f:
    json.dump(cfg, f, indent=2)
    f.write("\n")
PY
  printf '%s\n' "$current"
}

# Remove a cache directory the config no longer points at. Only one the
# installer made (…/multisite-player/cache) under /var/log*: it holds
# downloaded segments, which the player fetches again, and nothing else is
# ever removed by this.
cache_remove_old() {
  local old="$1"
  cache_path_is_log "$old" || return 0
  case "$old" in */multisite-player/cache) ;; *) return 0 ;; esac
  rm -rf -- "${CACHE_FS_ROOT}${old}"
  rmdir -- "${CACHE_FS_ROOT}${old%/cache}" 2>/dev/null || true
}
