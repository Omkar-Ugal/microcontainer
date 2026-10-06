#!/usr/bin/env bash
set -euo pipefail

destination=${1:-./rootfs-test}
suite=${MICROCONTAINER_DEBIAN_SUITE:-bookworm}
mirror=${MICROCONTAINER_DEBIAN_MIRROR:-https://deb.debian.org/debian}

if [[ ${EUID} -ne 0 ]]; then
    echo "create-rootfs.sh requires root for debootstrap; use a disposable test directory" >&2
    exit 1
fi
if ! command -v debootstrap >/dev/null 2>&1; then
    echo "debootstrap is required (Debian/Ubuntu: apt install debootstrap)" >&2
    exit 1
fi
if [[ -e "$destination" && -n $(find "$destination" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null) ]]; then
    echo "refusing to populate non-empty rootfs destination: $destination" >&2
    exit 1
fi
mkdir -p "$destination"
resolved=$(realpath "$destination")
if [[ "$resolved" == / || "$resolved" == /proc || "$resolved" == /sys ]]; then
    echo "unsafe rootfs destination: $resolved" >&2
    exit 1
fi

debootstrap --variant=minbase "$suite" "$resolved" "$mirror"
mkdir -p "$resolved/proc" "$resolved/dev" "$resolved/tmp"
chmod 1777 "$resolved/tmp"
echo "Created disposable Debian $suite rootfs at $resolved"
echo "This rootfs is for local testing; it is not a hardened container image."
