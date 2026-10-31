/* probe_os.h - the few Toolbox services ApiProbe needs. Implemented in probe_os.c, the
 * only file that includes Retro68's Toolbox headers (they define their own MacTCP types,
 * which would clash with mt_mactcp.h). Plain C types only. */
#ifndef PROBE_OS_H
#define PROBE_OS_H
void          os_init(void);
unsigned long os_ticks(void);                 /* TickCount() */
short         os_open_ipp(short *ref);        /* OpenDriver(".IPP"); returns OSErr */
short         os_close_drv(short ref);        /* CloseDriver */
short         os_control_sync(void *pb);      /* PBControlSync (returns OSErr) */
short         os_control_async(void *pb);     /* PBControlAsync */
unsigned long os_cur_a5(void);                /* low memory CurrentA5 */
unsigned long os_free_mem(void);              /* FreeMem in the current zone */
short         os_out_open(const char *pname); /* pascal name; creates/truncates in the default dir */
short         os_out_write(const char *buf, long n);
void          os_out_flush(void);             /* FlushVol: survive a crash of the machine */
void          os_out_close(void);
void          os_done_alert(void);
int           os_read_cfg(const char *pname, char *buf, int max);   /* bytes read, 0 if absent */
/* DNR: load the 'dnrp' resource of "MacTCP DNR" (System Folder), locked; NULL on failure (err gets the
 * OSErr). os_free_dnr disposes it. os_set_a5 sets A5 (returns the old one); os_get_sr reads SR. */
void         *os_load_dnr(short *err);
void          os_free_dnr(void *code);
unsigned long os_app_free(void);              /* FreeMem in the application zone */
unsigned long os_sys_free(void);              /* FreeMem in the system zone */
#endif
