#ifndef MICROCONTAINER_H
#define MICROCONTAINER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <sys/types.h>

#define MC_ID_MAX 64
#define MC_HOSTNAME_MAX 64
#define MC_ERROR_MAX 512

typedef enum {
    MC_LOG_DEBUG,
    MC_LOG_INFO,
    MC_LOG_WARN,
    MC_LOG_ERROR
} mc_log_level;

typedef enum {
    MC_NETWORK_NONE,
    MC_NETWORK_VETH
} mc_network_mode;

typedef struct {
    char id[MC_ID_MAX];
    pid_t pid;
    char rootfs[PATH_MAX];
    char hostname[MC_HOSTNAME_MAX];
    char host_interface[16];
    bool host_interface_created;
    bool cgroup_parent_created;
    mc_network_mode network;
    uint64_t memory_limit;
    unsigned long pids_limit;
    char state[16];
    char cgroup_path[PATH_MAX];
} mc_container;

typedef struct {
    char rootfs[PATH_MAX];
    char hostname[MC_HOSTNAME_MAX];
    uint64_t memory_limit;
    unsigned long pids_limit;
    mc_network_mode network;
    char **command;
} mc_config;

void mc_log(mc_log_level level, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
void mc_log_errno(mc_log_level level, const char *operation, int error_number);
void mc_usage(int fd);
int mc_parse_size(const char *text, uint64_t *result);
int mc_parse_config(int argc, char **argv, mc_config *config);
int mc_run_container(const mc_config *config);
int mc_inspect(void);

int mc_validate_rootfs(const char *path, char resolved[PATH_MAX]);
int mc_setup_filesystem(const char *rootfs);
int mc_setup_namespaces(const mc_container *container);
int mc_setup_network_host(mc_container *container);
int mc_setup_network_child(const mc_container *container);
void mc_cleanup_network(const mc_container *container);
int mc_setup_cgroup(mc_container *container);
int mc_attach_cgroup(const mc_container *container, pid_t pid);
void mc_cleanup_cgroup(const mc_container *container);
int mc_drop_capabilities(void);
int mc_drop_capability_bounding_set(void);
int mc_set_no_new_privileges(void);

#endif
