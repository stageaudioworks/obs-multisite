#!/usr/bin/env bash
#
# install.sh — turn a stock Raspberry Pi OS install into a campus player.
#
#   curl -fsSL --retry 5 https://raw.githubusercontent.com/stageaudioworks/obs-multisite/main/scripts/player/install.sh | sudo bash
#
# (From a checkout it is simply: sudo bash scripts/player/install.sh)
#
# --retry matters: GitHub's raw CDN occasionally answers 503 for a few minutes,
# and curl rides over that instead of handing over a half-install. The script
# itself also retries its own source fetch below rather than stopping silently
# when GitHub hiccups mid-run.
#
# It installs the build dependencies, builds the player, installs it as a
# player that starts on power-up, and leaves the box showing a screen with its
# own address and a QR code on it so somebody can finish the job from a phone.
#
# It also installs ZeroTier and cloudflared, the two optional tools that make a
# box at the back of a hall reachable from the office. Give it either or both
# and the box joins the private network / starts the tunnel as it installs:
#
#   ZT_NETWORK_ID=8056c2e21c000001 \
#   CF_TUNNEL_TOKEN=eyJhIjoi…        \
#     sudo -E bash scripts/player/install.sh
#
# Run from a terminal it asks for them instead; run the one-line way (piped
# from curl) there is no terminal to ask on, so pass them as above. Both can
# also be set or changed later from the web interface, and the ZeroTier
# address is printed on the box's own screen as the remote access address.
#
# Safe to run again: it updates an existing installation in place and keeps the
# settings and the segment cache.
#
# It builds the tip of main. To build a release, or any one commit, instead:
#
#   REF=v0.1.24-alpha sudo -E bash scripts/player/install.sh

set -euo pipefail

REPO_URL="${REPO_URL:-https://github.com/stageaudioworks/obs-multisite.git}"
BRANCH="${BRANCH:-main}"
# A tag or a full 40-character commit to build instead of the branch's tip, so
# whatever installs the player can pin exactly what it gets. Empty builds the
# branch, as always. A short hash cannot be fetched from GitHub by itself, so
# it is refused rather than half-honoured.
REF="${REF:-}"
SRC_DIR="${SRC_DIR:-/opt/multisite-player/src}"
PREFIX="${PREFIX:-/usr/local}"
CONFIG_DIR="/etc/multisite-player"
CONFIG="$CONFIG_DIR/config.json"
STATE_DIR="/var/lib/multisite-player"
SERVICE="multisite-player"
# One compiler per core is what makes a small Pi lock up and reboot part-way
# through a build: memory is the binding constraint, not cores, and the failure
# is not a failed build but an unresponsive machine. Work it out from the memory
# free when the build starts — which is less than the memory installed, because
# on a box being updated the player is already running. JOBS=N overrides it.
build_jobs() {
    local cores avail_kb by_mem
    cores="$(nproc 2>/dev/null || echo 2)"
    avail_kb="$(awk '/^MemAvailable:/ {print $2; exit}' /proc/meminfo 2>/dev/null || echo 0)"
    case "$avail_kb" in ''|*[!0-9]*) echo 1; return ;; esac
    [ "$avail_kb" -gt 0 ] || { echo 1; return; }
    by_mem=$(( avail_kb / 1024 / 900 ))
    [ "$by_mem" -lt 1 ] && by_mem=1
    if [ "$by_mem" -lt "$cores" ]; then echo "$by_mem"; else echo "$cores"; fi
}
JOBS="${JOBS:-$(build_jobs)}"

say()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
warn() { printf '\033[1;33m    %s\033[0m\n' "$*"; }
die()  { printf '\n\033[1;31mThat did not work:\033[0m %s\n\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "run this with sudo."

# ── What are we installing onto? ─────────────────────────────────────────────
MODEL="$(tr -d '\0' < /sys/firmware/devicetree/base/model 2>/dev/null || echo 'unknown machine')"
say "Installing the campus player on: $MODEL"

case "$(uname -m)" in
  aarch64|arm64|x86_64) ;;
  *) warn "This has only been tested on 64-bit ARM and x86. Carrying on anyway." ;;
esac

if [ -n "${DISPLAY:-}" ] || systemctl is-active --quiet lightdm 2>/dev/null \
   || systemctl is-active --quiet gdm3 2>/dev/null; then
  warn "A desktop is running on this box."
  warn "The player takes over the HDMI output directly and cannot share it"
  warn "with a desktop. Raspberry Pi OS Lite is the right image for a campus"
  warn "box. To carry on here, stop the desktop first:"
  warn "    sudo systemctl disable --now lightdm"
fi

# ── Dependencies ─────────────────────────────────────────────────────────────
say "Installing what it needs to build"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends \
  build-essential cmake pkg-config git ca-certificates \
  libcurl4-openssl-dev libssl-dev \
  libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libswscale-dev \
  libdrm-dev libasound2-dev \
  libqrencode-dev libfreetype-dev \
  fonts-dejavu-core \
  >/dev/null
note "done"

# ── Remote access ────────────────────────────────────────────────────────────
# A box at the back of a hall cannot be fixed without somebody driving to the
# campus, and a church network rarely allows an inbound port-forward. Two
# optional tools remove that drive: ZeroTier puts the box on a private network
# that follows it, and cloudflared publishes the operator page on a public
# hostname. Both are installed here; whether they are used is decided by the
# two values below, and either can be added or changed later from the web
# interface. Nothing below is fatal — a box with neither still plays the
# event, which is the only thing that has to work.
say "Setting up remote access (optional)"

ZT_NETWORK_ID="${ZT_NETWORK_ID:-}"
CF_TUNNEL_TOKEN="${CF_TUNNEL_TOKEN:-}"

install_zerotier() {
  command -v zerotier-cli >/dev/null 2>&1 && return 0
  # ZeroTier's own installer adds their repository and the daemon; the stock
  # Debian package is old enough to be a support problem by itself.
  curl -fsSL --retry 5 https://install.zerotier.com 2>/dev/null \
    | bash >/dev/null 2>&1 || true
  command -v zerotier-cli >/dev/null 2>&1 && return 0
  apt-get install -y --no-install-recommends zerotier-one >/dev/null 2>&1 || true
  command -v zerotier-cli >/dev/null 2>&1
}

install_cloudflared() {
  command -v cloudflared >/dev/null 2>&1 && return 0
  # Cloudflare's apt repository, with its signing key pinned by the file we
  # fetched rather than by trusting whatever the network returned.
  mkdir -p --mode=0755 /usr/share/keyrings
  if curl -fsSL --retry 5 https://pkg.cloudflare.com/cloudflare-main.gpg \
       -o /usr/share/keyrings/cloudflare-main.gpg 2>/dev/null; then
    echo "deb [signed-by=/usr/share/keyrings/cloudflare-main.gpg] https://pkg.cloudflare.com/cloudflared any main" \
      > /etc/apt/sources.list.d/cloudflared.list
    apt-get update -qq || true
    apt-get install -y --no-install-recommends cloudflared >/dev/null 2>&1 || true
  fi
  command -v cloudflared >/dev/null 2>&1
}

install_zerotier && note "ZeroTier installed" \
  || warn "could not install ZeroTier — it can be added later"
install_cloudflared && note "cloudflared installed" \
  || warn "could not install cloudflared — it can be added later"

# Ask for the two values only when there is a terminal to ask on. Piped in
# from curl, stdin is the script's own text and reading it there would eat the
# rest of the installer — so in that form they have to be passed in.
if [ -z "$ZT_NETWORK_ID" ] && [ -r /dev/tty ]; then
  printf '    ZeroTier network key (16 hex digits, blank to skip): '
  read -r ZT_NETWORK_ID < /dev/tty || ZT_NETWORK_ID=""
fi
if [ -z "$CF_TUNNEL_TOKEN" ] && [ -r /dev/tty ]; then
  printf '    Cloudflare tunnel token (blank to skip): '
  read -r CF_TUNNEL_TOKEN < /dev/tty || CF_TUNNEL_TOKEN=""
fi

# Join the ZeroTier network now so the address is up by the time the splash
# screen first draws. The member still has to be authorised in ZeroTier
# Central before it answers; until then the box shows the setting but no
# address, which is the honest picture.
if [ -n "$ZT_NETWORK_ID" ] && command -v zerotier-cli >/dev/null 2>&1; then
  systemctl enable --quiet --now zerotier-one 2>/dev/null || true
  for _ in $(seq 1 10); do
    zerotier-cli info >/dev/null 2>&1 && break
    sleep 1
  done
  if zerotier-cli join "$ZT_NETWORK_ID" >/dev/null 2>&1; then
    note "joined ZeroTier network $ZT_NETWORK_ID"
    note "authorise this box in ZeroTier Central to give it an address"
  else
    warn "could not join the ZeroTier network — it can be set from the web interface"
  fi
fi

if [ -n "$CF_TUNNEL_TOKEN" ] && command -v cloudflared >/dev/null 2>&1; then
  if cloudflared service install "$CF_TUNNEL_TOKEN" >/dev/null 2>&1; then
    note "Cloudflare tunnel installed and running"
  else
    warn "could not install the Cloudflare tunnel — it can be set from the web interface"
  fi
fi

# ── Source ───────────────────────────────────────────────────────────────────
if [ -d "$SRC_DIR/.git" ]; then
  say "Updating the source"
  current="$(git -C "$SRC_DIR" log -1 --format='%h %s' 2>/dev/null || true)"
  note "current: ${current:-unknown}"
  # The one-line bootstrap clones the branch itself and asks to skip this
  # step; an ordinary re-run of the script fetches and resets. Fetching is
  # retried, because GitHub's servers hiccup and a box mid-install has no one
  # to ask — this is the step that must never end the script silently.
  if [ "${SKIP_GIT_UPDATE:-0}" != "1" ]; then
    # Fetch into the branch's own remote-tracking ref. A plain `git fetch
    # origin $BRANCH` writes only FETCH_HEAD, so the reset below would fail
    # the first time a box is moved from one branch to another. The `+` force
    # is deliberate: in a shallow clone git can decide an update is not a
    # fast-forward because it cannot see the shared history, and without the
    # force the fetch is rejected — silently, if --quiet is hiding the
    # rejection. Remote-tracking refs are bookkeeping; a plain `git fetch
    # origin` force-updates them, so this matches normal behaviour.
    fetched=""
    for attempt in 1 2 3 4 5; do
      if git -C "$SRC_DIR" fetch --depth 1 \
            origin "+$BRANCH:refs/remotes/origin/$BRANCH"; then
        fetched=1
        break
      fi
      warn "could not reach GitHub (attempt $attempt of 5) — retrying in ${attempt}s"
      sleep "$attempt"
    done
    [ -n "$fetched" ] \
      || die "could not fetch the '$BRANCH' branch from GitHub — check this box's internet and run this again"
    git -C "$SRC_DIR" reset --quiet --hard "origin/$BRANCH"
  fi
else
  say "Fetching the source"
  mkdir -p "$(dirname "$SRC_DIR")"
  git clone --quiet --depth 1 --branch "$BRANCH" "$REPO_URL" "$SRC_DIR"
fi
if [ -n "$REF" ] && [ "${SKIP_GIT_UPDATE:-0}" != "1" ]; then
  case "$REF" in
    *[!0-9a-f]*) ;;   # not a hash: a tag
    *) [ "${#REF}" -eq 40 ] \
         || die "REF=$REF looks like a short commit hash — give the full 40 characters, or a tag" ;;
  esac
  say "Pinning the source to $REF"
  fetched=""
  for attempt in 1 2 3 4 5; do
    if git -C "$SRC_DIR" fetch --quiet --depth 1 origin "$REF"; then
      fetched=1
      break
    fi
    warn "could not fetch $REF (attempt $attempt of 5) — retrying in ${attempt}s"
    sleep "$attempt"
  done
  [ -n "$fetched" ] \
    || die "could not fetch '$REF' from GitHub — check the tag or commit exists, and this box's internet"
  git -C "$SRC_DIR" reset --quiet --hard FETCH_HEAD
fi
note "building $(git -C "$SRC_DIR" log -1 --format='%H %s')"

# ── Build ────────────────────────────────────────────────────────────────────
say "Building $JOBS at a time (this takes a few minutes on a Pi)"
cmake -S "$SRC_DIR" -B "$SRC_DIR/build" \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_PLAYER=ON \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
cmake --build "$SRC_DIR/build" --target multisite-player -j "$JOBS" >/dev/null
cmake --install "$SRC_DIR/build" >/dev/null 2>&1 || {
  # Only the player is wanted here, not every optional target the project can
  # produce.
  install -m 0755 "$SRC_DIR/build/multisite-player" "$PREFIX/bin/multisite-player"
  mkdir -p "$PREFIX/share/multisite-player/web"
  cp -r "$SRC_DIR/src/appliance/web/." "$PREFIX/share/multisite-player/web/"
}
note "installed $($PREFIX/bin/multisite-player --version)"

# ── Where the cache goes ─────────────────────────────────────────────────────
# The cache writes roughly 3 GB an hour. On an SD card that is a wear-out
# problem, not a performance one, so a USB SSD is looked for and used.
say "Choosing where to keep the downloaded event"
CACHE_DIR="$STATE_DIR/cache"
SSD_MOUNT=""
while read -r src target fstype _; do
  case "$src" in
    /dev/sd*|/dev/nvme*)
      case "$target" in
        /|/boot*) ;;
        *) [ "$fstype" != "vfat" ] && SSD_MOUNT="$target" && break ;;
      esac ;;
  esac
done < /proc/self/mounts

if [ -n "$SSD_MOUNT" ]; then
  CACHE_DIR="$SSD_MOUNT/multisite-player/cache"
  note "using the drive mounted at $SSD_MOUNT"
else
  ROOT_SRC="$(findmnt -n -o SOURCE / 2>/dev/null || true)"
  case "$ROOT_SRC" in
    *mmcblk*)
      warn "No USB drive found, so the cache will go on the SD card."
      warn "That writes about 3 GB an hour and will wear the card out."
      warn "Plug in a USB SSD and change the cache folder in Settings." ;;
  esac
fi
mkdir -p "$CACHE_DIR" "$STATE_DIR"

# ── Settings ─────────────────────────────────────────────────────────────────
mkdir -p "$CONFIG_DIR"
chmod 0700 "$CONFIG_DIR"
CONFIG_EXISTED=""
if [ -f "$CONFIG" ]; then
  say "Keeping the settings already on this box"
  CONFIG_EXISTED=1
else
  say "Writing a starting set of settings"
  cat > "$CONFIG" <<EOF
{
  "room_id": "main-auditorium",
  "cache_dir": "$CACHE_DIR",
  "web_port": 8080,
  "idle_mode": "splash",
  "auto_play": true,
  "audio_enabled": true,
  "alsa_device": "default",
  "buffer_minutes": 10,
  "start_buffer_seconds": 60,
  "zerotier_network_id": "$ZT_NETWORK_ID",
  "cloudflared_token": "$CF_TUNNEL_TOKEN"
}
EOF
  chmod 0600 "$CONFIG"
  note "the storage details still need entering — from a browser, in a moment"
fi

# Remember a network key or token given above even when the config already
# existed, without rewriting the rest of the file. python3 is present on
# Raspberry Pi OS Lite; where it is not, a fresh install has already written
# the values and only a re-run of the installer loses them.
if [ -n "$CONFIG_EXISTED" ] && command -v python3 >/dev/null 2>&1 \
   && { [ -n "$ZT_NETWORK_ID" ] || [ -n "$CF_TUNNEL_TOKEN" ]; }; then
  python3 - "$CONFIG" "$ZT_NETWORK_ID" "$CF_TUNNEL_TOKEN" <<'PY'
import json, sys
path, zt, cf = sys.argv[1], sys.argv[2], sys.argv[3]
with open(path) as f:
    cfg = json.load(f)
if zt:
    cfg["zerotier_network_id"] = zt
if cf:
    cfg["cloudflared_token"] = cf
with open(path, "w") as f:
    json.dump(cfg, f, indent=2)
    f.write("\n")
PY
  chmod 0600 "$CONFIG"
fi

# ── Service ──────────────────────────────────────────────────────────────────
say "Setting it to start on power-up"
install -m 0644 "$SRC_DIR/scripts/player/$SERVICE.service" \
        "/etc/systemd/system/$SERVICE.service"
systemctl daemon-reload
systemctl enable --quiet "$SERVICE"
systemctl restart "$SERVICE"

# The console cursor and kernel messages would otherwise blink over the
# picture the player is putting on the screen.
if [ -w /sys/class/graphics/fb0/blank ] 2>/dev/null; then
  printf 0 > /sys/class/graphics/fb0/blank || true
fi
if ! grep -q 'consoleblank=0' /boot/firmware/cmdline.txt 2>/dev/null; then
  warn "To stop the console blanking the screen, add consoleblank=0 to"
  warn "/boot/firmware/cmdline.txt and reboot."
fi

# RestartSec is 3 seconds, so a box that is merely between restart attempts
# looks identical to a box that has given up if you only glance once. Watch it
# for long enough to see it settle, and only then call it.
settled=""
for _ in $(seq 1 12); do
  if systemctl is-active --quiet "$SERVICE"; then
    settled="yes"
  else
    settled=""
  fi
  sleep 1
done
if [ -z "$settled" ] || ! systemctl is-active --quiet "$SERVICE"; then
  warn "The player did not stay running. What it said:"
  journalctl -u "$SERVICE" -n 40 --no-pager || true
  warn ""
  warn "To see it fail with the log in front of you:"
  warn "    sudo systemctl stop $SERVICE"
  warn "    sudo $PREFIX/bin/multisite-player --config $CONFIG --verbose"
  die "see above"
fi

# ── Where to go next ─────────────────────────────────────────────────────────
PORT="$(grep -o '"web_port"[[:space:]]*:[[:space:]]*[0-9]*' "$CONFIG" \
        | grep -o '[0-9]*$' || echo 8080)"
say "Done. Finish setting it up from a browser on this network:"
ip -4 -o addr show scope global 2>/dev/null \
  | awk -v p="$PORT" '{split($4,a,"/"); printf "        http://%s:%s   (%s)\n", a[1], p, $2}'
echo
note "The same address is on the screen attached to this box — as text and as a"
note "QR code, so a phone pointed at the screen opens the control page."
note "Enter the bucket details under Settings, then press Play."
echo

# The remote access address, if the ZeroTier interface has come up. It only
# appears once the box has been authorised in ZeroTier Central, so a network
# key with no address yet is reported as waiting rather than as a failure.
ZT_IP="$(ip -4 -o addr show 2>/dev/null \
         | awk '$2 ~ /^zt/ {split($4,a,"/"); print a[1]; exit}')"
if [ -n "$ZT_IP" ]; then
  note "Remote access address (shown on the screen as the remote access IP):"
  note "        http://$ZT_IP:$PORT"
elif [ -n "${ZT_NETWORK_ID:-}" ]; then
  note "ZeroTier joined $ZT_NETWORK_ID but has no address yet — authorise this"
  note "box in ZeroTier Central and it will appear."
fi
echo
note "If something is wrong:  journalctl -u $SERVICE -f"
note "To update it later:     sudo bash $SRC_DIR/scripts/player/install.sh"
echo
