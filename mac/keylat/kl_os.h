/* kl_os.h - the Toolbox services KeyLat needs. Implemented in kl_os.c, the only file that includes Retro68's
 * Toolbox headers (they define their own MacTCP types, which clash with mt_mactcp.h). Plain C types only. */
#ifndef KL_OS_H
#define KL_OS_H
#include <stdint.h>

typedef struct KlKey { uint32_t when; uint32_t seen; char ch; } KlKey;

void     os_init(void);
uint32_t os_ticks(void);                          /* TickCount(): 60.15 Hz, the only clock KeyLat uses */
short    os_open_ipp(short *ref);                 /* OpenDriver(".IPP") */
short    os_control_sync(void *pb);
short    os_control_async(void *pb);
/* Setup dialog (DLOG 128): host and port prefilled; returns 1 for Connect, 0 for Cancel. */
int      os_setup_dialog(char *host, int hmax, char *port, int pmax);
void     os_window(void);                         /* the status window */
void     os_show(int line, const char *s);        /* text line 0..5 of the window (redrawn at once) */
void     os_alert(const char *s);                 /* modal note (ALRT 128), s is C text, <= 200 chars */
/* One WaitNextEvent / GetNextEvent. 0 = nothing for the app, 1 = a keyDown (not autoKey, no Cmd; *k filled,
 * k->seen = TickCount() right after the event call returned), 2 = quit (Cmd-Q or the close box). */
int      os_poll(KlKey *k);
int      os_event_pending(void);                  /* EventAvail: an event waits */
short    os_out_open(void);                       /* "KeyLat.out" beside the app, created/truncated */
short    os_out_write(const char *buf, long n);
void     os_out_close(void);
int      os_read_cfg(char *buf, int max);         /* "KeyLat.cfg" beside the app; bytes read, 0 if absent */
void     os_write_cfg(const char *buf, long n);
#endif
