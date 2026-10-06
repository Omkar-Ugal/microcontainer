#include "microcontainer.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CGROUP_ROOT "/sys/fs/cgroup"

static int write_value(const char *path, const char *value) {
    const int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        mc_log_errno(MC_LOG_ERROR, path, errno);
        return -1;
    }
    const size_t length = strlen(value);
    ssize_t written;
    do {
        written = write(fd, value, length);
    } while (written < 0 && errno == EINTR);
    const int saved_errno = written < 0 ? errno : (size_t)written != length ? EIO : 0;
    if (close(fd) != 0 && saved_errno == 0) {
        mc_log_errno(MC_LOG_ERROR, "close(cgroup control)", errno);
        return -1;
    }
    if (saved_errno != 0) {
        mc_log_errno(MC_LOG_ERROR, path, saved_errno);
        return -1;
    }
    return 0;
}

static int control_path(char path[PATH_MAX], const mc_container *container, const char *name) {
    const int count = snprintf(path, PATH_MAX, "%s/%s", container->cgroup_path, name);
    if (count < 0 || count >= PATH_MAX) {
        mc_log(MC_LOG_ERROR, "cgroup path is too long");
        return -1;
    }
    return 0;
}

int mc_setup_cgroup(mc_container *container) {
    if (container->memory_limit == 0U && container->pids_limit == 0UL) {
        return 0;
    }
    struct stat info;
    if (stat(CGROUP_ROOT, &info) != 0) {
        mc_log_errno(MC_LOG_ERROR, "stat(cgroup v2 root)", errno);
        return -1;
    }
    if (!S_ISDIR(info.st_mode)) {
        mc_log(MC_LOG_ERROR, "cgroup v2 is unavailable at %s", CGROUP_ROOT);
        return -1;
    }
    char parent[PATH_MAX];
    const int parent_length = snprintf(parent, sizeof(parent), "%s/microcontainer", CGROUP_ROOT);
    if (parent_length < 0 || (size_t)parent_length >= sizeof(parent)) {
        mc_log(MC_LOG_ERROR, "cgroup parent path is too long");
        return -1;
    }
    if (mkdir(parent, 0755) != 0) {
        if (errno != EEXIST) {
            mc_log_errno(MC_LOG_ERROR, "mkdir(cgroup parent)", errno);
            return -1;
        }
    } else {
        container->cgroup_parent_created = true;
    }
    struct stat parent_info;
    if (lstat(parent, &parent_info) != 0) {
        mc_log_errno(MC_LOG_ERROR, "lstat(cgroup parent)", errno);
        if (container->cgroup_parent_created) (void)rmdir(parent);
        return -1;
    }
    if (!S_ISDIR(parent_info.st_mode) || S_ISLNK(parent_info.st_mode) ||
        parent_info.st_uid != 0) {
        mc_log(MC_LOG_ERROR, "cgroup parent is not a real directory");
        if (container->cgroup_parent_created) (void)rmdir(parent);
        return -1;
    }
    char group[PATH_MAX];
    const int group_length = snprintf(group, sizeof(group), "%s/%s", parent, container->id);
    if (group_length < 0 || (size_t)group_length >= sizeof(group)) {
        mc_log(MC_LOG_ERROR, "cgroup path is too long");
        if (container->cgroup_parent_created) (void)rmdir(parent);
        container->cgroup_parent_created = false;
        return -1;
    }
    if (mkdir(group, 0755) != 0) {
        mc_log_errno(MC_LOG_ERROR, "mkdir(container cgroup)", errno);
        if (container->cgroup_parent_created) (void)rmdir(parent);
        container->cgroup_parent_created = false;
        return -1;
    }
    (void)snprintf(container->cgroup_path, sizeof(container->cgroup_path), "%s", group);

    char path[PATH_MAX];
    char value[64];
    if (container->memory_limit > 0U) {
        if (control_path(path, container, "memory.max") != 0) return -1;
        (void)snprintf(value, sizeof(value), "%llu", (unsigned long long)container->memory_limit);
        if (write_value(path, value) != 0) return -1;
    }
    if (container->pids_limit > 0UL) {
        if (control_path(path, container, "pids.max") != 0) return -1;
        (void)snprintf(value, sizeof(value), "%lu", container->pids_limit);
        if (write_value(path, value) != 0) return -1;
    }
    return 0;
}

int mc_attach_cgroup(const mc_container *container, pid_t pid) {
    if (container->cgroup_path[0] == '\0') return 0;
    char path[PATH_MAX];
    const int length = snprintf(path, sizeof(path), "%s/cgroup.procs", container->cgroup_path);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        mc_log(MC_LOG_ERROR, "cgroup.procs path is too long");
        return -1;
    }
    char value[32];
    (void)snprintf(value, sizeof(value), "%ld", (long)pid);
    return write_value(path, value);
}

void mc_cleanup_cgroup(const mc_container *container) {
    if (container->cgroup_path[0] == '\0') return;
    if (rmdir(container->cgroup_path) != 0 && errno != ENOENT) {
        mc_log_errno(MC_LOG_WARN, "rmdir(container cgroup)", errno);
        return;
    }
    if (!container->cgroup_parent_created) return;
    char parent[PATH_MAX];
    const int length = snprintf(parent, sizeof(parent), "%s/microcontainer", CGROUP_ROOT);
    if (length >= 0 && (size_t)length < sizeof(parent) && rmdir(parent) != 0 &&
        errno != ENOENT && errno != ENOTEMPTY) {
        mc_log_errno(MC_LOG_WARN, "rmdir(cgroup parent)", errno);
    }
}
