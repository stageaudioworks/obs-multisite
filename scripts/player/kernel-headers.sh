# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck shell=bash
#
# Which package has the headers for this kernel, sourced by merging-aes67.sh
# (obs-multisite#30). The RAVENNA module is built against them.
#
# Tried in order, and the first apt has is the one:
#
#   linux-headers-<release>      Debian's own kernels, and the Pi's since
#                                Bookworm (linux-headers-6.12.47+rpt-rpi-2712).
#   linux-headers-<branch>       Armbian: headers are named for the branch, not
#                                the release, so 6.1.115-vendor-rk35xx takes
#                                linux-headers-vendor-rk35xx. Only for Armbian's
#                                branch names (vendor, current, edge, legacy),
#                                so nothing else is ever guessed at.
#   raspberrypi-kernel-headers   Raspberry Pi OS before Bookworm.
#
# apt_has is a function so the tests can say what apt would have.

apt_has() { apt-cache show "$1" >/dev/null 2>&1; }

headers_candidates() {   # headers_candidates KERNEL_RELEASE
    local krel="$1"
    echo "linux-headers-$krel"
    case "$krel" in
        [0-9]*-vendor-*|[0-9]*-current-*|[0-9]*-edge-*|[0-9]*-legacy-*)
            echo "linux-headers-${krel#*-}" ;;
    esac
    echo "raspberrypi-kernel-headers"
}

headers_package() {   # headers_package KERNEL_RELEASE -> the package, or nothing
    local p
    for p in $(headers_candidates "$1"); do
        if apt_has "$p"; then echo "$p"; return 0; fi
    done
    return 1
}
