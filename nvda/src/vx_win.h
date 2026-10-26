// Force-included (gcc -include) into every core/*.c compile on Windows.
// Redirects the handful of POSIX/Darwin calls the engine makes to Windows
// implementations in vx_win.c, with UTF-8 paths throughout (NVDA passes
// UTF-8; the CRT's ANSI functions would mangle non-ASCII user names).
//
// The system headers are included here FIRST so that the function-like macros
// below never see (and mangle) the CRT's own declarations: when the engine
// sources include these headers again, the include guards make them no-ops.
#pragma once
#include <fcntl.h>
#include <io.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

FILE *vx_fopen_utf8(const char *path, const char *mode);
int vx_open_rd(const char *path);
int vx_access_utf8(const char *path, int mode);
int vx_stat_utf8(const char *path, struct stat *st);
void vx_sleep_ns(long long ns);

#define fopen vx_fopen_utf8
#define open(p, f) vx_open_rd(p)
#define access(p, m) vx_access_utf8((p), (m))
#define stat(p, b) vx_stat_utf8((p), (b))
#define getpagesize() 4096
#define nanosleep(ts, rem) (vx_sleep_ns((ts)->tv_sec * 1000000000LL + (ts)->tv_nsec), 0)

// Apple QoS thread attributes: no-ops here (the worker thread is plenty fast).
#define pthread_attr_set_qos_class_np(attr, cls, pri) 0
#define QOS_CLASS_USER_INTERACTIVE 0


#ifdef __cplusplus
}
#endif
