/* mf_os.h - the Toolbox side of MfEcho (mf_os.c is the only file with Toolbox headers: they clash
 * with mt_mactcp.h). Plain C types only. */
#ifndef MF_OS_H
#define MF_OS_H
#include <stdint.h>

void     os_init(void);
uint32_t os_ticks(void);
short    os_open_ipp(short *ref);
short    os_control_sync(void *pb);
/* Output "<CurApName>.out" (created/truncated) and config "<CurApName>.cfg" in the app's folder. */
void     os_app_name(char *c, int max);          /* C string */
short    os_out_open(void);
void     os_out(const char *fmt, ...);           /* appends, flushes the volume */
void     os_out_close(void);
int      os_read_cfg(char *buf, int max);        /* bytes read, 0 if absent */
/* WaitNextEvent (or GetNextEvent + SystemTask) for up to `ticks`; returns 1 if the user asked to
 * quit (Cmd-Q / a mouse click in the menu bar is ignored). */
int      os_yield(uint32_t ticks);
/* One line with the low-memory globals of the current application (ApplZone, ApplLimit, HeapEnd,
 * BufPtr, CurrentA5, CurStackBase, CurApName, CurApRefNum, SysZone, TheZone, MemTop), the zone's
 * bkLim, the 8 bytes before ApplZone and whether MultiFinder's _OSDispatch / WaitNextEvent exist. */
void     os_dump_lowmem(const char *tag);
/* The driver's resident block through Status 'MT': returns the MtDrv pointer (or 0) and the entry's
 * goodbye / goodbye-forced / dropped counters. */
void    *os_drv_status(short ref, uint32_t *n_goodbye, uint32_t *n_forced, uint32_t *n_dropped);
uint32_t os_cur_a5(void);
uint32_t os_appl_zone(void);
#endif
