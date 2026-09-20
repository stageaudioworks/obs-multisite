#!/usr/bin/env bash
#
# merging-aes67.sh — the open AES67 stack for the satellite's sound.
#
#   sudo bash scripts/player/merging-aes67.sh
#
# or, on the box itself, with no checkout to hand:
#
#   curl -fsSL https://raw.githubusercontent.com/stageaudioworks/obs-multisite/main/scripts/player/merging-aes67.sh | sudo bash
#
# Why this exists
# ---------------
# The player needs its sound on the network, and the licensed virtual
# sound cards that would do it are amd64-only and pin a buffer shape of their
# own: eight milliseconds in the kernel, a playback position that is a daemon's
# millisecond counter, and no way for us to choose the multicast address, the
# port or the channel map.
#
# Merging's kernel module is the open half of that story. It registers a
# virtual ALSA card — so the parts of this program that read the card are
# unchanged — and it is a *normal* card: it takes a sane buffer, and its daemon
# speaks SDP, SAP and PTP properly and lets you aim a stream where you like.
# The daemon in this repository replaces Merging's commercial Butler (which is
# amd64-only and licensed) with a GPL process that talks to the same kernel
# module over netlink. That is why this runs on a Pi at all.
#
# What it does, and what it does not
# ----------------------------------
# It builds and installs the module and the daemon, writes their configuration,
# and stops here. It does NOT repoint the player: the new card sits next to
# whatever the box already had and the operator chooses with one line of the
# player's config. That is deliberate — this is being tried on the production
# Pi, with no spare, and the working installation has to survive a failed
# experiment. `--point-player` moves the player onto the new card when you ask
# for it, and prints how to put it back.
#
# What is genuinely uncertain, said plainly
# -----------------------------------------
#   1. NOBODY HAS RUN THIS YET. Every command below is taken from the project's
#      own build.sh, debian-packages.sh, daemon.conf, systemd unit and DEVICES.md
#      rather than invented, and the Pi 5 is close to the NanoPi NEO2 the
#      project cites as working — but close is not the same as tested. Expect to
#      hit something.
#
#   2. PTP NEEDS A MASTER. This daemon is a slave; it does not hand out the
#      clock. If nothing on the campus network is a PTP master — a Dante device,
#      a console, an Anubis — the daemon will never report "locked" and no audio
#      will flow. That is the most likely reason for silence after a clean
#      install, and it is a network question, not a bug in this script.
#
#   3. DANTE WANTS THE ROUTING DONE BY HAND. A source shows up in Dante
#      Controller, but connecting it to a receiver is a manual step in that
#      application. The notes at the end walk through it.
#
#   4. THE MODULE IS BUILT FROM SOURCE AGAINST THIS KERNEL. A kernel upgrade
#      means rebuilding it. It is not put through DKMS here because its own
#      build takes a branch of the submodule and a compiler choice this script
#      cannot reconstruct reliably from a DKMS hook; rerun this script after a
#      kernel upgrade instead.
#
# Safe to run again: it reuses an existing clone, and an existing daemon.conf is
# left alone unless you pass --rewrite-config.
set -euo pipefail

# ── What this installs, and where ────────────────────────────────────────────
# The project, pinned to master by default. Set MERGING_REF to a tag or commit
# once you have found one that works here, so a fresh install is not a moving
# target.
MERGING_URL="${MERGING_URL:-https://github.com/bondagit/aes67-linux-daemon.git}"
MERGING_REF="${MERGING_REF:-master}"
BUILD_DIR="${BUILD_DIR:-/var/tmp/aes67-merging}"

# The daemon's CMake has no install target — it builds into daemon/ and has to
# be copied by hand. Same for the module.
DAEMON_BIN_NAME="aes67-daemon"
PREFIX_BIN="${PREFIX_BIN:-/usr/local/bin}"
DAEMON="$PREFIX_BIN/$DAEMON_BIN_NAME"
# The daemon's own default, from its main.cpp: --config defaults to this path.
DAEMON_CONF="/etc/daemon.conf"
# The project's released WebUI archive holds a `dist/` directory at its root,
# so it is extracted here and `http_base_dir` is pointed at the absolute path
# rather than the `../webui/dist` the sample config uses, which only resolves
# when the daemon is started from inside the source tree.
WEBUI_ROOT="/usr/local/share/aes67-daemon"
WEBUI_DIST="$WEBUI_ROOT/dist"
DAEMON_SERVICE="aes67-daemon"
DAEMON_USER="aes67-daemon"

# Which Ethernet port the daemon binds to and advertises streams on. eth0 is
# where this box's AES67 traffic already lives (the vendor daemon's config says
# bindIf=eth0 and the player reaches the room over it), so it is the default.
IFACE="${IFACE:-eth0}"

# The daemon's WebUI port. The project's sample config uses 8080, and that is
# exactly what the player's own operator interface already uses on this box
# (config.h: web_port = 8080), so a straight copy of their config would leave
# two processes fighting for one port and one of them refusing to start. 8081
# keeps the daemon's interface reachable next to the player's. Set WEBUI_PORT
# to move it again if 8081 is taken on some other install.
WEBUI_PORT="${WEBUI_PORT:-8081}"

# The card the module registers. Everything the operator sees refers to it by
# this name, and the player's config takes the plughw: form so the plug layer
# converts the decoder's floats to the L24 the card takes.
CARD_NAME="${CARD_NAME:-RAVENNA}"
PLAYER_CONF="/etc/multisite-player/config.json"

# Whether to move the player onto the new card, and whether to overwrite an
# existing daemon.conf. Both default to leaving things alone.
POINT_PLAYER=0
REWRITE_CONFIG=0
# The stream this script sets up for the player, which is what makes a fresh
# install a finished one: eight channels by default, and the address
# is read out of the daemon's own configuration unless it is given here.
NO_SOURCE=0
SOURCE_CHANNELS="${SOURCE_CHANNELS:-8}"

# Where this repository's raw files live, so the closing notes can name a command
# that works on a box that has no checkout. Set RAW_BASE to a fork or a branch
# when testing.
RAW_BASE="${RAW_BASE:-https://raw.githubusercontent.com/stageaudioworks/obs-multisite/main}"

# ── Output ───────────────────────────────────────────────────────────────────
say()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
warn() { printf '\033[1;33m    %s\033[0m\n' "$*"; }
die()  { printf '\n\033[1;31mThat did not work:\033[0m %s\n\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

# ── How much of this box to use at once ──────────────────────────────────────
# This used to be `make -j$(nproc)`, and that is what made the install kill the
# machine. The daemon's heaviest translation units pull in Boost, and each g++
# working on one can hold well over a gigabyte. Four of those at once do not fit
# in a 4 GB Pi, and the failure is not a failed build — the kernel thrashes
# until the box becomes unresponsive and reboots, always at the same percentage.
#
# Memory is the binding constraint here, not cores, so the job count is worked
# out from the memory actually free at the moment the build starts — which on a
# campus box is less than the memory installed, because the player is running.
MEM_PER_JOB_MB="${MEM_PER_JOB_MB:-1200}"

build_jobs() {
    if [ -n "${JOBS:-}" ]; then echo "$JOBS"; return; fi

    local cores avail_kb by_mem
    cores="$(nproc 2>/dev/null || echo 2)"
    avail_kb="$(awk '/^MemAvailable:/ {print $2; exit}' /proc/meminfo 2>/dev/null || echo 0)"

    # No way to tell: one at a time is slow but it finishes.
    case "$avail_kb" in ''|*[!0-9]*) echo 1; return ;; esac
    [ "$avail_kb" -gt 0 ] || { echo 1; return; }

    by_mem=$(( avail_kb / 1024 / MEM_PER_JOB_MB ))
    [ "$by_mem" -lt 1 ] && by_mem=1
    if [ "$by_mem" -lt "$cores" ]; then echo "$by_mem"; else echo "$cores"; fi
}

# A temporary swap file, so that a box which can only manage one compiler at a
# time still has somewhere to put the peak rather than dying at it. It is
# removed again whether the script succeeds, fails or is interrupted: leaving
# swap on the SD card of a box that writes 3 GB an hour of segment cache would
# be trading one wear problem for another.
SWAPFILE=""
SWAPFILE_MB="${SWAPFILE_MB:-2048}"

remove_temp_swap() {
    [ -n "$SWAPFILE" ] || return 0
    swapoff "$SWAPFILE" >/dev/null 2>&1 || true
    rm -f "$SWAPFILE" 2>/dev/null || true
    SWAPFILE=""
}
trap remove_temp_swap EXIT INT TERM

add_temp_swap() {
    local existing_kb
    existing_kb="$(awk '/^SwapTotal:/ {print $2; exit}' /proc/meminfo 2>/dev/null || echo 0)"
    case "$existing_kb" in ''|*[!0-9]*) existing_kb=0 ;; esac
    # Raspberry Pi OS ships a couple of hundred megabytes, which is not enough
    # to matter here. A gigabyte or more already on the box is.
    [ "$existing_kb" -ge 1048576 ] && return 0

    have mkswap || return 0
    local target="${BUILD_DIR%/*}/aes67-build-swap"
    note "adding ${SWAPFILE_MB} MB of temporary swap for the build"
    if ! fallocate -l "${SWAPFILE_MB}M" "$target" 2>/dev/null; then
        dd if=/dev/zero of="$target" bs=1M count="$SWAPFILE_MB" \
           status=none 2>/dev/null || { rm -f "$target"; return 0; }
    fi
    chmod 600 "$target"
    if mkswap "$target" >/dev/null 2>&1 && swapon "$target" >/dev/null 2>&1; then
        SWAPFILE="$target"
        note "it is removed again when this script finishes"
    else
        rm -f "$target"
        warn "could not enable the temporary swap — carrying on without it"
    fi
}

usage() {
    cat <<'USAGE'
Put the open AES67 stack (Merging's kernel module + the aes67-daemon that
replaces their Butler) on this box without disturbing anything already here.

  sudo bash merging-aes67.sh [options]

Options
  --ref <tag|commit>   Build this revision instead of master.
  --build-dir <dir>    Where to clone and build. Default /var/tmp/aes67-merging.
  --rewrite-config     Overwrite /etc/daemon.conf, keeping a .bak.
  --point-player       Also set the player's alsa_device to the new card and
                       restart it. Without this the player is left untouched.
  --no-source          Do not set up a stream. Do this if the box's streams are
                       managed elsewhere, or by hand.
  --channels N         Channels in that stream, default 8.
  --jobs N             Compile N files at once. The default is worked out from
                       the memory free when the build starts, because running
                       one per core is what makes a small Pi lock up. Use 1 if
                       it still does.
  --check              Look at what is already here and report, change nothing.
  -h, --help           This text.

Environment
  MERGING_URL, MERGING_REF, BUILD_DIR, PREFIX_BIN, RAW_BASE, JOBS,
  MEM_PER_JOB_MB, SWAPFILE_MB

What it changes
  apt packages, /var/tmp/aes67-merging (build tree), /usr/local/bin/aes67-daemon,
  /usr/local/share/aes67-daemon/dist (the WebUI), /etc/daemon.conf,
  /etc/status.json, /etc/systemd/system/aes67-daemon.service,
  /etc/sysctl.d/90-aes67.conf, an /etc/modules-load.d entry so the module
  loads at boot, and one stream — eight channels, at the multicast address the
  daemon's configuration names. While it builds, it may add a temporary swap
  file next to the build tree; that is removed again when the script finishes.

What it never touches
  Any card already registered on the box, and the player's config unless
  --point-player is given.
USAGE
}

CHECK_ONLY=0
while [ $# -gt 0 ]; do
    case "$1" in
        --ref)            MERGING_REF="${2:?--ref needs a value}"; shift 2 ;;
        --build-dir)      BUILD_DIR="${2:?--build-dir needs a value}"; shift 2 ;;
        --rewrite-config) REWRITE_CONFIG=1; shift ;;
        --point-player)   POINT_PLAYER=1; shift ;;
        --no-source)      NO_SOURCE=1; shift ;;
        --channels)       SOURCE_CHANNELS="${2:?--channels needs a value}"; shift 2 ;;
        --jobs)           JOBS="${2:?--jobs needs a value}"; shift 2 ;;
        --check)          CHECK_ONLY=1; shift ;;
        -h|--help)        usage; exit 0 ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
done

# The daemon's own README is blunt about this: it must not run, and killing it
# is not enough because it is respawned immediately.
[ -n "${BASH_VERSION:-}" ] || die "run this with bash, not sh"
[ "$(id -u)" -eq 0 ] || die "run this as root: sudo bash $0"
[ -d /proc/asound ] || die "no /proc/asound — this does not look like a machine with sound support"

# Kernel version, both for the compiler choice below and for the report, so the
# run is self-documenting when it is pasted into a conversation later.
KERNEL_RELEASE="$(uname -r)"
KERNEL_HEADERS="/lib/modules/$KERNEL_RELEASE/build"

# ── What is already here ─────────────────────────────────────────────────────
# Printed before anything is changed, both so a --check run is useful and so a
# real run leaves a record of what state it started from.
report_state() {
    say "What is already on this box"
    note "kernel:      $KERNEL_RELEASE"
    if [ -d "$KERNEL_HEADERS" ]; then
        note "headers:     present ($KERNEL_HEADERS)"
    else
        warn "headers:     missing — the module cannot be built without them"
    fi
    note "arch:        $(uname -m)"

    if [ -f /proc/asound/cards ]; then
        note "sound cards:"
        grep -E '^\s*[0-9]+\s+\[' /proc/asound/cards | sed 's/^/      /' || true
    fi

    # The card this script is trying to add, if a previous run got that far.
    if [ -d "/proc/asound/$CARD_NAME" ]; then
        note "$CARD_NAME:    already registered"
    else
        note "$CARD_NAME:    not registered yet"
    fi
    if lsmod | grep -q '^MergingRavennaALSA'; then
        note "module:      MergingRavennaALSA is loaded"
    else
        note "module:      MergingRavennaALSA not loaded"
    fi
    if [ -x "$DAEMON" ]; then
        note "daemon:      $DAEMON installed"
    else
        note "daemon:      not installed"
    fi
    [ -f "$DAEMON_CONF" ] && note "config:      $DAEMON_CONF exists"

    # PulseAudio fights the daemon for the ALSA devices and the project asks for
    # it to be gone. Reported, and stopped later only if it is running.
    if pgrep -x pulseaudio >/dev/null 2>&1; then
        warn "pulseaudio:  running — the AES67 daemon needs it stopped"
    else
        note "pulseaudio:  not running"
    fi
}

# ── Dependencies ─────────────────────────────────────────────────────────────
# The exact list from the project's debian-packages.sh, minus valgrind (a
# debugging tool, and this is a church appliance) and plus what a Raspberry Pi
# OS image needs to match it: the kernel headers package here is
# raspberrypi-kernel-headers, not linux-headers-$(uname -r), because the Pi's
# kernel is not one of Debian's.
install_deps() {
    say "Installing build dependencies"
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq

    # `linux-sound-base` and `alsa-base` are in the project's own list and are
    # deliberately not here: Debian removed both in Bookworm, and Raspberry Pi
    # OS followed. They were meta-packages that only pulled in alsa-utils and
    # the sound modules, both of which are asked for below or are already in the
    # Pi's kernel. Naming a package apt does not have is not a warning — it
    # fails the entire install, which is how this list first stopped on the Pi.
    local pkgs=(
        build-essential clang git cmake pkg-config
        libboost-all-dev
        alsa-utils libasound2-dev
        linuxptp
        libavahi-client-dev
        libsystemd-dev
        libfaac-dev
        psmisc
        # Used at the end to set up the stream through the daemon's own REST
        # interface. Almost certainly here already — this script is normally
        # fetched with curl — but the one box where it is not would be the one
        # that ends up with a daemon and no stream.
        curl
    )

    # The kernel headers, by whichever name this distribution uses.
    if [ -d "$KERNEL_HEADERS" ]; then
        note "kernel headers already present"
    elif apt-cache show "linux-headers-$KERNEL_RELEASE" >/dev/null 2>&1; then
        pkgs+=("linux-headers-$KERNEL_RELEASE")
    elif apt-cache show raspberrypi-kernel-headers >/dev/null 2>&1; then
        pkgs+=(raspberrypi-kernel-headers)
    else
        warn "no kernel headers package found for $KERNEL_RELEASE"
        note "the module build will fail until headers matching the running"
        note "kernel are installed; on Raspberry Pi OS that is usually"
        note "  sudo apt install raspberrypi-kernel-headers"
    fi

    note "apt: ${pkgs[*]}"

    # Install only what this distribution actually has. A name apt cannot
    # resolve makes the whole transaction fail, so one stale entry from the
    # project's list would stop the install outright — which is exactly what
    # `linux-sound-base` did before it was removed above. Checking first turns
    # that failure into a line in the log.
    local have=() absent=()
    for p in "${pkgs[@]}"; do
        if apt-cache show "$p" >/dev/null 2>&1; then
            have+=("$p")
        else
            absent+=("$p")
        fi
    done
    if [ ${#absent[@]} -gt 0 ]; then
        warn "not in this distribution's repositories, skipping: ${absent[*]}"
    fi
    # `apt-get install` with no names is an error, and under `set -e` that
    # would end the run with a message about apt rather than about the real
    # problem, which is that this distribution has none of what is needed.
    if [ ${#have[@]} -eq 0 ]; then
        die "this distribution has none of the packages the build needs"
    fi

    apt-get install -y -qq "${have[@]}"
    note "dependencies in place"
}

# ── The source ───────────────────────────────────────────────────────────────
# Cloned once and reused. A previous build tree is left as it is rather than
# reset, so a local change made while debugging survives a rerun.
fetch_source() {
    say "Getting the sources"
    if [ -d "$BUILD_DIR/.git" ]; then
        note "reusing $BUILD_DIR"
    else
        note "cloning into $BUILD_DIR"
        mkdir -p "$(dirname "$BUILD_DIR")"
        git clone --recurse-submodules "$MERGING_URL" "$BUILD_DIR"
    fi

    # Checkout the ref, but only when the tree is clean enough to move — a
    # dirty tree is somebody debugging something and is not to be thrown away.
    if [ -n "$MERGING_REF" ]; then
        if [ -z "$(git -C "$BUILD_DIR" status --porcelain)" ]; then
            note "checking out $MERGING_REF"
            git -C "$BUILD_DIR" fetch --all --tags --quiet || true
            git -C "$BUILD_DIR" checkout "$MERGING_REF"
        else
            warn "build tree has local changes — not moving it to $MERGING_REF"
        fi
    fi

    note "updating submodules"
    git -C "$BUILD_DIR" submodule update --init --recursive

    # The driver submodule carries the daemon's own branch of the module. The
    # project's build.sh checks this out explicitly, and the module will not
    # talk to the daemon without it.
    local driver="$BUILD_DIR/3rdparty/ravenna-alsa-lkm/driver"
    [ -d "$driver" ] || die "the ravenna-alsa-lkm submodule is missing — cloning did not complete"
    if git -C "$driver" rev-parse --verify --quiet aes67-daemon >/dev/null; then
        note "module: using the aes67-daemon branch"
        git -C "$driver" checkout aes67-daemon
    else
        warn "no aes67-daemon branch in the submodule — using the default branch"
    fi
}

# ── Kernel parameters ────────────────────────────────────────────────────────
# Three settings the project's README asks for by name. None is optional on a
# current kernel, and none is guesswork:
#
#   igmp_max_memberships      20 per socket by default, and every stream joins a
#                             multicast group. 66 is the project's own value for
#                             its 8-channel test configuration.
#   sched_rt_runtime_us       from kernel 5.10 the round-robin scheduler is
#                             throttled to 95% of the CPU, which makes the
#                             latency test fail. The README raises it to 100%
#                             (their issue 96).
#   perf_cpu_time_max_percent CPU frequency scaling appears as a few seconds of
#                             distortion while the daemon runs; 0 disables it.
apply_sysctl() {
    say "Kernel parameters"
    local conf="/etc/sysctl.d/90-aes67.conf"
    cat > "$conf" <<'EOF'
# Written by multisite's merging-aes67.sh — the settings the AES67 daemon's own
# README requires. Delete this file and reboot to go back to the defaults.
net.ipv4.igmp_max_memberships = 66
kernel.sched_rt_runtime_us = 1000000
kernel.perf_cpu_time_max_percent = 0
EOF
    note "wrote $conf"
    sysctl -p "$conf" >/dev/null 2>&1 || \
        warn "some settings would not take effect now — they will at the next boot"
}

# ── PulseAudio ───────────────────────────────────────────────────────────────
# The README is blunt: it fights the daemon for the ALSA devices, and killing it
# is not enough because it comes straight back. Mask the units first, then the
# process, and only as a last resort use the README's own fallback of renaming
# the binary so nothing can start it.
stop_pulseaudio() {
    say "PulseAudio"
    if ! pgrep -x pulseaudio >/dev/null 2>&1; then
        note "not running — nothing to do"
        return
    fi
    warn "running, and the AES67 daemon needs it gone"
    systemctl mask pulseaudio.socket pulseaudio.service >/dev/null 2>&1 || true
    pkill -x pulseaudio 2>/dev/null || true
    sleep 1
    if pgrep -x pulseaudio >/dev/null 2>&1; then
        warn "still running — renaming the executable so it cannot restart"
        [ -x /usr/bin/pulseaudio ] && mv /usr/bin/pulseaudio /usr/bin/_pulseaudio
        pkill -x pulseaudio 2>/dev/null || true
    fi
    pgrep -x pulseaudio >/dev/null 2>&1 && \
        die "PulseAudio is still running — reboot and run this again"
    note "stopped"
}

# ── The kernel module ────────────────────────────────────────────────────────
# Built in the submodule, on the branch the daemon expects (checked out above in
# fetch_source). The project's build.sh switches to clang on newer kernels — the
# rule it uses compares "618" against 72 for a 6.18 kernel, so in practice that
# means clang for anything modern — and falls back to the system compiler here
# if that fails, because on the production box a compiler argument is not worth
# losing the run over.
build_module() {
    say "Building the kernel module"
    local dir="$BUILD_DIR/3rdparty/ravenna-alsa-lkm/driver"
    [ -d "$dir" ] || die "the driver submodule is missing: $dir"
    cd "$dir"
    make clean >/dev/null 2>&1 || true

    local kver
    kver="$(uname -r | cut -d. -f1,2 | tr -d '.')"
    if [ "${kver:-0}" -ge 72 ] 2>/dev/null; then
        note "kernel $KERNEL_RELEASE — building with clang, as the project's build.sh does"
        if ! make CC=clang; then
            warn "the clang build failed — trying the system compiler instead"
            make clean >/dev/null 2>&1 || true
            make
        fi
    else
        note "kernel $KERNEL_RELEASE — building with the system compiler"
        make
    fi
    [ -f MergingRavennaALSA.ko ] || die "the build finished but produced no MergingRavennaALSA.ko"
    cd - >/dev/null
    note "module built"
}

install_module() {
    say "Installing the kernel module"
    local dir="$BUILD_DIR/3rdparty/ravenna-alsa-lkm/driver"
    local dest="/lib/modules/$KERNEL_RELEASE/extra"
    mkdir -p "$dest"
    install -m 644 "$dir/MergingRavennaALSA.ko" "$dest/MergingRavennaALSA.ko"
    depmod -a "$KERNEL_RELEASE"
    note "installed $dest/MergingRavennaALSA.ko"

    # Load it at boot. A file of its own, so deleting this one path leaves
    # anything else on the box exactly as it was.
    echo "MergingRavennaALSA" > /etc/modules-load.d/merging-ravenna.conf
    note "wrote /etc/modules-load.d/merging-ravenna.conf"

    if lsmod | grep -q '^MergingRavennaALSA'; then
        note "already loaded"
    else
        modprobe MergingRavennaALSA || die "the module would not load — check dmesg"
        note "loaded"
    fi
}

# ── The daemon ───────────────────────────────────────────────────────────────
# Built with the cmake line from the project's own build.sh, with two
# departures, both to remove things this box does not need and cannot afford to
# have fail:
#
#   ENABLE_TESTS=OFF   build.sh turns the googletest suite on and then builds a
#                      separate test binary. Nothing here runs it, and it is one
#                      more thing that can break the build.
#   WITH_NMOS=OFF      NMOS is a media-over-IP *control* protocol (IS-04/IS-05).
#                      It has nothing to do with AES67 audio reaching a Dante
#                      receiver, and it is the component that drags in the
#                      largest extra submodule.
#
# Everything else — the LKM and httplib directories, Avahi, systemd, the
# streamer — is as upstream has it.
build_daemon() {
    say "The WebUI"
    mkdir -p "$WEBUI_ROOT"
    if [ -f "$WEBUI_DIST/index.html" ]; then
        note "already extracted at $WEBUI_DIST"
    else
        # The project's build.sh fetches a prebuilt archive rather than running
        # npm; the same here, so node is not a dependency.
        local tarball="$BUILD_DIR/webui/webui.tar.gz"
        mkdir -p "$(dirname "$tarball")"
        note "downloading the released WebUI"
        wget --timestamping -O "$tarball" \
            "https://github.com/bondagit/aes67-linux-daemon/releases/latest/download/webui.tar.gz" \
            || die "could not download the WebUI release"
        tar -xzf "$tarball" -C "$WEBUI_ROOT"
        [ -f "$WEBUI_DIST/index.html" ] || \
            warn "extracted, but $WEBUI_DIST/index.html is not there — the archive layout may have changed"
        note "extracted to $WEBUI_DIST"
    fi

    say "Building the daemon"
    add_temp_swap
    local jobs; jobs="$(build_jobs)"
    local cores; cores="$(nproc 2>/dev/null || echo 2)"
    if [ "$jobs" -lt "$cores" ]; then
        note "building $jobs at a time, not $cores — there is not enough free"
        note "memory on this box to compile more of it at once safely"
        note "(override with --jobs N if you know better)"
    else
        note "building $jobs at a time"
    fi

    local top="$BUILD_DIR" dir="$BUILD_DIR/daemon"
    [ -d "$dir" ] || die "no daemon directory in the source tree"
    [ -d "$top/3rdparty/cpp-httplib" ] || die "the cpp-httplib submodule is missing"
    cd "$dir"
    make clean >/dev/null 2>&1 || true
    note "cmake (this takes a few minutes on a Pi)"

    # upstream links libfaac and libasound when WITH_STREAMER is on, and CMake
    # treats a library it could not find as a hard error rather than a warning.
    # libfaac has left recent Debian, so ask for it only when it is really here.
    # Nothing is lost: the streamer feeds a file or URL *into* AES67, while the
    # player's audio comes out of the card and needs none of it.
    local streamer=ON
    if ! dpkg -s libfaac-dev >/dev/null 2>&1; then
        streamer=OFF
        warn "libfaac-dev is not installed — building without the daemon's streamer"
        note "that is its file/URL-to-AES67 feature; AES67 output is unaffected"
    fi

    cmake \
        -DBoost_NO_WARN_NEW_VERSIONS=1 \
        -DCPP_HTTPLIB_DIR="$top/3rdparty/cpp-httplib" \
        -DRAVENNA_ALSA_LKM_DIR="$top/3rdparty/ravenna-alsa-lkm" \
        -DENABLE_TESTS=OFF \
        -DWITH_AVAHI=ON \
        -DFAKE_DRIVER=OFF \
        -DWITH_SYSTEMD=ON \
        -DWITH_STREAMER="$streamer" \
        -DWITH_NMOS=OFF \
        . >/dev/null || die "cmake failed — see the output above"
    if ! make -j"$jobs"; then
        # Almost always memory, and the operator cannot tell that from the
        # compiler's own output. Say it plainly and give them the way out.
        warn "the daemon build failed at $jobs job(s) at a time"
        warn "if the box became unresponsive or rebooted, it ran out of memory;"
        warn "re-run with:  sudo bash $0 --jobs 1"
        die "the daemon build failed"
    fi
    [ -f aes67-daemon ] || die "the build finished but produced no aes67-daemon"
    cd - >/dev/null
    note "daemon built"
}

install_daemon() {
    say "Installing the daemon"
    install -m 755 "$BUILD_DIR/daemon/aes67-daemon" "$DAEMON"
    note "installed $DAEMON"

    # The helper scripts the sample config refers to (the PTP status script the
    # WebUI can call). Copied whole if the tree has them, so the config's
    # absolute path below resolves.
    if [ -d "$BUILD_DIR/daemon/scripts" ]; then
        mkdir -p "$WEBUI_ROOT/scripts"
        cp -a "$BUILD_DIR/daemon/scripts/." "$WEBUI_ROOT/scripts/"
        note "installed helper scripts to $WEBUI_ROOT/scripts"
    fi

    # Runs as a system account with no login and no home, but it must be in the
    # `audio` group: the ALSA nodes the module creates belong to root:audio, and
    # the service unit's own DeviceAllow only governs systemd's device policy,
    # not the filesystem permission on /dev/snd/*. The project's own
    # systemd/install.sh does `useradd -g audio` for exactly this reason.
    if id "$DAEMON_USER" >/dev/null 2>&1; then
        note "user $DAEMON_USER already exists"
        adduser "$DAEMON_USER" audio >/dev/null 2>&1 || true
    else
        getent group audio >/dev/null 2>&1 || groupadd --system audio
        useradd --system --gid audio --no-create-home --shell /usr/sbin/nologin \
            "$DAEMON_USER"
        note "created the $DAEMON_USER system user (group audio)"
    fi
}

# ── Configuration ────────────────────────────────────────────────────────────
# The daemon's own systemd/daemon.conf, unchanged except where this box differs
# from the project's reference. Line for line against
# https://github.com/bondagit/aes67-linux-daemon/blob/master/systemd/daemon.conf
#
#   http_base_dir      the project points at /usr/local/share/aes67-daemon/webui/.
#                      This script extracts the released archive to ${WEBUI_ROOT}/dist,
#                      so it points there instead — an absolute path, because the
#                      daemon is started by systemd and the sample's relative
#                      `../webui/dist` only resolves when run from the source tree.
#   status_file        /etc/status.json, as the project's systemd install uses.
#   interface_name     the project uses eth0; so does this box (the vendor
#                      daemon's own config says bindIf=eth0).
#   ptp_status_script  absolute, for the same reason as http_base_dir.
#   streamer_channels  8 — the count a Dante receiver will see advertised.
#
# `playout_delay` is left at the project's 0. It adds latency in whole
# tic-frames, and lip sync is the open question on this box, so the less added
# the easier it is to reason about. Raise it only if network jitter needs it.
write_config() {
    say "Configuration"
    if [ -f "$DAEMON_CONF" ] && [ "$REWRITE_CONFIG" -ne 1 ]; then
        note "$DAEMON_CONF already exists — left alone (--rewrite-config to replace)"
    else
        if [ -f "$DAEMON_CONF" ]; then
            cp -a "$DAEMON_CONF" "$DAEMON_CONF.bak"
            warn "kept the old one as $DAEMON_CONF.bak"
        fi
        cat > "$DAEMON_CONF" <<EOF
{
  "http_port": $WEBUI_PORT,
  "rtsp_port": 8854,
  "http_base_dir": "$WEBUI_DIST/",
  "log_severity": 2,
  "playout_delay": 0,
  "tic_frame_size_at_1fs": 48,
  "max_tic_frame_size": 1024,
  "sample_rate": 48000,
  "rtp_mcast_base": "239.1.0.1",
  "rtp_mcast_base_sec": "239.1.0.1",
  "rtp_port": 5004,
  "rtp_port_sec": 5004,
  "ptp_domain": 0,
  "ptp_dscp": 48,
  "sap_mcast_addr": "239.255.255.255",
  "sap_interval": 30,
  "syslog_proto": "none",
  "syslog_server": "255.255.255.254:1234",
  "status_file": "/etc/status.json",
  "interface_name": "$IFACE",
  "mdns_enabled": true,
  "custom_node_id": "",
  "ptp_status_script": "$WEBUI_ROOT/scripts/ptp_status.sh",
  "streamer_channels": 8,
  "streamer_files_num": 8,
  "streamer_file_duration": 1,
  "streamer_player_buffer_files_num": 1,
  "streamer_enabled": false,
  "auto_sinks_update": true,
  "nmos_enabled": false,
  "nmos_registry_address": "127.0.0.1",
  "nmos_registry_port": 3210,
  "nmos_node_port": 3218,
  "nmos_label": "AES67 Daemon"
}
EOF
        chmod 644 "$DAEMON_CONF"
        note "wrote $DAEMON_CONF"
    fi

    # The daemon rewrites this as streams change; it must exist before first
    # start, because the unit lists it under ReadWritePaths and runs with
    # ProtectSystem=strict.
    if [ ! -f /etc/status.json ]; then
        printf '{\n  "sources": [ ],\n  "sinks": [ ]\n}\n' > /etc/status.json
        note "wrote /etc/status.json"
    fi
    chown "$DAEMON_USER" /etc/status.json 2>/dev/null || true
}

# ── The service ──────────────────────────────────────────────────────────────
# The project's systemd/aes67-daemon.service, verbatim except that the user is
# created here by useradd rather than by sysusers.d, so that reference is
# dropped and SupplementaryGroups=audio is added. The hardening
# (ProtectSystem=strict with two ReadWritePaths, a closed device policy that
# allows only the sound nodes, no new privileges) is upstream's and is kept as
# written — it is what lets a GPL daemon run safely beside the vendor's on a box
# that is doing a job.
install_service() {
    say "The daemon service"
    cat > "/etc/systemd/system/$DAEMON_SERVICE.service" <<EOF
[Unit]
Description=AES67 daemon service
Before=multi-user.target
After=network.target

[Service]
Type=notify
# Will be adjusted by service during startup
WatchdogSec=10
# In case of no IP we wait forever
TimeoutStartSec=0

# The system user created above, which is in the audio group.
User=$DAEMON_USER
SupplementaryGroups=audio
ExecStart=$DAEMON

# Security filters (as upstream).
CapabilityBoundingSet=
DevicePolicy=closed
DeviceAllow=char-alsa
DeviceAllow=/dev/snd/*
LockPersonality=yes
MemoryDenyWriteExecute=yes
NoNewPrivileges=yes
PrivateDevices=no
PrivateMounts=yes
PrivateTmp=yes
PrivateUsers=yes
# interface::get_mac_from_arp_cache() reads from /proc/net/arp
ProcSubset=all
ProtectClock=no
ProtectControlGroups=yes
ProtectHome=yes
ProtectHostname=yes
ProtectKernelLogs=yes
ProtectKernelModules=yes
ProtectKernelTunables=yes
ProtectProc=invisible
ProtectSystem=strict
RemoveIPC=yes
RestrictAddressFamilies=AF_INET AF_NETLINK AF_UNIX
RestrictNamespaces=yes
RestrictRealtime=yes
RestrictSUIDSGID=yes
SystemCallArchitectures=native
SystemCallFilter=~@clock
SystemCallFilter=~@cpu-emulation
SystemCallFilter=~@debug
SystemCallFilter=~@module
SystemCallFilter=~@mount
SystemCallFilter=~@obsolete
SystemCallFilter=~@privileged
SystemCallFilter=~@raw-io
SystemCallFilter=~@reboot
SystemCallFilter=~@resources
SystemCallFilter=~@swap
UMask=077
# Paths matching daemon.conf
ReadWritePaths=$DAEMON_CONF
ReadWritePaths=/etc/status.json

[Install]
WantedBy=multi-user.target
EOF
    note "wrote /etc/systemd/system/$DAEMON_SERVICE.service"
    systemctl daemon-reload
    systemctl enable "$DAEMON_SERVICE" >/dev/null 2>&1 || \
        warn "could not enable the service — it can still be started by hand"

    if systemctl is-active --quiet "$DAEMON_SERVICE"; then
        note "already running — restarting onto the new build"
        systemctl restart "$DAEMON_SERVICE" || \
            warn "restart failed — see: journalctl -u $DAEMON_SERVICE -n 40"
    else
        note "starting"
        systemctl start "$DAEMON_SERVICE" || \
            warn "it did not start — see: journalctl -u $DAEMON_SERVICE -n 40"
    fi
    sleep 1
    if systemctl is-active --quiet "$DAEMON_SERVICE"; then
        note "running"
    else
        warn "not running. The usual reasons, in order:"
        warn "  * PulseAudio is back (see above)"
        warn "  * the module is not loaded: lsmod | grep Merging"
        warn "  * the .ko was built for another kernel: uname -r vs the module"
    fi
}

# ── Pointing the player at the new card ──────────────────────────────────────
# Only when asked. The card's ALSA id is read from /proc/asound/cards rather than
# assumed: the module lets the id be set at load time and the project does not
# fix it to a constant, so the name the player must use is whatever this box
# actually registered.
ravenna_card_id() {
    # The line looks like:  3 [RAVENNA        ]: MergingRavennaALSA - Merging RAVENNA
    awk -F'[][]' -v want="$CARD_NAME" '
        index(tolower($2), tolower(want)) > 0 {
            gsub(/[[:space:]]+$/, "", $2); print $2; exit
        }' /proc/asound/cards 2>/dev/null
}

point_player() {
    say "Pointing the player at the new card"
    [ -f "$PLAYER_CONF" ] || die "$PLAYER_CONF does not exist — is the player installed?"

    local card_id
    card_id="$(ravenna_card_id)"
    if [ -z "$card_id" ]; then
        warn "no card matching '$CARD_NAME' in /proc/asound/cards:"
        sed 's/^/      /' /proc/asound/cards
        die "the module is not registering a card — fix that before moving the player onto it"
    fi
    local device="plughw:CARD=$card_id"
    note "card id '$card_id' -> alsa_device '$device'"

    cp -a "$PLAYER_CONF" "$PLAYER_CONF.bak"
    note "kept the old config as $PLAYER_CONF.bak"

    # The same edit the interface makes, so the file keeps the shape the player
    # reads back.
    python3 - "$PLAYER_CONF" "$device" <<'PY'
import json, sys
path, device = sys.argv[1], sys.argv[2]
with open(path) as f:
    cfg = json.load(f)
cfg["alsa_device"] = device
with open(path, "w") as f:
    json.dump(cfg, f, indent=2)
PY
    note "set alsa_device to $device"

    if systemctl is-active --quiet multisite-player 2>/dev/null; then
        systemctl restart multisite-player
        note "player restarted"
    else
        note "player is not running under systemd — start it when ready"
    fi
    say "To put the player back on the device it was using"
    note "sudo cp $PLAYER_CONF.bak $PLAYER_CONF && sudo systemctl restart multisite-player"
}

# ── The stream ───────────────────────────────────────────────────────────────
# The card on its own transmits nothing. The daemon has to be told what to send
# and where, and that is a source: which ALSA playback channels become the
# stream's channels, at what width, and to which multicast address.
#
# Done here rather than left to the player, because a fresh install should be a
# finished one. Somebody who runs this and walks away should have a box that is
# sending audio, not a box that could be made to — the player will keep the
# source in shape afterwards, but it cannot do that on a box where the source
# has never existed and nobody has opened the interface.
create_source() {
    say "The stream"

    if [ "$NO_SOURCE" -eq 1 ]; then
        note "skipped, because --no-source was given"
        return 0
    fi

    local api="http://127.0.0.1:$WEBUI_PORT/api"

    # The daemon reports itself active before it is serving, so its interface is
    # waited for rather than assumed. Thirty seconds is generous for a process
    # that has already started.
    local tries=0
    while [ "$tries" -lt 30 ]; do
        if curl -fsS --max-time 2 "$api/version" >/dev/null 2>&1; then break; fi
        tries=$((tries + 1))
        sleep 1
    done
    if [ "$tries" -ge 30 ]; then
        warn "the daemon's interface did not answer on port $WEBUI_PORT."
        warn "No stream has been set up. Once the daemon is running, re-run this"
        warn "script and it will be."
        return 0
    fi

    # The address, read out of the daemon's own configuration rather than
    # written down a second time here: one place to change it, and no way for
    # the two to disagree.
    local mcast
    mcast="$(sed -n 's/.*"rtp_mcast_base"[^"]*"\([^"]*\)".*/\1/p' "$DAEMON_CONF" | head -1)"
    mcast="${mcast:-239.1.0.1}"

    local channels="$SOURCE_CHANNELS"
    case "$channels" in
        ''|*[!0-9]*) channels=8 ;;
    esac
    [ "$channels" -ge 1 ] && [ "$channels" -le 64 ] || channels=8

    # The channel map is the first N ALSA playback channels, in order — the map
    # is what decides which channel of the stream carries what.
    local map
    map="$(python3 -c 'import sys;print(",".join(str(i) for i in range(int(sys.argv[1]))))' \
           "$channels" 2>/dev/null || true)"
    if [ -z "$map" ]; then
        map="0,1,2,3,4,5,6,7"
        channels=8
    fi

    # The name is what a console shows in its source list, so it says which box
    # this is rather than what it is running.
    local name
    name="Multisite $(hostname 2>/dev/null || echo player)"

    local body
    body="$(printf '{"id": 0, "enabled": true, "name": "%s", "io": "Audio Device",
 "max_samples_per_packet": 48, "codec": "L24", "address": "%s",
 "ttl": 15, "payload_type": 98, "dscp": 34, "refclk_ptp_traceable": false,
 "map": [%s]}' "$name" "$mcast" "$map" | tr -d '\n')"

    if curl -fsS --max-time 5 -X PUT -H 'Content-Type: application/json' \
            --data "$body" "$api/source/0" >/dev/null 2>&1; then
        note "stream:      source 0, $channels channels, L24, to $mcast"
    else
        warn "the daemon would not take the stream."
        warn "Its own interface, at http://$(hostname -I 2>/dev/null | awk '{print $1}'):$WEBUI_PORT,"
        warn "can be used instead: add a source there with $channels channels."
        return 0
    fi

    # What is actually on the wire, as the daemon publishes it. This is the
    # address and port an operator has to give whatever is receiving — worth
    # printing here, because the alternative is somebody reading it off the
    # daemon's own interface later.
    local sdp addr port
    sdp="$(curl -fsS --max-time 5 "$api/source/sdp/0" 2>/dev/null || true)"
    if [ -n "$sdp" ]; then
        addr="$(printf '%s' "$sdp" | sed -n 's/^c=IN IP4 \([0-9.]*\).*/\1/p' | tail -1)"
        port="$(printf '%s' "$sdp" | sed -n 's/^m=audio \([0-9]*\).*/\1/p' | head -1)"
        if [ -n "$addr" ]; then
            note "publishing:  $addr${port:+:$port}"
        fi
    fi
}


# ── The run ──────────────────────────────────────────────────────────────────
main() {
    report_state
    if [ "$CHECK_ONLY" -eq 1 ]; then
        say "Check only — nothing changed"
        exit 0
    fi

    install_deps
    fetch_source
    apply_sysctl
    stop_pulseaudio
    build_module
    install_module
    build_daemon
    install_daemon
    write_config
    install_service
    create_source

    if [ "$POINT_PLAYER" -eq 1 ]; then
        point_player
    fi

    say "Done"
    note "The open AES67 stack is installed and running."
    echo
    note "The WebUI is at:  http://$(hostname -I 2>/dev/null | awk '{print $1}'):$WEBUI_PORT"
    note "Logs:             journalctl -u $DAEMON_SERVICE -f"
    echo
    note "Is it usable? The WebUI's PTP page should show a master and 'locked'."
    note "No PTP master on the network means no audio — this daemon slaves to the"
    note "clock, it does not hand one out. That is the most likely silence."
    echo
    note "The player's own pages show the same thing, read back from the daemon:"
    note "  Settings > Network audio output   the switch, the address, the width"
    note "  This box > Network audio output   what is actually being sent"
    echo
    if [ "$POINT_PLAYER" -eq 0 ]; then
        note "The player is still on whatever card it was using. When the WebUI"
        note "says the clock is locked, move it with:"
        note "  sudo bash $0 --point-player"
        note "or, if this was piped in from the network and there is no file at"
        note "$0 to re-run:"
        note "  curl -fsSL $RAW_BASE/scripts/player/merging-aes67.sh \\"
        note "    | sudo bash -s -- --point-player"
    fi
    echo
    note "Dante: the source appears in Dante Controller, but the route from it to"
    note "a receiver is made by hand in that application. Multicast, 8 channels,"
    note "1 ms packets — the AES67 defaults this daemon announces."
    echo
    warn "Nothing here has changed the card the player is on. If it fails, run"
    warn "the line above to put the player back where it was."
}

main "$@"

