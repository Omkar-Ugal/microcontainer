#include "microcontainer.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int run_ip(char *const arguments[]) {
    static const char *const candidates[] = {"/usr/sbin/ip", "/sbin/ip", "/usr/bin/ip", NULL};
    char *const environment[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C", NULL};
    pid_t child = fork();
    if (child < 0) {
        mc_log_errno(MC_LOG_ERROR, "fork(iproute2 helper)", errno);
        return -1;
    }
    if (child == 0) {
        for (size_t i = 0; candidates[i] != NULL; ++i) {
            execve(candidates[i], arguments, environment);
            if (errno != ENOENT) {
                mc_log_errno(MC_LOG_ERROR, "execv(ip)", errno);
                _exit(127);
            }
        }
        mc_log(MC_LOG_ERROR, "iproute2 executable not found (/usr/sbin/ip, /sbin/ip, /usr/bin/ip)");
        _exit(127);
    }
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        mc_log_errno(MC_LOG_ERROR, "waitpid(iproute2 helper)", errno);
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        mc_log(MC_LOG_ERROR, "iproute2 command failed (status=%d)", status);
        return -1;
    }
    return 0;
}

int mc_setup_network_host(mc_container *container) {
    if (container->network != MC_NETWORK_VETH) return 0;
    const int length = snprintf(container->host_interface, sizeof(container->host_interface),
                                "mch%ld", (long)container->pid);
    if (length < 0 || (size_t)length >= sizeof(container->host_interface)) {
        mc_log(MC_LOG_ERROR, "host interface name exceeds IFNAMSIZ");
        return -1;
    }
    char *add[] = {"ip", "link", "add", container->host_interface,
                   "type", "veth", "peer", "name", "mcpeer", NULL};
    char pid[32];
    (void)snprintf(pid, sizeof(pid), "%ld", (long)container->pid);
    char *move[] = {"ip", "link", "set", "mcpeer", "netns", pid, NULL};
    char *address[] = {"ip", "addr", "add", "10.200.0.1/30", "dev", container->host_interface, NULL};
    char *up[] = {"ip", "link", "set", container->host_interface, "up", NULL};
    if (run_ip(add) != 0) return -1;
    container->host_interface_created = true;
    if (run_ip(move) != 0 || run_ip(address) != 0 || run_ip(up) != 0) {
        mc_cleanup_network(container);
        return -1;
    }
    mc_log(MC_LOG_INFO, "veth host interface %s configured (10.200.0.1/30)",
           container->host_interface);
    return 0;
}

int mc_setup_network_child(const mc_container *container) {
    if (container->network != MC_NETWORK_VETH) return 0;
    char *loopback[] = {"ip", "link", "set", "lo", "up", NULL};
    char *rename[] = {"ip", "link", "set", "mcpeer", "name", "eth0", NULL};
    char *address[] = {"ip", "addr", "add", "10.200.0.2/30", "dev", "eth0", NULL};
    char *up[] = {"ip", "link", "set", "eth0", "up", NULL};
    char *route[] = {"ip", "route", "add", "default", "via", "10.200.0.1", NULL};
    if (run_ip(loopback) != 0 || run_ip(rename) != 0 ||
        run_ip(address) != 0 || run_ip(up) != 0 || run_ip(route) != 0) {
        return -1;
    }
    return 0;
}

void mc_cleanup_network(const mc_container *container) {
    if (!container->host_interface_created || container->host_interface[0] == '\0') return;
    char *remove[] = {"ip", "link", "delete", (char *)container->host_interface, NULL};
    if (run_ip(remove) != 0) {
        mc_log(MC_LOG_WARN, "host veth cleanup may require manual removal of %s",
               container->host_interface);
    }
}
