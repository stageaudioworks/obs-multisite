#!/usr/bin/env bash
# Which package has this kernel's headers (merging-aes67.sh, obs-multisite#30).
# On an Armbian vendor kernel they are named for the branch, not the release,
# and the script used to find none, so the RAVENNA module did not build.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source-path=SCRIPTDIR source=../scripts/player/kernel-headers.sh
. "$here/../scripts/player/kernel-headers.sh"

fails=0
check() {  # name, got, want
  if [ "$2" = "$3" ]; then echo "ok    $1"
  else echo "FAIL  $1"; echo "        got:  $2"; echo "        want: $3"; fails=$((fails + 1)); fi
}
APT=""                                      # what apt has, space-separated
apt_has() { case " $APT " in *" $1 "*) return 0 ;; esac; return 1; }
pick() { APT="$2"; headers_package "$1" || echo "(none)"; }

check "Armbian vendor kernel: the branch package" \
      "$(pick 6.1.115-vendor-rk35xx "linux-headers-vendor-rk35xx linux-headers-current-rockchip64")" \
      "linux-headers-vendor-rk35xx"
check "Armbian mainline: its own branch, not another" \
      "$(pick 6.12.17-current-rockchip64 "linux-headers-vendor-rk35xx linux-headers-current-rockchip64")" \
      "linux-headers-current-rockchip64"
check "a release-named package wins where there is one" \
      "$(pick 6.1.115-vendor-rk35xx "linux-headers-6.1.115-vendor-rk35xx linux-headers-vendor-rk35xx")" \
      "linux-headers-6.1.115-vendor-rk35xx"
check "Pi OS Bookworm on: the release's package, as before" \
      "$(pick 6.12.47+rpt-rpi-2712 "linux-headers-6.12.47+rpt-rpi-2712 raspberrypi-kernel-headers")" \
      "linux-headers-6.12.47+rpt-rpi-2712"
check "older Pi OS: raspberrypi-kernel-headers, as before" \
      "$(pick 6.1.21-v8+ "raspberrypi-kernel-headers")" "raspberrypi-kernel-headers"
check "a Pi kernel is never given an Armbian branch name" \
      "$(headers_candidates 6.12.47+rpt-rpi-2712 | tr '\n' ' ')" \
      "linux-headers-6.12.47+rpt-rpi-2712 raspberrypi-kernel-headers "
check "Debian's own: the release's" \
      "$(pick 6.1.0-38-arm64 "linux-headers-6.1.0-38-arm64")" "linux-headers-6.1.0-38-arm64"
check "none available: none, and says so" "$(pick 6.1.115-vendor-rk35xx "")" "(none)"
check "the installer uses it for the install and for --check" \
      "$(grep -c 'headers_package "\$KERNEL_RELEASE"' "$here/../scripts/player/merging-aes67.sh")" "2"

[ "$fails" -eq 0 ] && echo "kernel headers: all passed" || echo "kernel headers: $fails failed"
exit "$fails"
