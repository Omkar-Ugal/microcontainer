#include "microcontainer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

static void test_sizes(void) {
    uint64_t size = 0;
    check(mc_parse_size("256M", &size) == 0 && size == 256ULL * 1024ULL * 1024ULL,
          "memory size parses binary megabytes");
    check(mc_parse_size("3g", &size) == 0 && size == 3ULL * 1024ULL * 1024ULL * 1024ULL,
          "memory size suffix is case insensitive");
    check(mc_parse_size("4096", &size) == 0 && size == 4096, "plain byte count parses");
    check(mc_parse_size("0", &size) != 0, "zero memory limit rejected");
    check(mc_parse_size("-1", &size) != 0, "negative memory limit rejected");
    check(mc_parse_size("2MB", &size) != 0, "unknown memory suffix rejected");
    check(mc_parse_size("18446744073709551615G", &size) != 0, "overflow is rejected");
}

static void test_config(void) {
    char *valid[] = {"microcontainer", "run", "--hostname", "testbox",
                     "--memory", "256M", "--pids", "50", "--network", "veth",
                     "/tmp/rootfs", "/bin/sh", "-c", "true"};
    mc_config config;
    check(mc_parse_config((int)(sizeof(valid) / sizeof(valid[0])), valid, &config) == 0,
          "valid run configuration accepted");
    check(strcmp(config.hostname, "testbox") == 0, "hostname stored");
    check(config.memory_limit == 256ULL * 1024ULL * 1024ULL, "memory limit stored");
    check(config.pids_limit == 50UL, "pid limit stored");
    check(config.network == MC_NETWORK_VETH, "network mode stored");
    check(strcmp(config.rootfs, "/tmp/rootfs") == 0, "rootfs stored");
    check(config.command != NULL && strcmp(config.command[0], "/bin/sh") == 0 &&
          strcmp(config.command[2], "true") == 0, "command and arguments preserved");

    char *bad_hostname[] = {"microcontainer", "run", "--hostname", "bad\nname",
                            "/tmp/rootfs", "/bin/sh"};
    check(mc_parse_config(6, bad_hostname, &config) != 0, "hostname control characters rejected");
    char *bad_network[] = {"microcontainer", "run", "--network", "bridge", "/tmp/rootfs", "/bin/sh"};
    check(mc_parse_config(6, bad_network, &config) != 0, "unsupported network mode rejected");
    char *bad_memory[] = {"microcontainer", "run", "--memory", "0", "/tmp/rootfs", "/bin/sh"};
    check(mc_parse_config(6, bad_memory, &config) != 0, "invalid memory limit rejected");
    char *relative_command[] = {"microcontainer", "run", "/tmp/rootfs", "sh"};
    check(mc_parse_config(4, relative_command, &config) != 0, "relative command rejected");
    char *missing_arguments[] = {"microcontainer", "run", "/tmp/rootfs"};
    check(mc_parse_config(3, missing_arguments, &config) != 0, "missing command rejected");
}

static void test_rootfs_validation(void) {
    char directory[] = "/tmp/microcontainer-rootfs.XXXXXX";
    char resolved[PATH_MAX];
    char *created = mkdtemp(directory);
    check(created != NULL, "temporary rootfs directory created");
    if (created != NULL) {
        check(mc_validate_rootfs(created, resolved) == 0, "existing directory accepted as rootfs");
        check(strcmp(resolved, created) == 0, "rootfs canonicalized");
        check(mc_validate_rootfs("/", resolved) != 0, "host root rejected as rootfs");
        check(rmdir(created) == 0, "temporary rootfs removed");
    }
}

int main(void) {
    test_sizes();
    test_config();
    test_rootfs_validation();
    if (failures != 0) {
        fprintf(stderr, "%d configuration tests failed\n", failures);
        return EXIT_FAILURE;
    }
    puts("MicroContainer configuration tests passed");
    return EXIT_SUCCESS;
}
