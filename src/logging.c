#include "microcontainer.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

void mc_log(mc_log_level level, const char *format, ...) {
    static const char *const names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    struct timespec now;
    (void)clock_gettime(CLOCK_REALTIME, &now);
    struct tm timestamp;
    if (localtime_r(&now.tv_sec, &timestamp) == NULL) {
        memset(&timestamp, 0, sizeof(timestamp));
    }

    fprintf(stderr, "%04d-%02d-%02dT%02d:%02d:%02d.%03ld %-5s ",
            timestamp.tm_year + 1900, timestamp.tm_mon + 1, timestamp.tm_mday,
            timestamp.tm_hour, timestamp.tm_min, timestamp.tm_sec,
            now.tv_nsec / 1000000L, names[level]);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

void mc_log_errno(mc_log_level level, const char *operation, int error_number) {
    mc_log(level, "%s failed: %s (errno=%d)", operation, strerror(error_number), error_number);
}
