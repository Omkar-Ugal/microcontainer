#include "microcontainer.h"

#include <errno.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

int mc_setup_namespaces(const mc_container *container) {
    if (sethostname(container->hostname, strlen(container->hostname)) != 0) {
        mc_log_errno(MC_LOG_ERROR, "sethostname()", errno);
        return -1;
    }
    struct utsname identity;
    if (uname(&identity) != 0) {
        mc_log_errno(MC_LOG_ERROR, "uname()", errno);
        return -1;
    }
    mc_log(MC_LOG_DEBUG, "container hostname is %s (kernel %s)", identity.nodename, identity.release);
    return 0;
}
