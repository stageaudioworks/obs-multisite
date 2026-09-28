# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck shell=bash
#
# This box's own AES67 multicast group, sourced by merging-aes67.sh.
#
# Every box used to send to 239.1.0.1, the daemon's sample group, so two on one
# network shared a group and a receiver heard every packet twice. Each now takes
# 239.1.x.y from the last two bytes of the MAC of the card it sends on ($IFACE),
# which stays the same across DHCP changes; two boxes share one about once in
# 65,536. The player works it out the same way (aes67_address_for_mac in
# src/appliance/aes67.h), so the stream the installer sets up is the one the
# player then keeps. 239.1.0.1 only when the card has no MAC to read, and never
# otherwise: a MAC ending 00:01 gets 239.1.0.2.

box_mcast_address() {
    local mac b5 b6
    mac="$(cat "${SYSFS_NET:-/sys/class/net}/$IFACE/address" 2>/dev/null || true)"
    case "$mac" in
        [0-9a-fA-F][0-9a-fA-F]:[0-9a-fA-F][0-9a-fA-F]:[0-9a-fA-F][0-9a-fA-F]:[0-9a-fA-F][0-9a-fA-F]:[0-9a-fA-F][0-9a-fA-F]:[0-9a-fA-F][0-9a-fA-F]) ;;
        *) echo "239.1.0.1"; return ;;
    esac
    b5=$((16#$(echo "$mac" | cut -d: -f5)))
    b6=$((16#$(echo "$mac" | cut -d: -f6)))
    if [ "$b5" -eq 0 ] && [ "$b6" -eq 1 ]; then b6=2; fi
    echo "239.1.$b5.$b6"
}
