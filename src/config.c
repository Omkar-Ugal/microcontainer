#include "microcontainer.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mc_usage(int fd) {
    dprintf(fd,
        "MicroContainer - educational Linux namespace runtime\n"
        "Usage:\n"
        "  microcontainer run [--hostname NAME] [--memory SIZE] [--pids COUNT]\n"
        "                     [--network none|veth] ROOTFS COMMAND [ARG...]\n"
        "  microcontainer inspect\n"
        "  microcontainer help\n"
        "SIZE accepts bytes or K, M, G suffixes (binary units), e.g. 256M.\n"
        "veth uses 10.200.0.0/30; it does not configure NAT or DNS.\n");
}

int mc_parse_size(const char *text, uint64_t *result) {
    if (text == NULL || !isdigit((unsigned char)*text) || result == NULL) {
        return -1;
    }
    errno = 0;
    char *end = NULL;
    const unsigned long long value = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || value == 0U) {
        return -1;
    }
    uint64_t multiplier = 1;
    if (*end != '\0') {
        if (end[1] != '\0') {
            return -1;
        }
        switch (toupper((unsigned char)*end)) {
            case 'K': multiplier = 1024ULL; break;
            case 'M': multiplier = 1024ULL * 1024ULL; break;
            case 'G': multiplier = 1024ULL * 1024ULL * 1024ULL; break;
            default: return -1;
        }
    }
    if ((uint64_t)value > UINT64_MAX / multiplier) {
        return -1;
    }
    *result = (uint64_t)value * multiplier;
    return 0;
}

static int parse_pids(const char *text, unsigned long *result) {
    if (text == NULL || *text == '\0' || result == NULL) {
        return -1;
    }
    errno = 0;
    char *end = NULL;
    const unsigned long value = strtoul(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || value == 0UL || value > INT_MAX) {
        return -1;
    }
    *result = value;
    return 0;
}

static int valid_hostname(const char *name) {
    if (name == NULL || *name == '\0') return 0;
    for (const unsigned char *cursor = (const unsigned char *)name; *cursor != '\0'; ++cursor) {
        if (!isalnum(*cursor) && *cursor != '-' && *cursor != '_' && *cursor != '.') return 0;
    }
    return 1;
}

int mc_parse_config(int argc, char **argv, mc_config *config) {
    if (config == NULL || argc < 2) {
        return -1;
    }
    memset(config, 0, sizeof(*config));
    (void)snprintf(config->hostname, sizeof(config->hostname), "microcontainer");
    config->network = MC_NETWORK_NONE;

    int index = 2;
    while (index < argc && argv[index][0] == '-') {
        if (strcmp(argv[index], "--hostname") == 0) {
            if (++index >= argc || argv[index][0] == '\0' ||
                strlen(argv[index]) >= sizeof(config->hostname) || !valid_hostname(argv[index])) {
                mc_log(MC_LOG_ERROR, "--hostname requires a name of 1 to %zu bytes",
                       sizeof(config->hostname) - 1U);
                return -1;
            }
            (void)snprintf(config->hostname, sizeof(config->hostname), "%s", argv[index++]);
        } else if (strcmp(argv[index], "--memory") == 0) {
            if (++index >= argc || mc_parse_size(argv[index], &config->memory_limit) != 0) {
                mc_log(MC_LOG_ERROR, "--memory requires a positive size such as 256M");
                return -1;
            }
            ++index;
        } else if (strcmp(argv[index], "--pids") == 0) {
            if (++index >= argc || parse_pids(argv[index], &config->pids_limit) != 0) {
                mc_log(MC_LOG_ERROR, "--pids requires a positive integer no greater than INT_MAX");
                return -1;
            }
            ++index;
        } else if (strcmp(argv[index], "--network") == 0) {
            if (++index >= argc) {
                mc_log(MC_LOG_ERROR, "--network requires none or veth");
                return -1;
            }
            if (strcmp(argv[index], "none") == 0) {
                config->network = MC_NETWORK_NONE;
            } else if (strcmp(argv[index], "veth") == 0) {
                config->network = MC_NETWORK_VETH;
            } else {
                mc_log(MC_LOG_ERROR, "unsupported network mode '%s' (choose none or veth)", argv[index]);
                return -1;
            }
            ++index;
        } else {
            mc_log(MC_LOG_ERROR, "unknown run option '%s'", argv[index]);
            return -1;
        }
    }

    if (argc - index < 2) {
        mc_log(MC_LOG_ERROR, "run requires ROOTFS and an absolute COMMAND");
        return -1;
    }
    if (argv[index][0] == '\0' || strlen(argv[index]) >= sizeof(config->rootfs)) {
        mc_log(MC_LOG_ERROR, "invalid rootfs path");
        return -1;
    }
    (void)snprintf(config->rootfs, sizeof(config->rootfs), "%s", argv[index++]);
    if (argv[index][0] != '/') {
        mc_log(MC_LOG_ERROR, "COMMAND must be an absolute path inside ROOTFS");
        return -1;
    }
    config->command = &argv[index];
    return 0;
}
