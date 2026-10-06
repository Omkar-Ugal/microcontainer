# Linux systems concepts learned in MicroContainer

This note follows the code path. System-call failures are intentionally surfaced with the operation, numeric errno, and its human-readable description.

## Process creation and lifetime

### `clone()`

- **What:** creates a child process while selecting Linux namespace flags. `CLONE_NEWPID`, `CLONE_NEWNS`, `CLONE_NEWUTS`, and `CLONE_NEWNET` place it in new namespaces; `SIGCHLD` requests normal child-exit notification.
- **Why:** the new PID namespace applies to children created by the caller; using `clone()` directly makes the first child PID 1 there. A private mount/UTS/network namespace gives subsequent setup calls isolated effects.
- **Arguments:** a child callback, top of a separately allocated child stack, flags, and a pointer to the child context containing rootfs, requested command, and the start-gate pipe.
- **Failures:** unsupported namespaces, missing `CAP_SYS_ADMIN`, seccomp policy, or kernel restrictions commonly yield `EPERM`; resource exhaustion can yield `ENOMEM`/`EAGAIN`.
- **Handling:** the parent logs `clone(namespaces) failed` with errno, frees the stack and closes the gate. It does not pretend isolation succeeded.

### `pipe2()`, `read()`, and `write()`

- **What:** the close-on-exec pipe is a one-byte parent-to-child start gate.
- **Why:** it prevents a race where the child executes before cgroup membership or optional veth setup is complete.
- **Arguments:** `pipe2()` receives two descriptor slots and `O_CLOEXEC`; the child reads exactly one byte and proceeds only for the expected release token.
- **Failures:** descriptor exhaustion, interrupted I/O, or parent setup failure can prevent release.
- **Handling:** interrupted reads are retried. An EOF or invalid token terminates the waiting child; the parent kills and reaps it when setup fails.

### `waitpid()`

- **What:** waits for a specific child and reports its exit or signal status.
- **Why:** the parent monitors the container command, retains its exit status, and does not leave an unreaped process.
- **Arguments:** host PID from `clone()`, status storage, and blocking options.
- **Failures:** signals can interrupt the wait; invalid child state produces a reported error.
- **Handling:** `EINTR` is retried; normal exit codes are preserved, and signal exits become `128 + signal`.

### `execve()`

- **What:** replaces the current process image with a new executable.
- **Why:** after namespace/rootfs/security setup, it starts the requested container command without creating an extra wrapper process. That command is PID 1 in the new PID namespace.
- **Arguments:** an absolute path interpreted from inside the rootfs, its argument vector, and the inherited environment.
- **Failures:** missing executable/interpreter, permissions, or missing dynamic loader/libraries return errors such as `ENOENT` or `EACCES`.
- **Handling:** the child logs `execve(container command)` and exits 127. Setup failures use 125.

## Namespace and hostname operations

### Namespace flags

Namespace creation is selected in the `clone()` flags rather than piecemeal calls to `unshare()`. This ensures the process starts in the intended namespaces before any namespace-specific configuration is performed. A PID namespace does not change the host PID returned to the parent.

### `sethostname()`

- **What:** changes the UTS namespace hostname.
- **Why:** demonstrates per-container host identity through `--hostname`.
- **Arguments:** requested hostname bytes and their length.
- **Failures:** hostname too long, missing UTS namespace, or missing namespace capability.
- **Handling:** logs errno; the container is not started with a misleading hostname.

## Mount and filesystem operations

### `mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL)`

- **What:** changes mount propagation on the namespace root and descendants to private.
- **Why:** prevents the container's later mounts from propagating into the host or peer mount namespaces.
- **Arguments:** a recursive target `/`, private propagation flags, and no filesystem source/type/data.
- **Failures:** missing `CAP_SYS_ADMIN`, locked propagation, or mount namespace restrictions.
- **Handling:** setup aborts before making the rootfs bind mount.

### `mount(rootfs, rootfs, NULL, MS_BIND | MS_REC, NULL)`

- **What:** bind-mounts the selected rootfs directory into the mount namespace.
- **Why:** gives the process a mount-backed filesystem root independent of the host's mount tree.
- **Arguments:** the canonical rootfs path as source and target and recursive bind flags.
- **Failures:** missing mount capability, invalid path, or mount limits.
- **Handling:** reports the failed bind operation. The namespace is discarded with the child.

### `chroot()` and `chdir()`

- **What:** changes the process root directory and working directory.
- **Why:** the command resolves absolute paths against the selected rootfs rather than the host root.
- **Arguments:** `"."` after changing directory into the canonical rootfs, then `"/"` for the new working directory.
- **Failures:** missing `CAP_SYS_CHROOT`, inaccessible root, or a bad current directory.
- **Handling:** aborts launch before exec. `chroot()` alone is not described as a complete security boundary; the process also loses root UID and capabilities, but this prototype still has major documented limitations.

### `mount("proc", "/proc", "proc", ...)`

- **What:** mounts procfs at the rootfs `/proc`.
- **Why:** procfs reflects the PID namespace and supports programs that expect process metadata.
- **Arguments:** proc source/type, rootfs `/proc` target, and `nosuid,nodev,noexec` flags.
- **Failures:** missing target directory, missing mount capability, or unavailable procfs support.
- **Handling:** the test rootfs helper creates the directory; a mount error is explicit and prevents command execution.

## Network configuration

### Network namespace and veth

The new network namespace starts isolated. `--network veth` uses a checked `iproute2` subprocess to create the host veth, move its peer using the child's host PID, configure `10.200.0.1/30` on the host end, then configure `eth0`, loopback, and a default route from inside the child network namespace.

- **Why:** the pair demonstrates that two namespaces can be joined by a virtual point-to-point Ethernet link.
- **Arguments:** each `ip` command is passed as a fixed argv array to `execv()`; no shell is involved.
- **Failures:** missing iproute2, unavailable `CAP_NET_ADMIN`, conflicting interface, or unsupported veth operations.
- **Handling:** any host setup failure stops the waiting child; the parent attempts to delete the host endpoint. Normal exit also deletes it. No bridge, NAT, forwarding, DNS, or internet access is configured.

`fork()`, `execv()`, and `waitpid()` are used for the helper so the runtime can check the utility's exit status. `ip` is resolved only from fixed system locations (`/usr/sbin`, `/sbin`, `/usr/bin`).

## cgroup v2

### Control-file writes

- **What:** writing `memory.max`, `pids.max`, and `cgroup.procs` configures resource limits and membership in a unified cgroup v2 hierarchy.
- **Why:** namespaces isolate views; cgroups account for and constrain resource use.
- **Arguments:** decimal byte or process limits and the host PID, written to files in a per-container cgroup directory.
- **Failures:** no cgroup v2 mount, read-only/non-delegated hierarchy, disabled controllers, permission errors, or invalid values.
- **Handling:** file-open/write/close errors include the operation/path and errno. A requested limit is never silently skipped; launch aborts and cleanup attempts to remove its group.

The runtime does not mount cgroup2 or change `cgroup.subtree_control`. The administrator must provide a delegated hierarchy with controllers enabled. Without a requested limit, no cgroup directory is created.

## Inherited process state

Before setup, the child closes inherited file descriptors above standard input, output, and error (retaining only the start-gate descriptor until it is consumed). It uses Linux `close_range(2)` and falls back to the process descriptor limit. This avoids unintentionally carrying host file handles into the payload. `execve()` receives a small explicit environment instead of the caller's complete host environment. Standard streams remain attached for interactive use.

## Privilege reduction

### `prctl(PR_CAPBSET_DROP)` and `capset()`

- **What:** removes capabilities from the process bounding set and clears effective/permitted/inheritable process capability sets.
- **Why:** the requested program must not inherit the setup capabilities.
- **Arguments:** each bounding capability number, then a capability header and two version-3 capability data words.
- **Failures:** missing `CAP_SETPCAP`, kernel/API mismatch, or capability operation denial.
- **Handling:** any unexpected failure prevents exec. Unsupported capability numbers reported as `EINVAL` are ignored for kernels with a smaller capability range.

Bounding-set reduction happens while the child still has setup privileges. The process capability sets are cleared after changing identity.

### `setgroups()`, `setgid()`, `setuid()`

- **What:** clears supplementary groups and switches to numeric identity 65534.
- **Why:** the payload should not run with UID 0 after mounts and network setup.
- **Arguments:** an empty supplementary-group list followed by GID and UID 65534.
- **Failures:** unavailable privilege, host restrictions, or an invalid operation order.
- **Handling:** each syscall has an explicit error diagnostic and aborts before `execve()`. There is no user namespace or host-to-container ID mapping; root is required.

### `prctl(PR_SET_NO_NEW_PRIVS)`

- **What:** prevents the process and descendants from gaining additional privilege through `execve()`.
- **Why:** it is a defensive default before starting the payload.
- **Arguments:** `PR_SET_NO_NEW_PRIVS`, value 1, and zeroed unused arguments.
- **Failures:** unsupported kernel behavior or a failed prctl.
- **Handling:** reported as an error; the payload is not executed.

## Logging, metadata, and cleanup

The logger prefixes messages with timestamp and `DEBUG`, `INFO`, `WARN`, or `ERROR`. Syscall failures include both `strerror(errno)` and numeric errno. Active state is written to a root-owned mode-0700 `/run/microcontainer` directory as an atomically renamed metadata file. `inspect` validates metadata entries before reading them.

The parent waits for the child, deletes any host veth, removes the per-container cgroup when empty, unlinks metadata, and frees the clone stack. Mounts are private to the mount namespace and disappear when its last process exits. Cleanup failures are warnings rather than being silently hidden.

## Further study

- `man 2 clone`, `man 2 mount`, `man 2 chroot`, `man 2 setns`, `man 7 namespaces`
- `man 7 cgroups`, `man 7 capabilities`, `man 2 prctl`
- Kernel documentation: cgroup v2 and `/proc` namespaces
- Compare this runtime with a mature OCI runtime; do not infer production readiness from this learning implementation.
