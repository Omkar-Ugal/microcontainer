# MicroContainer

MicroContainer is a small, educational Linux container runtime in C. It demonstrates how a process can be placed into Linux namespaces, given a separate root filesystem, constrained with cgroup v2, and launched with fewer privileges. It is a learning project—not a Docker replacement and not a production security boundary.

It deliberately does not implement container escape methods, privilege escalation, exploitation, or security bypasses. It does not perform live host networking changes unless `--network veth` is explicitly requested.

## Current implementation

- `clone(2)` creates a child in new PID, mount, UTS, and network namespaces. The parent gates the child until cgroup placement and optional host-side veth setup complete.
- The child sets its UTS hostname, makes mount propagation private, bind-mounts the selected rootfs, `chroot(2)`s into it, and mounts a fresh `/proc`.
- Before `execve(2)`, it closes inherited non-standard file descriptors, drops the capability bounding set and supplementary groups, changes to UID/GID 65534, clears process capabilities, sets `no_new_privs`, and supplies a small clean environment.
- Optional cgroup v2 `memory.max` and `pids.max` limits are configured before the child is released.
- The default network mode is `none`: the new network namespace has no host interface or external route. Optional `--network veth` creates a temporary host/child veth pair with a point-to-point IPv4 route. It does not enable forwarding, NAT, DNS, or internet access.
- `inspect` lists active runtime metadata under `/run/microcontainer`. Metadata is removed after the container exits.

## Build and test

Requirements: Linux, GCC or Clang with GNU/Linux C library headers, GNU Make, and C11. Runtime namespace operations require a suitable Linux kernel and privileges. `iproute2` is required only for `--network veth`; `debootstrap` is used only by the rootfs helper and privileged integration test.

```sh
make
make test
make debug
make sanitize
make integration  # privileged; see the test prerequisites below
make clean
```

The regular test suite is unprivileged and covers option/configuration parsing, memory-size bounds, rootfs path validation, help, and error paths. `make sanitize` rebuilds the CLI/configuration tests with AddressSanitizer and UndefinedBehaviorSanitizer. The integration test launches an actual namespaced process when run as root on a host that permits the required namespaces, verifies metadata/veth cleanup, and tests cgroup limits when memory and PID controllers are delegated; unavailable host prerequisites are reported as explicit skips.

Create a disposable Debian root filesystem (run only in a test directory you control):

```sh
sudo apt-get install debootstrap iproute2
sudo ./scripts/create-rootfs.sh ./rootfs-test
```

The helper refuses to populate a non-empty destination. The generated rootfs is a minimal test image, not a hardened or trusted base image. Do not point it at a valuable directory.

## Usage

```sh
sudo ./build/microcontainer run ./rootfs /bin/sh
sudo ./build/microcontainer run --hostname test ./rootfs /bin/sh
sudo ./build/microcontainer run --memory 256M --pids 50 ./rootfs /bin/sh
sudo ./build/microcontainer run --network veth ./rootfs /bin/sh
sudo ./build/microcontainer inspect
./build/microcontainer help
```

Options are parsed before the rootfs path. The command must be an absolute path inside the rootfs. Memory suffixes `K`, `M`, and `G` are powers of 1024; an unsuffixed value is bytes. Exit status is the executed command's exit code, or 128 plus the terminating signal. Setup failures return a nonzero status and report the failed operation, errno, and `strerror()` text.

### Why root is required

This implementation does not configure user-namespace UID/GID mappings. The runtime explicitly requires effective UID 0; the kernel must also grant the needed capabilities. `CLONE_NEWNS`, `CLONE_NEWPID`, `CLONE_NEWUTS`, `CLONE_NEWNET`, mount operations, hostname changes, veth creation, and cgroup writes are commonly privileged. Root inside a restricted container may still lack `CAP_SYS_ADMIN`, and WSL, hardened hosts, or restricted CI runners may disable namespaces or cgroups. Failure is reported rather than emulated.

Do not run this on a production host or use an untrusted rootfs. The runtime is intentionally not production-secure.

## Architecture and lifecycle

```text
CLI/configuration
       |
       v
Parent: validate rootfs -> create cgroup -> clone namespaces
       |                                      |
       |                           child waits on start gate
       |                                      |
       +--> attach child to cgroup            |
       +--> optional host veth setup          |
       +--> write active metadata             |
       +--> release gate -------------------->+
                                              |
                               hostname / child veth
                               private mounts / rootfs bind
                               chroot / mount proc
                               UID+GID 65534 / drop caps
                               no_new_privs / execve
                                              |
Parent: waitpid -> collect status -> remove veth, cgroup,
        metadata; child namespace mounts disappear on exit
```

The lifecycle is create/configure, start, execute, monitor, stop (the command exits or receives a signal), then cleanup. `clone()` returns the child's host PID to the parent; inside the new PID namespace, the child sees itself as PID 1. The start gate prevents the child from executing before cgroup membership and requested host networking are configured.

The code is separated into CLI/configuration, logging, lifecycle, namespace, filesystem, networking, cgroup, and security modules under `src/`.

## Linux concepts

### Containers and virtual machines

A container is a set of ordinary host processes with selected kernel namespaces, resource controls, and filesystem views. It shares the host kernel. A virtual machine runs a guest kernel behind a hardware-virtualization boundary and normally has a separate guest OS. Containers start quickly and share kernel resources, but their isolation relies on correct host-kernel configuration and is not equivalent to a VM boundary.

### Namespaces

- **PID (`CLONE_NEWPID`)**: the child becomes PID 1 in its process namespace and sees a separate process-ID view. The parent still tracks it by host PID. PID 1 has special signal/reaping responsibilities; MicroContainer runs the requested command directly as PID 1 and does not provide a full init/supervisor.
- **Mount (`CLONE_NEWNS`)**: mount operations get a separate mount table. The runtime marks `/` recursively private before creating the rootfs bind mount, preventing mount propagation back to the host.
- **UTS (`CLONE_NEWUTS`)**: hostname/domain-name changes apply to the namespace. `--hostname` uses `sethostname(2)`.
- **Network (`CLONE_NEWNET`)**: the child receives an isolated network stack. The default has no connected interface. Optional veth setup places one endpoint in the host and one in the container namespace.

### Root filesystem

The rootfs is an existing directory prepared with the command and its runtime libraries. The runtime resolves it in the parent, rejects `/`, bind-mounts its top-level filesystem in the private mount namespace, then calls `chroot(2)` and changes the current directory to `/`. Nested host mount points are not recursively copied. It mounts a new procfs at the rootfs `/proc` so process listings correspond to the child PID namespace. A bind mount plus chroot is a teaching mechanism, not a complete production rootfs confinement strategy: there is no `pivot_root`, mount allowlist, read-only remount policy, `/dev` construction, or seccomp profile.

### veth networking

```text
Host network namespace                       Container network namespace
10.200.0.1/30  mch<host-pid>  <==== veth ====>  eth0  10.200.0.2/30
                                                     default via 10.200.0.1
```

`--network veth` invokes `iproute2` by absolute executable path, creates a temporary pair, moves its peer into the child namespace, and configures a `/30` route. The host endpoint is deleted during cleanup. This requires host `CAP_NET_ADMIN` and compatible kernel support. No forwarding, NAT, firewall rules, DHCP, DNS, or external connectivity is configured.

### cgroups v2

When a memory or PID limit is requested, the runtime creates `/sys/fs/cgroup/microcontainer/<id>`, writes `memory.max` and/or `pids.max`, then writes the host PID to `cgroup.procs` before releasing the child. The host must expose a writable unified cgroup v2 hierarchy and delegate/enable the requested controllers for this parent cgroup. This runtime does not change `cgroup.subtree_control` or remount cgroups. A failure is fatal for that launch; cleanup attempts to remove the created group.

### Capabilities and identity

After mount and network setup, the child closes inherited descriptors above standard input/output/error, drops the capability bounding set, removes supplementary groups, switches to numeric UID/GID 65534, clears its process capability sets, and enables `PR_SET_NO_NEW_PRIVS`. It receives a minimal `PATH`, `HOME`, `USER`, `LOGNAME`, and `container` environment rather than the host environment. The requested command therefore does not run as root in the container. The rootfs must permit this identity to execute the chosen binary; writable paths such as `/tmp` must be prepared as such by the image builder.

### Docker concepts mapped to this implementation

| Docker/runtime concept | MicroContainer equivalent |
| --- | --- |
| Container CLI/create/start | `microcontainer run` and the `clone()` start gate |
| PID/UTS/mount/network isolation | `clone()` namespace flags |
| Image/rootfs | User-supplied directory from `scripts/create-rootfs.sh` |
| Container entrypoint | Absolute `COMMAND` passed to `execve()` |
| Memory/PID constraints | cgroup v2 `memory.max` and `pids.max` |
| Bridge/veth network | Optional isolated point-to-point veth; no bridge/NAT |
| Runtime metadata | Short-lived files under `/run/microcontainer` |
| Daemon, registry, image layers, orchestration | Not implemented |

## Debugging and inspection

While a long-running command is active, use another host terminal:

```sh
sudo ./build/microcontainer inspect
ps -o pid,ppid,comm -p HOST_PID
sudo lsns -p HOST_PID
sudo readlink /proc/HOST_PID/ns/pid
sudo readlink /proc/HOST_PID/ns/net
sudo cat /proc/HOST_PID/status
sudo cat /proc/HOST_PID/mountinfo
sudo find /sys/fs/cgroup/microcontainer -maxdepth 2 -type f -print
```

Compare the host PID view (`ps -e`) with the container view (`sudo nsenter -t HOST_PID -p -m --fork ps -e`) where `nsenter` is installed. The program inside sees its own PID as 1; host `ps` and `lsns` show the host PID and namespace inode.

For `--network veth`:

```sh
ip link show mchHOST_PID
sudo nsenter -t HOST_PID -n ip address
sudo nsenter -t HOST_PID -n ip route
sudo readlink /proc/HOST_PID/ns/net
```

The host interface should be removed after the process exits. Inspect cgroup files under `/sys/fs/cgroup/microcontainer/` while a limited container is running. Trace startup or a failing syscall with:

```sh
sudo strace -f -e clone,wait4,mount,chroot,sethostname,execve \
  ./build/microcontainer run ./rootfs /bin/sh -c 'sleep 10'
```

Replace `HOST_PID` with the host PID printed by the runtime. The metadata file gives the current PID, rootfs, network mode, resource limits, and state.

## Limitations and safety boundary

- Educational prototype only. It is not hardened, audited, or suitable for hostile workloads.
- Requires root and host capabilities; there is no rootless/user-namespace mode.
- No `pivot_root`, mount allowlist, read-only rootfs, seccomp, Landlock, device policy, syscall filtering, or robust signal-forwarding/init process.
- UID/GID 65534 is fixed; there is no configurable ID mapping or user database management.
- No image format, layered filesystem, container daemon, persistent container database, attach/exec/stop command, or concurrent orchestration.
- Network veth setup assumes iproute2 and a free host interface name. It has no bridge, NAT, firewall, DNS, or connectivity guarantee.
- Cgroup v2 limits depend on controller delegation and host hierarchy policy. The program will not modify the host's controller configuration.
- The project does not claim protection from kernel vulnerabilities, dangerous rootfs contents, or host misconfiguration.

## CI

`.github/workflows/ci.yml` runs strict builds, unprivileged tests, and sanitizer tests. A privileged Linux runner job installs debootstrap/iproute2 and runs the namespace integration test with `sudo`. Restricted CI environments may not permit namespace or cgroup operations; those prerequisites are made explicit by the integration script.
