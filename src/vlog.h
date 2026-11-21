/* vlog: the one logger of voxup. Every run appends to voxup.log beside the exe (out\voxup.log).
   One line a message:  2026-11-19 20:41:07.123 WARN  [save] could not write out\pick\004\desc.txt: Permission denied
   Levels: VLOG_ERR (something the user asked for failed), VLOG_WARN (went on, but not as asked), VLOG_INFO (what was
   done: sprite opened, way built, file saved, batch summary), VLOG_DBG (detail, written only when VOXUP_DEBUG=1 or
   --debug). The file is kept under 1 MB: when bigger at start it is renamed voxup.log.1 (the older .1 replaced).
   vlog_last() is the last ERR/WARN text, for the window's footer; vlog_errors() counts ERR lines this run.
   Rules: every failed fopen/malloc/load/save is logged with the path and the reason (vlog_errno / GetLastError text),
   and the user sees it too (footer in the window, stderr in batch). Never a silent failure, never a crash on bad input. */
#ifndef VLOG_H
#define VLOG_H
#ifdef __cplusplus
extern "C" {
#endif
enum { VLOG_ERR, VLOG_WARN, VLOG_INFO, VLOG_DBG };
void vlog_open(const char *path, int debug);          /* NULL path: beside the exe; safe to call twice */
void vlog_close(void);
void vlog(int level, const char *area, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 3, 4)))
#endif
    ;
void vlog_echo(int on);                               /* batch: ERR/WARN also go to stderr (as "error: ..." / "warning: ...") */
const char *vlog_errno(void);                         /* strerror(errno), or the Windows GetLastError() text */
const char *vlog_last(void);                          /* last ERR/WARN message ("" if none) */
int vlog_errors(void);
#ifdef __cplusplus
}
#endif
#endif
