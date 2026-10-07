#!/usr/bin/env bash
set -euo pipefail

binary=${1:?usage: integration.sh BINARY}
if [[ ${EUID} -ne 0 ]]; then
    echo "SKIP: integration requires root, user/mount/PID/UTS/network namespaces and cgroup v2 access"
    exit 0
fi
if ! command -v debootstrap >/dev/null 2>&1; then
    echo "SKIP: debootstrap is required to create a disposable test rootfs"
    exit 0
fi

temporary=$(mktemp -d /tmp/microcontainer-test.XXXXXX)
rootfs="$temporary/rootfs"
cleanup() {
    rm -rf "$temporary"
}
trap cleanup EXIT

"$(dirname "$0")/../scripts/create-rootfs.sh" "$rootfs"
output=$("$binary" run --hostname mc-test "$rootfs" /bin/sh -c \
    'test "$$" -eq 1 && IFS= read -r name </proc/sys/kernel/hostname && test "$name" = mc-test && test -r /proc/1/status && echo isolated-ok' \
    2>"$temporary/namespace.log")
grep -q '^isolated-ok$' <<<"$output"
echo "MicroContainer namespace/filesystem integration test passed"
container_id=$(sed -n 's/.*created container \([^ ]*\) .*/\1/p' "$temporary/namespace.log")
if [[ -z "$container_id" || -e "/run/microcontainer/$container_id.meta" ]]; then
    echo "container metadata was not created/cleaned up as expected" >&2
    exit 1
fi
echo "MicroContainer metadata cleanup test passed"

if command -v ip >/dev/null 2>&1; then
    "$binary" run --network veth "$rootfs" /bin/sh -c \
        'grep -q eth0 /proc/net/dev && grep -q "10.200.0.2" /proc/net/fib_trie' \
        2>"$temporary/veth.log"
    host_interface=$(sed -n 's/.*veth host interface \([^ ]*\) configured.*/\1/p' "$temporary/veth.log")
    if [[ -z "$host_interface" ]] || ip link show "$host_interface" >/dev/null 2>&1; then
        echo "veth host endpoint was not cleaned up" >&2
        exit 1
    fi
    echo "MicroContainer veth integration test passed"
else
    echo "SKIP: iproute2 is required for the veth integration test"
fi

cgroup_parent=/sys/fs/cgroup/microcontainer
   mkdir -p "$cgroup_parent" 2>/dev/null || true
   echo "+memory +pids" > "$cgroup_parent/cgroup.subtree_control" 2>/dev/null || true
if grep -qw memory /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null &&
   grep -qw pids /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null; then
    if ! "$binary" run --memory 64M --pids 32 "$rootfs" /bin/sh -c \
        'grep -q "/microcontainer/mc-" /proc/self/cgroup' 2>"$temporary/cgroup.log"; then
        echo "cgroup test: container run failed:" >&2
        cat "$temporary/cgroup.log" >&2
        exit 1
    fi
    container_id=$(sed -n 's/.*created container \([^ ]*\) .*/\1/p' "$temporary/cgroup.log")
    if [[ -z "$container_id" || -e "/sys/fs/cgroup/microcontainer/$container_id" ]]; then
        echo "cgroup resource configuration or cleanup failed" >&2
        exit 1
    fi
    echo "MicroContainer cgroup integration test passed"
else
    echo "SKIP: memory and pids controllers are not delegated through cgroup v2"
fi
