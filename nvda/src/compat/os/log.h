// os_log shim: the Windows engine logs through vx_winlog, which rewrites
// Apple's %{public}s format specifiers and writes to stderr when CV_DEBUG=1.
#pragma once

#define OS_LOG_DEFAULT 0
void vx_winlog(int dummy, const char *fmt, ...);
#define os_log vx_winlog
#define os_log_error vx_winlog
#define os_log_info vx_winlog
#define os_log_debug vx_winlog
