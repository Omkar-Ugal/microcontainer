#include "microcontainer.h"

#include <errno.h>
#include <linux/capability.h>
#include <linux/prctl.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

int mc_drop_capabilities(void) {
    struct __user_cap_header_struct header = {
        .version = _LINUX_CAPABILITY_VERSION_3,
        .pid = 0
    };
    struct __user_cap_data_struct data[2];
    memset(data, 0, sizeof(data));
    if (syscall(SYS_capset, &header, data) != 0) {
        mc_log_errno(MC_LOG_ERROR, "capset(drop capabilities)", errno);
        return -1;
    }
    return 0;
}

int mc_drop_capability_bounding_set(void) {
    for (int capability = 0; capability <= CAP_LAST_CAP; ++capability) {
        if (prctl(PR_CAPBSET_DROP, capability, 0L, 0L, 0L) != 0 && errno != EINVAL) {
            mc_log_errno(MC_LOG_ERROR, "prctl(PR_CAPBSET_DROP)", errno);
            return -1;
        }
    }
    return 0;
}

int mc_set_no_new_privileges(void) {
    if (prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) != 0) {
        mc_log_errno(MC_LOG_ERROR, "prctl(PR_SET_NO_NEW_PRIVS)", errno);
        return -1;
    }
    return 0;
}
