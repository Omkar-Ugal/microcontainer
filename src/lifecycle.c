#include "microcontainer.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>

#define MC_STATE_DIR "/run/microcontainer"
#define MC_STACK_SIZE (1024U * 1024U)
#define MC_CHILD_FAILURE 125

static volatile sig_atomic_t mc_pending_signal;

typedef struct {
    mc_container container;
    char **command;
    int gate_read;
    int gate_write;
} child_context;

static void record_signal(int signal_number) {
    mc_pending_signal = signal_number;
}

static int install_signal_handlers(struct sigaction previous[2]) {
    const int signals[2] = {SIGINT, SIGTERM};
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = record_signal;
    (void)sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < 2; ++i) {
        if (sigaction(signals[i], &action, &previous[i]) != 0) {
            mc_log_errno(MC_LOG_ERROR, "sigaction(parent)", errno);
            while (i > 0) {
                --i;
                (void)sigaction(signals[i], &previous[i], NULL);
            }
            return -1;
        }
    }
    return 0;
}

static void restore_signal_handlers(const struct sigaction previous[2]) {
    const int signals[2] = {SIGINT, SIGTERM};
    for (size_t i = 0; i < 2; ++i) {
        if (sigaction(signals[i], &previous[i], NULL) != 0) {
            mc_log_errno(MC_LOG_WARN, "sigaction(restore parent)", errno);
        }
    }
}

static int secure_state_directory(void) {
    if (mkdir(MC_STATE_DIR, 0700) != 0 && errno != EEXIST) {
        mc_log_errno(MC_LOG_ERROR, "mkdir(state directory)", errno);
        return -1;
    }
    struct stat info;
    if (lstat(MC_STATE_DIR, &info) != 0 || !S_ISDIR(info.st_mode) ||
        S_ISLNK(info.st_mode) || info.st_uid != 0) {
        mc_log(MC_LOG_ERROR, "state directory must be a root-owned directory: %s", MC_STATE_DIR);
        return -1;
    }
    if ((info.st_mode & 0077) != 0 && chmod(MC_STATE_DIR, 0700) != 0) {
        mc_log_errno(MC_LOG_ERROR, "chmod(state directory)", errno);
        return -1;
    }
    return 0;
}

static int save_metadata(const mc_container *container) {
    if (secure_state_directory() != 0) return -1;
    char temporary[PATH_MAX];
    char target[PATH_MAX];
    int length = snprintf(temporary, sizeof(temporary), "%s/.%s.tmp", MC_STATE_DIR, container->id);
    if (length < 0 || (size_t)length >= sizeof(temporary)) {
        mc_log(MC_LOG_ERROR, "metadata path is too long");
        return -1;
    }
    length = snprintf(target, sizeof(target), "%s/%s.meta", MC_STATE_DIR, container->id);
    if (length < 0 || (size_t)length >= sizeof(target)) {
        mc_log(MC_LOG_ERROR, "metadata path is too long");
        return -1;
    }
    const int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        mc_log_errno(MC_LOG_ERROR, "open(container metadata)", errno);
        return -1;
    }
    FILE *stream = fdopen(fd, "w");
    if (stream == NULL) {
        const int saved_errno = errno;
        (void)close(fd);
        (void)unlink(temporary);
        mc_log_errno(MC_LOG_ERROR, "fdopen(container metadata)", saved_errno);
        return -1;
    }
    const int failed = fprintf(stream,
        "id=%s\npid=%ld\nrootfs=%s\nhostname=%s\nnetwork=%s\nhost_interface=%s\n"
        "memory_limit=%llu\npids_limit=%lu\nstate=%s\ncgroup=%s\n",
        container->id, (long)container->pid, container->rootfs, container->hostname,
        container->network == MC_NETWORK_VETH ? "veth" : "none",
        container->host_interface, (unsigned long long)container->memory_limit,
        container->pids_limit, container->state, container->cgroup_path) < 0;
    if (fflush(stream) != 0 || fsync(fd) != 0) {
        mc_log_errno(MC_LOG_ERROR, "write(container metadata)", errno);
        (void)fclose(stream);
        (void)unlink(temporary);
        return -1;
    }
    if (fclose(stream) != 0) {
        mc_log_errno(MC_LOG_ERROR, "close(container metadata)", errno);
        (void)unlink(temporary);
        return -1;
    }
    if (failed) {
        mc_log(MC_LOG_ERROR, "write(container metadata) failed");
        (void)unlink(temporary);
        return -1;
    }
    if (rename(temporary, target) != 0) {
        mc_log_errno(MC_LOG_ERROR, "rename(container metadata)", errno);
        (void)unlink(temporary);
        return -1;
    }
    return 0;
}

static void remove_metadata(const mc_container *container) {
    char path[PATH_MAX];
    const int length = snprintf(path, sizeof(path), "%s/%s.meta", MC_STATE_DIR, container->id);
    if (length < 0 || (size_t)length >= sizeof(path)) return;
    if (unlink(path) != 0 && errno != ENOENT) {
        mc_log_errno(MC_LOG_WARN, "unlink(container metadata)", errno);
    }
}

static int child_fail(void) {
    _exit(MC_CHILD_FAILURE);
}

static int close_child_fds(int keep_fd) {
    bool fallback = false;
    if (keep_fd > 3 && syscall(SYS_close_range, 3U, (unsigned int)keep_fd - 1U, 0U) != 0) {
        fallback = true;
    }
    const unsigned int close_start = keep_fd >= 3 ? (unsigned int)keep_fd + 1U : 3U;
    if (syscall(SYS_close_range, close_start, UINT_MAX, 0U) != 0) {
        fallback = true;
    }
    if (!fallback) return 0;

    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
        mc_log_errno(MC_LOG_ERROR, "getrlimit(RLIMIT_NOFILE)", errno);
        return -1;
    }
    rlim_t maximum = limit.rlim_cur;
    if (maximum == RLIM_INFINITY) {
        const long open_max = sysconf(_SC_OPEN_MAX);
        if (open_max < 0) {
            mc_log(MC_LOG_ERROR, "cannot determine the descriptor limit for child cleanup");
            return -1;
        }
        maximum = (rlim_t)open_max;
    }
    if (maximum > (rlim_t)INT_MAX) maximum = (rlim_t)INT_MAX;
    for (rlim_t fd = 3; fd < maximum; ++fd) {
        if (fd != (rlim_t)keep_fd) (void)close((int)fd);
    }
    return 0;
}

static int container_child(void *opaque) {
    child_context *context = opaque;
    struct sigaction default_action;
    memset(&default_action, 0, sizeof(default_action));
    default_action.sa_handler = SIG_DFL;
    (void)sigemptyset(&default_action.sa_mask);
    if (sigaction(SIGINT, &default_action, NULL) != 0) {
        mc_log_errno(MC_LOG_ERROR, "sigaction(child SIGINT)", errno);
        return child_fail();
    }
    if (sigaction(SIGTERM, &default_action, NULL) != 0) {
        mc_log_errno(MC_LOG_ERROR, "sigaction(child SIGTERM)", errno);
        return child_fail();
    }
    (void)close(context->gate_write);
    if (close_child_fds(context->gate_read) != 0) return child_fail();
    char release;
    ssize_t count;
    do {
        count = read(context->gate_read, &release, sizeof(release));
    } while (count < 0 && errno == EINTR);
    (void)close(context->gate_read);
    if (count != 1 || release != 'G') return child_fail();

    if (mc_setup_namespaces(&context->container) != 0) return child_fail();
    if (mc_setup_network_child(&context->container) != 0) return child_fail();
    if (mc_setup_filesystem(context->container.rootfs) != 0) return child_fail();

    if (mc_drop_capability_bounding_set() != 0) return child_fail();
    if (setgroups(0, NULL) != 0) {
        mc_log_errno(MC_LOG_ERROR, "setgroups()", errno);
        return child_fail();
    }
    if (setgid(65534) != 0) {
        mc_log_errno(MC_LOG_ERROR, "setgid(65534)", errno);
        return child_fail();
    }
    if (setuid(65534) != 0) {
        mc_log_errno(MC_LOG_ERROR, "setuid(65534)", errno);
        return child_fail();
    }
    if (mc_drop_capabilities() != 0 || mc_set_no_new_privileges() != 0) return child_fail();

    char *const environment[] = {
        "PATH=/usr/sbin:/usr/bin:/sbin:/bin",
        "HOME=/",
        "USER=nobody",
        "LOGNAME=nobody",
        "container=microcontainer",
        NULL
    };
    execve(context->command[0], context->command, environment);
    mc_log_errno(MC_LOG_ERROR, "execve(container command)", errno);
    _exit(127);
}

static void release_child(int fd) {
    if (fd >= 0) {
        const char release = 'G';
        const ssize_t written = write(fd, &release, sizeof(release));
        if (written != (ssize_t)sizeof(release)) {
            mc_log_errno(MC_LOG_WARN, "write(container start gate)", errno);
        }
        (void)close(fd);
    }
}

static void stop_child(pid_t pid) {
    if (pid <= 0) return;
    (void)kill(pid, SIGKILL);
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}

static int child_exit_code(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return MC_CHILD_FAILURE;
}

int mc_run_container(const mc_config *config) {
    if (geteuid() != 0) {
        mc_log(MC_LOG_ERROR,
               "run requires root with CAP_SYS_ADMIN/CAP_SETUID/CAP_SETGID; user-namespace mappings are not implemented");
        return 1;
    }
    mc_container container;
    memset(&container, 0, sizeof(container));
    if (mc_validate_rootfs(config->rootfs, container.rootfs) != 0) return 1;
    (void)snprintf(container.hostname, sizeof(container.hostname), "%s", config->hostname);
    container.memory_limit = config->memory_limit;
    container.pids_limit = config->pids_limit;
    container.network = config->network;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        mc_log_errno(MC_LOG_ERROR, "clock_gettime()", errno);
        return 1;
    }
    (void)snprintf(container.id, sizeof(container.id), "mc-%ld-%lx",
                   (long)getpid(), (unsigned long)now.tv_nsec);

    if (mc_setup_cgroup(&container) != 0) {
        mc_cleanup_cgroup(&container);
        return 1;
    }

    int gate[2];
    if (pipe2(gate, O_CLOEXEC) != 0) {
        mc_log_errno(MC_LOG_ERROR, "pipe2()", errno);
        mc_cleanup_cgroup(&container);
        return 1;
    }
    void *stack = malloc(MC_STACK_SIZE);
    if (stack == NULL) {
        mc_log_errno(MC_LOG_ERROR, "malloc(clone stack)", errno);
        (void)close(gate[0]);
        (void)close(gate[1]);
        mc_cleanup_cgroup(&container);
        return 1;
    }
    child_context context = {
        .container = container,
        .command = config->command,
        .gate_read = gate[0],
        .gate_write = gate[1]
    };
    struct sigaction previous_signals[2];
    if (install_signal_handlers(previous_signals) != 0) {
        free(stack);
        (void)close(gate[0]);
        (void)close(gate[1]);
        mc_cleanup_cgroup(&container);
        return 1;
    }
    const int flags = CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNET | SIGCHLD;
    const pid_t child = clone(container_child, (char *)stack + MC_STACK_SIZE, flags, &context);
    if (child < 0) {
        mc_log_errno(MC_LOG_ERROR, "clone(namespaces)", errno);
        restore_signal_handlers(previous_signals);
        free(stack);
        (void)close(gate[0]);
        (void)close(gate[1]);
        mc_cleanup_cgroup(&container);
        return 1;
    }
    container.pid = child;
    context.container.pid = child;
    (void)close(gate[0]);
    int parent_gate = gate[1];

    if (mc_attach_cgroup(&container, child) != 0 ||
        mc_setup_network_host(&container) != 0) {
        (void)close(parent_gate);
        stop_child(child);
        restore_signal_handlers(previous_signals);
        free(stack);
        mc_cleanup_network(&container);
        mc_cleanup_cgroup(&container);
        return 1;
    }
    (void)snprintf(container.state, sizeof(container.state), "running");
    if (save_metadata(&container) != 0) {
        (void)close(parent_gate);
        stop_child(child);
        restore_signal_handlers(previous_signals);
        free(stack);
        mc_cleanup_network(&container);
        mc_cleanup_cgroup(&container);
        return 1;
    }
    mc_log(MC_LOG_INFO, "created container %s (host pid %ld)", container.id, (long)child);
    release_child(parent_gate);

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
        if (waited < 0 && errno == EINTR && mc_pending_signal != 0) {
            mc_pending_signal = 0;
            if (kill(child, SIGKILL) != 0 && errno != ESRCH) {
                mc_log_errno(MC_LOG_WARN, "kill(container after parent signal)", errno);
            }
        }
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        mc_log_errno(MC_LOG_ERROR, "waitpid(container)", errno);
        stop_child(child);
        status = MC_CHILD_FAILURE << 8;
    }
    (void)snprintf(container.state, sizeof(container.state), "stopped");
    mc_cleanup_network(&container);
    mc_cleanup_cgroup(&container);
    remove_metadata(&container);
    free(stack);
    const int requested_signal = mc_pending_signal;
    restore_signal_handlers(previous_signals);
    const int result = requested_signal != 0 ? 128 + requested_signal : child_exit_code(status);
    mc_log(MC_LOG_INFO, "container %s exited (status=%d)", container.id, result);
    return result;
}

int mc_inspect(void) {
    if (access(MC_STATE_DIR, R_OK | X_OK) != 0) {
        if (errno == ENOENT) {
            puts("No active containers.");
            return 0;
        }
        mc_log_errno(MC_LOG_ERROR, "access(state directory)", errno);
        return 1;
    }
    DIR *directory = opendir(MC_STATE_DIR);
    if (directory == NULL) {
        mc_log_errno(MC_LOG_ERROR, "opendir(state directory)", errno);
        return 1;
    }
    struct dirent *entry;
    bool found = false;
    while ((entry = readdir(directory)) != NULL) {
        const size_t name_length = strlen(entry->d_name);
        if (name_length < 6U || strcmp(entry->d_name + name_length - 5U, ".meta") != 0) continue;
        char path[PATH_MAX];
        const int length = snprintf(path, sizeof(path), "%s/%s", MC_STATE_DIR, entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(path)) {
            mc_log(MC_LOG_WARN, "metadata path too long; skipping");
            continue;
        }
        struct stat info;
        if (lstat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != 0) {
            mc_log(MC_LOG_WARN, "refusing unsafe metadata entry %s", entry->d_name);
            continue;
        }
        FILE *stream = fopen(path, "r");
        if (stream == NULL) {
            mc_log_errno(MC_LOG_WARN, "fopen(container metadata)", errno);
            continue;
        }
        if (found) putchar('\n');
        found = true;
        char line[PATH_MAX + 64];
        while (fgets(line, sizeof(line), stream) != NULL) fputs(line, stdout);
        (void)fclose(stream);
    }
    (void)closedir(directory);
    if (!found) puts("No active containers.");
    return 0;
}
