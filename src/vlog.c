/* vlog: see vlog.h. Plain C; Windows calls only for the exe's folder and GetLastError text. */
#include "vlog.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

static FILE *lf; static int dbg, echo, nerr; static volatile long nprob; static char last[512], errbuf[256];
static const char *LV[] = { "ERR ", "WARN", "INFO", "DBG " };

void vlog_open(const char *path, int debug) {
    char p[1024], old[1040]; long sz = 0; FILE *t;
    const char *env = getenv("VOXUP_DEBUG");
    dbg = debug || (env && *env && *env != '0');
    if (lf) return;
    if (path) { strncpy(p, path, sizeof p - 1); p[sizeof p - 1] = 0; }
    else {
#ifdef _WIN32
        char *s; DWORD n = GetModuleFileNameA(NULL, p, sizeof p);
        if (!n || n >= sizeof p) strcpy(p, "voxup.log");
        else { s = strrchr(p, '\\'); if (s) s[1] = 0; else p[0] = 0; strncat(p, "out\\", sizeof p - strlen(p) - 1); CreateDirectoryA(p, NULL); strncat(p, "voxup.log", sizeof p - strlen(p) - 1); }
#else
        strcpy(p, "out/voxup.log");
#endif
    }
    if ((t = fopen(p, "rb")) != NULL) { fseek(t, 0, SEEK_END); sz = ftell(t); fclose(t); }
    if (sz > 1024L * 1024) { snprintf(old, sizeof old, "%s.1", p); remove(old); rename(p, old); }
    lf = fopen(p, "ab");
    if (!lf) fprintf(stderr, "voxup: cannot open the log %s: %s\n", p, strerror(errno));
    vlog(VLOG_INFO, "start", "---- voxup started%s ----", dbg ? " (debug lines on)" : "");
}

void vlog_close(void) { if (lf) { vlog(VLOG_INFO, "start", "---- voxup ended, %d error(s) ----", nerr); fclose(lf); lf = NULL; } }
void vlog_echo(int on) { echo = on; }
const char *vlog_last(void) { return last; }
int vlog_errors(void) { return nerr; }

const char *vlog_errno(void) {
#ifdef _WIN32
    DWORD e = GetLastError();
    if (errno) { snprintf(errbuf, sizeof errbuf, "%s", strerror(errno)); errno = 0; return errbuf; }
    if (e && FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, errbuf, sizeof errbuf, NULL)) {
        size_t n = strlen(errbuf); while (n && (errbuf[n - 1] == '\n' || errbuf[n - 1] == '\r' || errbuf[n - 1] == '.')) errbuf[--n] = 0;
        return errbuf;
    }
    return "unknown reason";
#else
    return strerror(errno);
#endif
}

void vlog(int level, const char *area, const char *fmt, ...) {
    char msg[1024]; va_list ap; time_t now = time(NULL); struct tm tmv; int ms = 0;
    if (level < VLOG_ERR) level = VLOG_ERR;
    if (level > VLOG_DBG) level = VLOG_DBG;
    if (level == VLOG_DBG && !dbg) return;
    va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    if (level == VLOG_ERR) nerr++;
    if (level <= VLOG_WARN) { strncpy(last, msg, sizeof last - 1); last[sizeof last - 1] = 0; nprob++; }
#ifdef _WIN32
    { SYSTEMTIME st; GetLocalTime(&st); ms = st.wMilliseconds; }
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    if (lf) {
        fprintf(lf, "%04d-%02d-%02d %02d:%02d:%02d.%03d %s [%s] %s\r\n", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms, LV[level], area ? area : "-", msg);
        fflush(lf);
    }
    if (echo) {
        if (level <= VLOG_WARN) { fprintf(stderr, "%s: %s\n", level == VLOG_ERR ? "error" : "warning", msg); fflush(stderr); }          /* INFO stays in the log: batch prints its own progress */
    }
}

int vlog_problems(void) { return (int)nprob; }
