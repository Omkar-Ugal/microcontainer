#include "microcontainer.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 ||
        strcmp(argv[1], "-h") == 0) {
        mc_usage(argc < 2 ? fileno(stderr) : fileno(stdout));
        return argc < 2 ? 2 : 0;
    }
    if (strcmp(argv[1], "inspect") == 0) {
        if (argc != 2) {
            mc_log(MC_LOG_ERROR, "inspect accepts no arguments");
            return 2;
        }
        return mc_inspect();
    }
    if (strcmp(argv[1], "run") != 0) {
        mc_log(MC_LOG_ERROR, "unknown command '%s'", argv[1]);
        mc_usage(fileno(stderr));
        return 2;
    }

    mc_config config;
    if (mc_parse_config(argc, argv, &config) != 0) {
        mc_usage(fileno(stderr));
        return 2;
    }
    return mc_run_container(&config);
}
