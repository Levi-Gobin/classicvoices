// The released Mac app loads fast code the way Linux does
// (ports/posix/vx_native_dl.c); its messages go to the unified log.
#include <os/log.h>
#include <stdarg.h>
#include <stdio.h>

void vx_posix_log(int level, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    os_log_with_type(OS_LOG_DEFAULT, level >= 2 ? OS_LOG_TYPE_ERROR : OS_LOG_TYPE_INFO, "%{public}s", msg);
}
