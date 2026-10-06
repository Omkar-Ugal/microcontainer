#include "microcontainer.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

int mc_validate_rootfs(const char *path, char resolved[PATH_MAX]) {
    if (path == NULL || resolved == NULL || realpath(path, resolved) == NULL) {
        mc_log_errno(MC_LOG_ERROR, "realpath(rootfs)", errno);
        return -1;
    }
    struct stat info;
    if (stat(resolved, &info) != 0) {
        mc_log_errno(MC_LOG_ERROR, "stat(rootfs)", errno);
        return -1;
    }
    if (!S_ISDIR(info.st_mode) || strcmp(resolved, "/") == 0) {
        mc_log(MC_LOG_ERROR, "rootfs must be an existing directory other than /");
        return -1;
    }
    for (const unsigned char *cursor = (const unsigned char *)resolved; *cursor != '\0'; ++cursor) {
        if (*cursor < 0x20U || *cursor == 0x7fU) {
            mc_log(MC_LOG_ERROR, "rootfs path must not contain control characters");
            return -1;
        }
    }
    return 0;
}

int mc_setup_filesystem(const char *rootfs) {
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        mc_log_errno(MC_LOG_ERROR, "mount(private propagation)", errno);
        return -1;
    }
    if (mount(rootfs, rootfs, NULL, MS_BIND, NULL) != 0) {
        mc_log_errno(MC_LOG_ERROR, "mount(rootfs bind)", errno);
        return -1;
    }
    if (chdir(rootfs) != 0) {
        mc_log_errno(MC_LOG_ERROR, "chdir(rootfs)", errno);
        return -1;
    }
    if (chroot(".") != 0) {
        mc_log_errno(MC_LOG_ERROR, "chroot(rootfs)", errno);
        return -1;
    }
    if (chdir("/") != 0) {
        mc_log_errno(MC_LOG_ERROR, "chdir(/)", errno);
        return -1;
    }
    struct stat proc_info;
    if (stat("/proc", &proc_info) != 0) {
        mc_log_errno(MC_LOG_ERROR, "stat(rootfs /proc)", errno);
        return -1;
    }
    if (!S_ISDIR(proc_info.st_mode)) {
        mc_log(MC_LOG_ERROR, "rootfs must contain an empty /proc directory");
        return -1;
    }
    if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0) {
        mc_log_errno(MC_LOG_ERROR, "mount(proc)", errno);
        return -1;
    }
    return 0;
}
