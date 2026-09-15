#!/usr/bin/env bash
#
# uninstall.sh — take the campus player back off a box.
#
#   sudo bash scripts/player/uninstall.sh
#
# or, on the box itself, with no checkout to hand:
#
#   curl -fsSL --retry 5 https://raw.githubusercontent.com/stageaudioworks/obs-multisite/main/scripts/player/uninstall.sh | sudo bash
#
# Why this exists
# ---------------
# install.sh turns a stock Raspberry Pi OS image into a campus player and leaves
# it starting on power-up. This is the mirror image of it: every path below is
# one that installer wrote, so a box set up by it is fully covered. It stops and
# disables the service, removes its unit, the program, the web page, the source
# it was built from, the settings and the downloaded event — and then asks the
# same questions again to show that they are gone.
#
# What it leaves, unless asked
# ----------------------------
# The two remote-access tools install.sh brings up — ZeroTier and cloudflared —
# and the open AES67 stack merging-aes67.sh installs are each their own
# decision, because a box can keep any of them after the player is gone: the
# tunnel may still be how the box is reached, and the sound may still be on the
# network. So neither is touched without a flag:
#
#   --purge-remote-access   remove ZeroTier and cloudflared as well
#   --purge-aes67           remove Merging's kernel module and aes67-daemon as well
#
# --stock is the same question asked once, the way somebody actually asks it:
# "take this box back to something Raspberry Pi OS recognises, but leave my way
# in". It removes the player and the AES67 stack together — the sound stack is
# part of the job the box was doing — and the packages that existed only to
# build the two of them, and it leaves ZeroTier on purpose. Combining it with
# --purge-remote-access is refused rather than silently obeyed, because that
# pair means "clean this box and lock me out of it".
#
# ZeroTier is not merely left alone here: the run ends by checking it is
# enabled, running and has its address, and starting it if something had stopped
# it. A box that has just been cleared and has no way in is worse than one that
# was never cleared at all, and the box being cleared is usually one somebody
# has to reach afterwards.
#
# The legacy Digisynthetic stack is never touched here;
# scripts/player/purge-digisyn.sh is the script for that, and was written as
# the mirror of the vendor installer that put it there.
#
# Safe to run again: every step looks first, reports what it found, and does
# nothing if the thing is already gone. --help, --check and --dry-run change
# nothing and are allowed on a laptop — which is exactly where this wants to be
# read through before anybody drives to a church with it.
set -euo pipefail

# ── What the installer wrote, and where ──────────────────────────────────────
# Every path here is taken from scripts/player/install.sh and
# scripts/player/merging-aes67.sh rather than guessed at, and each is
# overridable so an install that used a different prefix is still covered.
PREFIX="${PREFIX:-/usr/local}"
SRC_DIR="${SRC_DIR:-/opt/multisite-player/src}"

CONFIG_DIR="/etc/multisite-player"
CONFIG="$CONFIG_DIR/config.json"
STATE_DIR="/var/lib/multisite-player"

SERVICE="multisite-player"
UNIT="/etc/systemd/system/$SERVICE.service"
BIN="$PREFIX/bin/multisite-player"
WEB="$PREFIX/share/multisite-player"

# Remote access. ZeroTier keeps its identity and joined networks under
# /var/lib/zerotier-one; cloudflared writes its unit, config and a token.
ZT_SERVICE="zerotier-one"
ZT_STATE="/var/lib/zerotier-one"
CF_DIR="/etc/cloudflared"
CF_KEYRING="/usr/share/keyrings/cloudflare-main.gpg"
CF_SOURCE="/etc/apt/sources.list.d/cloudflared.list"

# The open AES67 stack, from merging-aes67.sh. Its kernel module is built from
# source rather than through DKMS, so there is a .ko under extra/ to find by
# name, and PulseAudio was masked and its binary moved aside to keep it quiet.
AES_PREFIX_BIN="${AES_PREFIX_BIN:-$PREFIX/bin}"
AES_DAEMON="$AES_PREFIX_BIN/aes67-daemon"
AES_WEBUI="$PREFIX/share/aes67-daemon"
AES_CONF="/etc/daemon.conf"
AES_STATUS="/etc/status.json"
AES_SERVICE="aes67-daemon"
AES_UNIT="/etc/systemd/system/$AES_SERVICE.service"
AES_MODULES_LOAD="/etc/modules-load.d/merging-ravenna.conf"
AES_SYSCTL="/etc/sysctl.d/90-aes67.conf"
AES_MODULE="MergingRavennaALSA"
AES_USER="aes67-daemon"
AES_BUILD="${AES_BUILD:-/var/tmp/aes67-merging}"
PA_BIN="/usr/bin/pulseaudio"
PA_BIN_ASIDE="/usr/bin/_pulseaudio"

# The packages that exist only to build the player. The general-purpose ones
# install.sh also installs — build-essential, cmake, pkg-config, git,
# ca-certificates — are deliberately not in this list: they are wanted on a
# hundred other jobs, and removing them to uninstall one program is a trap.
PLAYER_DEV_PACKAGES="
  libcurl4-openssl-dev libssl-dev
  libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libswscale-dev
  libdrm-dev libasound2-dev libqrencode-dev libfreetype-dev"

# The AES67 daemon's own build-only dependencies, from merging-aes67.sh — the
# four in its apt list that nothing else on a Raspberry Pi wants. The others in
# that list (clang, alsa-utils, psmisc, libsystemd-dev and the kernel headers)
# are deliberately not here: they are ordinary things to have on a Linux box,
# and removing them to uninstall one program is the same trap. They are named in
# the closing notes instead, so the choice stays with the operator.
AES67_DEV_PACKAGES="
  libboost-all-dev libavahi-client-dev libfaac-dev linuxptp"

DRY_RUN=0
CHECK_ONLY=0
KEEP_CONFIG=0
KEEP_CACHE=0
PURGE_REMOTE=0
PURGE_AES67=0
PURGE_PACKAGES=0
STOCK=0
ASSUME_YES="${ASSUME_YES:-0}"

# ── Output ───────────────────────────────────────────────────────────────────
say()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
warn() { printf '\033[1;33m    %s\033[0m\n' "$*"; }
die()  { printf '\n\033[1;31mThat did not work:\033[0m %s\n\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

usage() {
    cat <<'EOF'
usage: uninstall.sh [options]

Takes the campus player off a box: its service, unit, program, web page, the
source it was built from, its settings and the downloaded event.

What it removes by default
  multisite-player.service and /etc/systemd/system/multisite-player.service,
  /usr/local/bin/multisite-player, /usr/local/share/multisite-player,
  /opt/multisite-player (the checkout and its build tree),
  /etc/multisite-player, /var/lib/multisite-player and the cache folder named
  in the settings, wherever that points (the USB-SSD case included).

What it never touches
  ZeroTier, cloudflared and the open AES67 stack, unless one of the --purge-*
  options below is given; the legacy Digisynthetic stack, which is
  scripts/player/purge-digisyn.sh; and snd/snd-pcm, which HDMI needs too.

Options
  --keep-config        Keep /etc/multisite-player and the downloaded cache.
                       Do this when the box is going to be reinstalled.
  --keep-cache         Remove everything except the downloaded event cache.
  --stock              Take the player AND the AES67 stack off, and the packages
                       that existed only to build them, and leave ZeroTier on
                       and up. The one-command answer to "put this box back to
                       stock, but keep my way in".
  --purge-remote-access
                       Also remove ZeroTier and cloudflared. This takes the way
                       into the box with it; refused alongside --stock.
  --purge-aes67        Also remove Merging's kernel module, aes67-daemon and
                       their configuration, and put PulseAudio back. This takes
                       the sound off the network. Implied by --stock.
  --purge-packages     Also remove the -dev packages installed only to build
                       the player. General tools such as build-essential are
                       left alone. Implied by --stock.
  --check              Report what is on the box and change nothing.
  --dry-run            Print every change and make none of them.
  -y, --yes            Do not ask for confirmation.
  -h, --help           This text.

Environment
  PREFIX, SRC_DIR, AES_PREFIX_BIN, AES_BUILD, ASSUME_YES
EOF
}

# ── Doing things, or saying what would have been done ────────────────────────
# Every mutating command goes through one of these, so --dry-run is a property
# of the script rather than something each step has to remember. `set -e` is on,
# which is the other reason: a command that would fail on a box already cleaned
# up never runs, and the run does not end on it.
run() {
    if [ "$DRY_RUN" -eq 1 ]; then
        note "would run:  $*"
        return 0
    fi
    "$@"
}

# remove <path> [tree] — pass the literal -r for a directory.
remove() {
    local path="$1" tree="${2:-}"
    if [ ! -e "$path" ] && [ ! -L "$path" ]; then
        note "absent:      $path"
        return 0
    fi
    if [ "$DRY_RUN" -eq 1 ]; then
        note "would remove $path"
        return 0
    fi
    if [ "$tree" = "-r" ]; then
        rm -rf -- "$path"
    else
        rm -f -- "$path"
    fi
    note "removed:     $path"
}

# Several paths at once, where an argument may be a pattern rather than a name.
# The patterns arrive quoted and are expanded here rather than at the call site,
# so nothing depends on the caller's word splitting — and a pattern that matches
# nothing is reported as absent instead of being handed to rm as its own literal
# name, which is what would otherwise try to delete a file called "/etc/apt/…*".
remove_all() {
    local pattern path
    for pattern in "$@"; do
        # shellcheck disable=SC2086  # the expansion is the point: this is a glob
        for path in $pattern; do
            if [ -e "$path" ] || [ -L "$path" ]; then
                remove "$path" -r
            else
                note "absent:      $pattern"
            fi
        done
    done
}

# True if any of these patterns matches something on disk. compgen does the
# expansion itself, so a pattern can be passed quoted and still work.
any_exists() {
    local pattern
    for pattern in "$@"; do
        if compgen -G "$pattern" >/dev/null 2>&1; then return 0; fi
    done
    return 1
}

# A directory this script is willing to remove recursively. The paths it removes
# come from the environment and from the player's own settings, so the guard is
# here: an absolute path with at least two components, and never the root. The
# installer's defaults all pass; a typo or a settings file with cache_dir "/"
# does not, and is refused rather than obeyed.
sane_tree() {
    case "$1" in
        ""|"/") return 1 ;;
        /*[!/]/*) return 0 ;;
        *) return 1 ;;
    esac
}

confirm() {
    [ "$ASSUME_YES" -eq 1 ] && return 0
    if [ ! -r /dev/tty ]; then
        warn "there is no terminal here to ask on, and -y was not given."
        die "run again with -y to accept this, or --check to look first"
    fi
    local answer=""
    printf '    %s [y/N]: ' "$1" > /dev/tty
    IFS= read -r answer < /dev/tty || answer=""
    case "$answer" in
        y|Y|yes|YES) return 0 ;;
        *) return 1 ;;
    esac
}

# ── Reading the box ──────────────────────────────────────────────────────────
# One setting out of the player's JSON, read the same way the installer and the
# interface do. Empty when there is no config, no python3, or a file that will
# not parse — all three of which mean the same thing here: do not invent an
# answer from a file we cannot read.
config_value() {
    local key="$1"
    [ -f "$CONFIG" ] || return 0
    have python3 || return 0
    python3 - "$CONFIG" "$key" <<'PY' 2>/dev/null || true
import json, sys
try:
    with open(sys.argv[1]) as f:
        cfg = json.load(f)
except Exception:
    sys.exit(0)
v = cfg.get(sys.argv[2], "")
if isinstance(v, (str, int, float)):
    print(v)
PY
}

while [ $# -gt 0 ]; do
    case "$1" in
        --keep-config)         KEEP_CONFIG=1; shift ;;
        --keep-cache)          KEEP_CACHE=1; shift ;;
        --stock)               STOCK=1; shift ;;
        --purge-remote-access) PURGE_REMOTE=1; shift ;;
        --purge-aes67)         PURGE_AES67=1; shift ;;
        --purge-packages)      PURGE_PACKAGES=1; shift ;;
        --check)               CHECK_ONLY=1; shift ;;
        --dry-run)             DRY_RUN=1; shift ;;
        -y|--yes)              ASSUME_YES=1; shift ;;
        -h|--help)             usage; exit 0 ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
done

# --stock is the whole job asked in one word: the player, the AES67 stack under
# it, and the packages that existed only to build the two of them. The one
# combination it refuses is the one that would clean the box and lock somebody
# out of it — a box being returned to stock is very often a box at the back of a
# hall that nobody is standing next to, and the run is being watched from the
# office over ZeroTier.
if [ "$STOCK" -eq 1 ]; then
    if [ "$PURGE_REMOTE" -eq 1 ]; then
        die "--stock leaves ZeroTier on and up on purpose, and --purge-remote-access removes it — give one or the other, not both"
    fi
    PURGE_AES67=1
    PURGE_PACKAGES=1
fi

# ── Preflight ────────────────────────────────────────────────────────────────
# Only for a run that is going to change something. --help and --check are read
# only, and --dry-run changes nothing, so all three are allowed on a laptop.
if [ "$CHECK_ONLY" -eq 0 ] && [ "$DRY_RUN" -eq 0 ]; then
    if [ "$(uname -s)" != "Linux" ]; then
        die "this is for the player, which runs Linux; $(uname -s) is not that"
    fi
    if [ "$(id -u)" -ne 0 ]; then
        die "run this with sudo — it removes files under /etc, /usr and /var"
    fi
fi

# Read before anything is removed: once /etc/multisite-player is gone there is
# nothing left to read these from, and both decide what else to take with us.
CACHE_DIR="$(config_value cache_dir)"
ZT_NETWORK_ID="$(config_value zerotier_network_id)"

# ═════════════════════════════════════════════════════════════════════════════
# What is on the box now
# ═════════════════════════════════════════════════════════════════════════════
say "What is on this box before anything is changed"

# Every item the installer could have left, reported as found or not, so the
# whole picture is on the screen before anything is agreed to — and so the same
# list going quiet afterwards is the evidence that it worked.
present_file() { [ -e "$1" ] || [ -L "$1" ]; }

report_item() {
    if present_file "$2"; then
        note "found:  $1"
    else
        note "none:   $1"
    fi
}

# One unit, by name, out of systemctl's list. The list is read into a variable
# rather than piped into grep -q, and that is deliberate: with `pipefail` on,
# `systemctl list-unit-files | grep -q …` can report failure for a unit that is
# right there — grep exits at the match, systemctl takes SIGPIPE, and the
# pipeline's status becomes 141 rather than 0. On a box with a long enough unit
# list that would have this script decide the player's unit is already gone, or
# skip putting a stopped ZeroTier back up, which is the one thing it must not do.
unit_known() {
    have systemctl || return 1
    local units
    units="$(systemctl list-unit-files 2>/dev/null || true)"
    grep -q "^$1\.service" <<<"$units"
}
service_known()     { unit_known "$SERVICE"; }
aes_service_known() { unit_known "$AES_SERVICE"; }
zt_service_known()  { unit_known "$ZT_SERVICE"; }
aes_module_loaded() { lsmod 2>/dev/null | grep -qE "^$AES_MODULE\b"; }
zt_installed()      { have zerotier-cli || present_file "$ZT_STATE"; }
cf_installed()      { have cloudflared || present_file "$CF_DIR"; }

# The address ZeroTier gives its own interface, read rather than asked for: the
# interface is named after ZeroTier itself, and asking the daemon would want its
# authtoken. This is the same read the player's identity screen does.
zt_address() {
    have ip || return 0
    ip -4 -o addr show 2>/dev/null \
        | awk '$2 ~ /^zt/ {split($4,a,"/"); print a[1]; exit}' || true
}

# One file per network the box has joined, under the daemon's state directory,
# each named for the network's sixteen hex digits. The daemon also keeps
# "<id>.local.conf" in there, which is why the name is checked and not just the
# extension.
zt_networks() {
    local file name out=""
    for file in "$ZT_STATE"/networks.d/*.conf; do
        [ -e "$file" ] || continue
        name="$(basename "$file")"
        case "$name" in
            *.local.conf) continue ;;
        esac
        out="$out ${name%.conf}"
    done
    printf '%s' "${out# }"
}

if service_known; then
    note "found:  the $SERVICE service"
else
    note "none:   the $SERVICE service"
fi
report_item "$BIN" "$BIN"
report_item "the web page, $WEB" "$WEB"
report_item "the source and its build tree, $SRC_DIR" "$SRC_DIR"
report_item "the settings, $CONFIG_DIR" "$CONFIG_DIR"
report_item "the state directory, $STATE_DIR" "$STATE_DIR"
if [ -n "$CACHE_DIR" ]; then
    note "player: the cache is set to $CACHE_DIR"
fi

if zt_installed; then
    note "found:  ZeroTier$( [ -n "$ZT_NETWORK_ID" ] && printf ' (joined %s)' "$ZT_NETWORK_ID" )"
else
    note "none:   ZeroTier"
fi
if cf_installed; then
    note "found:  cloudflared"
else
    note "none:   cloudflared"
fi

# Named so the operator can see the script has noticed the other stack and is
# leaving it where it is — or, with --purge-aes67, that it is about to go.
if aes_service_known || present_file "$AES_DAEMON"; then
    if [ "$PURGE_AES67" -eq 1 ]; then
        note "found:  the open AES67 stack — --purge-aes67 will remove it"
    else
        note "found:  the open AES67 stack — left alone"
    fi
else
    note "none:   the open AES67 stack"
fi
if [ -f "$AES_CONF" ]; then
    note "found:  $AES_CONF"
fi
if aes_module_loaded; then
    note "found:  the $AES_MODULE module is loaded"
fi

# The one thing this script was written next to and must not silently undo.
if lsmod 2>/dev/null | grep -q '^Digisyn'; then
    note "legacy: Digisynthetic's module is loaded — removed by purge-digisyn.sh,"
    note "        not by this script"
fi

if [ "$CHECK_ONLY" -eq 1 ]; then
    say "Check only — nothing was changed"
    note "Run again without --check to remove the player."
    exit 0
fi

# ── What is about to happen ──────────────────────────────────────────────────
echo
say "About to remove the campus player"
note "$SERVICE, $BIN, $WEB, $SRC_DIR"
if [ "$KEEP_CONFIG" -eq 1 ]; then
    note "$CONFIG_DIR and the cache are kept (--keep-config)"
else
    note "$CONFIG_DIR and $STATE_DIR"
    if [ -n "$CACHE_DIR" ] && [ "$CACHE_DIR" != "$STATE_DIR/cache" ]; then
        note "and the cache at $CACHE_DIR"
    fi
fi
if [ "$KEEP_CACHE" -eq 1 ]; then
    note "the downloaded event cache is kept (--keep-cache)"
fi
if [ "$STOCK" -eq 1 ]; then
    warn "--stock: back to stock. The open AES67 stack and the packages that only"
    warn "existed to build these two go as well, and ZeroTier is the exception —"
    warn "it stays installed, enabled and up, and is checked at the end."
fi
if [ "$PURGE_REMOTE" -eq 1 ]; then
    warn "--purge-remote-access: ZeroTier and cloudflared go as well."
fi
if [ "$PURGE_AES67" -eq 1 ]; then
    warn "--purge-aes67: the open AES67 stack goes as well. The sound leaves the"
    warn "network with it, and PulseAudio is put back the way it was."
fi
if [ "$PURGE_PACKAGES" -eq 1 ]; then
    warn "--purge-packages: the player's -dev packages go as well."
fi
echo
if ! confirm "Remove it?"; then
    say "Nothing was changed"
    exit 0
fi

# ═════════════════════════════════════════════════════════════════════════════
# Taking it off
# ═════════════════════════════════════════════════════════════════════════════

# Run a command with its chatter suppressed, but still say what it would have
# been under --dry-run. It never fails the run: these are all best-effort
# cleanups whose state is checked again at the end anyway.
try() {
    if [ "$DRY_RUN" -eq 1 ]; then
        note "would run:  $*"
        return 0
    fi
    "$@" >/dev/null 2>&1 || true
}

apt_purge() {
    local pkgs="$1"
    if ! have apt-get; then
        note "apt:         not here, so no packages were removed"
        return 0
    fi
    # shellcheck disable=SC2086  # the list is deliberately word-split into names
    try apt-get remove -y --purge $pkgs
    note "apt:         asked apt to purge the packages named above"
}

say "Stopping the player"
if have systemctl; then
    if systemctl is-active --quiet "$SERVICE" 2>/dev/null; then
        run systemctl stop "$SERVICE"
        note "stopped:     $SERVICE"
    else
        note "not running: $SERVICE"
    fi
    if systemctl is-enabled --quiet "$SERVICE" 2>/dev/null; then
        run systemctl disable "$SERVICE" || \
            warn "could not disable it; removing its unit file below anyway"
        note "disabled:    $SERVICE"
    else
        note "not enabled: $SERVICE"
    fi
    # A unit left in a failed state keeps being mentioned until this is done.
    if [ "$DRY_RUN" -eq 0 ]; then
        systemctl reset-failed "$SERVICE" >/dev/null 2>&1 || true
    fi
else
    note "no systemd here, so there is no service to stop"
fi

# systemd only knows about the copy it started. One run by hand from a terminal
# is not its to stop, and removing the binary underneath it changes nothing
# until that process exits — so say so rather than pretend the box is clear.
if have pgrep && pgrep -x multisite-player >/dev/null 2>&1; then
    warn "a multisite-player process is still running (started by hand?)."
    warn "It keeps the old program until it exits; stop it yourself, or reboot."
fi

say "Removing the service"
remove "$UNIT"
if have systemctl; then
    run systemctl daemon-reload
    note "systemd reloaded"
fi

say "Removing the program"
remove "$BIN"
remove "$WEB" -r
if sane_tree "$SRC_DIR"; then
    remove "$SRC_DIR" -r
else
    warn "refusing to remove '$SRC_DIR' — that is not a directory path under a root"
fi

# The checkout's parent was created only to hold it, so it goes too — but only
# if it is now empty: anything else in there belongs to somebody else.
SRC_PARENT="$(dirname "$SRC_DIR")"
if [ -d "$SRC_PARENT" ]; then
    if [ "$DRY_RUN" -eq 1 ]; then
        note "would remove $SRC_PARENT if it is empty"
    elif rmdir "$SRC_PARENT" 2>/dev/null; then
        note "removed:     $SRC_PARENT (empty)"
    else
        note "kept:        $SRC_PARENT (not empty — something else is in it)"
    fi
fi

say "Removing the settings and the downloaded event"
if [ "$KEEP_CONFIG" -eq 1 ]; then
    note "--keep-config: $CONFIG_DIR and the cache are untouched."
else
    remove "$CONFIG_DIR" -r
    if [ "$KEEP_CACHE" -eq 1 ]; then
        if [ -n "$CACHE_DIR" ]; then
            note "--keep-cache: $CACHE_DIR is untouched."
        else
            note "--keep-cache: the downloaded event is untouched."
        fi
    else
        # The cache the installer chose is $STATE_DIR/cache, but a USB SSD moves
        # it to <mount>/multisite-player/cache. Only ever remove a path the
        # player's own settings named, and never one that holds the state dir.
        case "$CACHE_DIR" in
            ""|"$STATE_DIR"|"$STATE_DIR"/*)
                note "the cache is inside $STATE_DIR, removed with it" ;;
            *)
                if sane_tree "$CACHE_DIR"; then
                    remove "$CACHE_DIR" -r
                else
                    warn "refusing to remove cache_dir '$CACHE_DIR' — that is not a"
                    warn "directory path under a root; remove it yourself if it is real"
                fi ;;
        esac
        remove "$STATE_DIR" -r
    fi
fi

# ── Remote access, only when asked ───────────────────────────────────────────
if [ "$PURGE_REMOTE" -eq 1 ]; then
    say "Removing remote access"

    # cloudflared knows how to take its own service back down, and doing it that
    # way is the only way to be sure the unit, the token and the config go
    # together rather than leaving a tunnel that points at nothing.
    if have cloudflared; then
        try cloudflared service uninstall
        note "cloudflared: asked it to uninstall its service"
    else
        note "cloudflared: not installed"
    fi
    remove "$CF_DIR" -r
    remove "$CF_KEYRING"
    remove "$CF_SOURCE"
    apt_purge cloudflared

    # Leave the network before the identity is removed: a member that vanishes
    # without leaving lingers in the controller's list until it ages out.
    if have zerotier-cli && [ -n "$ZT_NETWORK_ID" ]; then
        try zerotier-cli leave "$ZT_NETWORK_ID"
        note "zerotier:    left $ZT_NETWORK_ID"
    fi
    if have systemctl; then
        if systemctl is-active --quiet "$ZT_SERVICE" 2>/dev/null; then
            run systemctl stop "$ZT_SERVICE"
            note "stopped:     $ZT_SERVICE"
        fi
        if systemctl is-enabled --quiet "$ZT_SERVICE" 2>/dev/null; then
            run systemctl disable "$ZT_SERVICE" || true
            note "disabled:    $ZT_SERVICE"
        fi
    fi
    remove "$ZT_STATE" -r
    remove_all "/etc/apt/sources.list.d/zerotier*.list"
    remove_all "/usr/share/keyrings/zerotier*.gpg" "/etc/apt/keyrings/zerotier*.gpg"
    apt_purge zerotier-one
fi

# ── The open AES67 stack, only when asked ────────────────────────────────────
if [ "$PURGE_AES67" -eq 1 ]; then
    say "Removing the open AES67 stack"

    if have systemctl; then
        if systemctl is-active --quiet "$AES_SERVICE" 2>/dev/null; then
            run systemctl stop "$AES_SERVICE"
            note "stopped:     $AES_SERVICE"
        else
            note "not running: $AES_SERVICE"
        fi
        if systemctl is-enabled --quiet "$AES_SERVICE" 2>/dev/null; then
            run systemctl disable "$AES_SERVICE" || true
            note "disabled:    $AES_SERVICE"
        fi
    fi
    remove "$AES_UNIT"
    if have systemctl; then
        run systemctl daemon-reload
    fi

    remove "$AES_DAEMON"
    remove "$AES_WEBUI" -r
    # The installer keeps a .bak when it rewrites a configuration, so both go.
    remove "$AES_CONF"
    remove "$AES_CONF.bak"
    remove "$AES_STATUS"
    remove "$AES_MODULES_LOAD"
    remove "$AES_SYSCTL"
    if sane_tree "$AES_BUILD"; then
        remove "$AES_BUILD" -r
    else
        warn "refusing to remove '$AES_BUILD' — that is not a directory path under a root"
    fi

    say "Unloading $AES_MODULE"
    if aes_module_loaded; then
        if [ "$DRY_RUN" -eq 1 ]; then
            note "would unload $AES_MODULE"
        elif modprobe -r "$AES_MODULE" 2>/dev/null; then
            note "unloaded:    $AES_MODULE"
        else
            warn "could not unload $AES_MODULE — something still has its card open."
            warn "Its files are removed either way, so it goes at the next reboot."
        fi
    else
        note "not loaded:  $AES_MODULE"
    fi

    say "Removing the installed module"
    # Built from source rather than through DKMS, so the .ko is found by name
    # under extra/ — which is also why there is no dkms step to undo here.
    _found_ko=0
    for _ko in /lib/modules/*/extra/"$AES_MODULE".ko; do
        [ -e "$_ko" ] || continue
        _found_ko=1
        remove "$_ko"

        # depmod has to be rerun for the kernel whose tree we just changed, and
        # only for that one — depmod -a with no argument rebuilds only the
        # running one.
        _kern="${_ko#/lib/modules/}"
        _kern="${_kern%%/*}"
        if [ "$DRY_RUN" -eq 1 ]; then
            note "would run:  depmod -a $_kern"
        elif depmod -a "$_kern" >/dev/null 2>&1; then
            note "depmod:      $_kern"
        else
            warn "depmod -a $_kern failed — the module may still be listed by name"
        fi

        _extra="$(dirname "$_ko")"
        if [ "$DRY_RUN" -eq 0 ] && rmdir "$_extra" 2>/dev/null; then
            note "removed:     $_extra (empty)"
        fi
    done
    [ "$_found_ko" -eq 1 ] || note "no installed .ko found under /lib/modules/*/extra"

    # The AES67 installer masked PulseAudio and, if it would not stay stopped,
    # moved the binary aside. Both are put back, because a box without the
    # AES67 stack is a box that wants its ordinary sound back.
    say "Putting PulseAudio back"
    if have systemctl; then
        if [ "$DRY_RUN" -eq 1 ]; then
            note "would run:  systemctl unmask pulseaudio.socket pulseaudio.service"
        else
            systemctl unmask pulseaudio.socket pulseaudio.service >/dev/null 2>&1 || true
            note "unmasked:    pulseaudio.socket and pulseaudio.service"
        fi
    fi
    if [ -e "$PA_BIN_ASIDE" ]; then
        if [ "$DRY_RUN" -eq 1 ]; then
            note "would move $PA_BIN_ASIDE back to $PA_BIN"
        else
            mv "$PA_BIN_ASIDE" "$PA_BIN" && note "restored:    $PA_BIN"
        fi
    else
        note "absent:      $PA_BIN_ASIDE (nothing was moved aside)"
    fi

    if have userdel && id "$AES_USER" >/dev/null 2>&1; then
        try userdel "$AES_USER"
        note "user:        removed $AES_USER"
    else
        note "user:        $AES_USER is not here"
    fi
fi

# ── Build packages, only when asked ──────────────────────────────────────────
if [ "$PURGE_PACKAGES" -eq 1 ]; then
    say "Removing the packages installed only to build"
    apt_purge "$PLAYER_DEV_PACKAGES"
    if [ "$STOCK" -eq 1 ]; then
        # --stock: the AES67 daemon's own build-only dependencies go with it.
        apt_purge "$AES67_DEV_PACKAGES"
    fi
    note "build-essential, cmake, pkg-config, git and ca-certificates are left"
    note "alone on purpose — they are not only the player's."
fi

# ═════════════════════════════════════════════════════════════════════════════
# The way back in
# ═════════════════════════════════════════════════════════════════════════════
# The player and the sound stack are gone; the box still has to be reachable.
# ZeroTier is a remote-access tool rather than part of the player, and the box
# being cleaned up is usually one somebody has to get back into — so it is not
# merely left alone. It is checked, and put back up if something had stopped it,
# because "I removed the player and now I cannot reach the box" is the one
# outcome this script must never produce.
ZT_UP=1
say "ZeroTier, left on and up"
if [ "$PURGE_REMOTE" -eq 1 ]; then
    note "not applicable — --purge-remote-access removed it"
elif ! zt_installed; then
    note "ZeroTier is not on this box, so there was nothing here to leave"
else
    if zt_service_known; then
        if systemctl is-enabled --quiet "$ZT_SERVICE" 2>/dev/null; then
            note "enabled:     $ZT_SERVICE starts on power-up"
        elif [ "$DRY_RUN" -eq 1 ]; then
            note "would run:  systemctl enable $ZT_SERVICE"
        elif systemctl enable "$ZT_SERVICE" >/dev/null 2>&1; then
            note "enabled:     $ZT_SERVICE"
        else
            warn "could not enable $ZT_SERVICE — it will not come back on its own"
            warn "after a power cut"
            ZT_UP=0
        fi

        if systemctl is-active --quiet "$ZT_SERVICE" 2>/dev/null; then
            note "running:     $ZT_SERVICE"
        elif [ "$DRY_RUN" -eq 1 ]; then
            note "would run:  systemctl start $ZT_SERVICE"
        elif systemctl start "$ZT_SERVICE" >/dev/null 2>&1; then
            note "started:     $ZT_SERVICE"
        else
            warn "$ZT_SERVICE would not start. What it says:"
            warn "    journalctl -u $ZT_SERVICE -n 40"
            ZT_UP=0
        fi
    fi

    # The address only appears once the member has been authorised in ZeroTier
    # Central, so its absence is reported as waiting rather than as a fault:
    # nothing this run does changes that, and a box that is not authorised was
    # not reachable before the player was removed either.
    _zt_ip="$(zt_address)"
    if [ -n "$_zt_ip" ]; then
        note "address:     $_zt_ip"
    else
        note "address:     none yet — authorise this box in ZeroTier Central and it"
        note "             turns up here. Nothing in this run changed that."
    fi
    _zt_nets="$(zt_networks)"
    if [ -n "$_zt_nets" ]; then
        note "joined:      $_zt_nets"
    elif [ -n "$ZT_NETWORK_ID" ]; then
        note "joined:      $ZT_NETWORK_ID (read from the settings just removed)"
    fi
    note "check it:    sudo zerotier-cli listnetworks"
fi

# ═════════════════════════════════════════════════════════════════════════════
# The same list again, quiet this time
# ═════════════════════════════════════════════════════════════════════════════
if [ "$DRY_RUN" -eq 1 ]; then
    say "Dry run — nothing was changed, so everything above is still on the box"
    note "Run again without --dry-run to do it."
    exit 0
fi

say "Checking the result"
CLEAN=1

# Each of these answers "is it still here?". The label says what it is; the
# command is one of the small predicates above, so the same definitions decide
# this as decided the report at the start.
check_gone() {
    local label="$1"; shift
    if "$@"; then
        printf '  \033[1;31mSTILL HERE\033[0m  %s\n' "$label"
        CLEAN=0
    else
        printf '  \033[1;32mgone\033[0m        %s\n' "$label"
    fi
}

check_gone "the $SERVICE service"  service_known
check_gone "$BIN"                  present_file "$BIN"
check_gone "$WEB"                  present_file "$WEB"
check_gone "$SRC_DIR"              present_file "$SRC_DIR"
if [ "$KEEP_CONFIG" -eq 0 ]; then
    check_gone "$CONFIG_DIR"       present_file "$CONFIG_DIR"
fi
if [ "$KEEP_CONFIG" -eq 0 ] && [ "$KEEP_CACHE" -eq 0 ]; then
    check_gone "$STATE_DIR"        present_file "$STATE_DIR"
    case "$CACHE_DIR" in
        ""|"$STATE_DIR"|"$STATE_DIR"/*) ;;
        *) check_gone "$CACHE_DIR" present_file "$CACHE_DIR" ;;
    esac
fi
if [ "$PURGE_REMOTE" -eq 1 ]; then
    check_gone "ZeroTier"          zt_installed
    check_gone "cloudflared"       cf_installed
    check_gone "$ZT_STATE"         present_file "$ZT_STATE"
fi
if [ "$PURGE_AES67" -eq 1 ]; then
    check_gone "the $AES_SERVICE service" aes_service_known
    check_gone "$AES_DAEMON"       present_file "$AES_DAEMON"
    check_gone "$AES_CONF"         present_file "$AES_CONF"
    check_gone "$AES_MODULE"       aes_module_loaded
    check_gone "/lib/modules/*/extra/$AES_MODULE.ko" \
               any_exists "/lib/modules/*/extra/$AES_MODULE.ko"
fi

echo
if [ "$CLEAN" -eq 1 ]; then
    say "The campus player is gone"
else
    say "Something is still here"
    note "The lines marked STILL HERE are above. A module that would not unload is"
    note "the usual one, and it is not a problem: its files are already gone, so it"
    note "goes at the next reboot. Anything else is worth a second look."
fi

# ── Left alone, on purpose ───────────────────────────────────────────────────
echo
say "Left alone, on purpose"
if [ "$PURGE_REMOTE" -eq 0 ]; then
    if zt_installed; then
        if [ "$ZT_UP" -eq 1 ]; then
            note "ZeroTier — installed, enabled and up, and the only way into this"
            note "box from anywhere else (--purge-remote-access removes it)"
        else
            note "ZeroTier — still installed, but it is not up: see the warning above"
        fi
    fi
    if cf_installed; then note "cloudflared — --purge-remote-access removes it"; fi
fi
if [ "$PURGE_AES67" -eq 0 ] && { aes_service_known || present_file "$AES_DAEMON"; }; then
    note "the open AES67 stack — --purge-aes67 removes it"
fi
note "snd and snd-pcm — the HDMI output needs them too"
note "the legacy Digisynthetic stack — scripts/player/purge-digisyn.sh"
if [ "$STOCK" -eq 1 ]; then
    note "clang, alsa-utils, psmisc, libsystemd-dev and the kernel headers, which"
    note "the AES67 build also wanted: ordinary things to have on a Linux box, and"
    note "for that reason left to you. sudo apt-get autoremove --purge is how to"
    note "discard whatever nothing uses any more."
fi

echo
note "To put the player back: scripts/player/install.sh, documented in"
note "docs/SATELLITE.md."
echo
if [ "$CLEAN" -eq 1 ] && [ "$ZT_UP" -eq 1 ]; then
    exit 0
fi
if [ "$ZT_UP" -eq 0 ]; then
    warn "ZeroTier is not up. That is the one thing this run must not take away"
    warn "with the player: until it is running and has its address, nobody reaches"
    warn "this box except somebody standing next to it."
fi
exit 1
