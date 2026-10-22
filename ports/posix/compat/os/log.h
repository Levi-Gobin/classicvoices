// os_log shim for Linux and Android: the engine's Apple-style log calls go to
// stderr on Linux (speech-dispatcher keeps module stderr in its log) and to
// logcat on Android. Apple's %{public}s specifiers are rewritten to %s.
#pragma once

#define OS_LOG_DEFAULT 0
void vx_posix_log(int level, const char *fmt, ...);
#define os_log(log, ...) vx_posix_log(1, __VA_ARGS__)
#define os_log_info(log, ...) vx_posix_log(1, __VA_ARGS__)
#define os_log_debug(log, ...) vx_posix_log(0, __VA_ARGS__)
#define os_log_error(log, ...) vx_posix_log(2, __VA_ARGS__)
#define os_log_fault(log, ...) vx_posix_log(2, __VA_ARGS__)
