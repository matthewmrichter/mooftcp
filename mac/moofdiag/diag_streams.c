/* diag_streams.c - MoofDiag's stream and owner lines (Plan 3 Task 12). Its own file: mt_pb.h brings
 * mt_mactcp.h, whose MacTCP types clash with the Toolbox headers moofdiag.c includes. Read-only: the
 * driver's MtDrv is read at task time without the gate (a line may be momentarily stale). */
#include <stdio.h>
#include "mt_pb.h"
#include "diag_streams.h"

static const char *const st_name[] = { "closed", "listen", "syn_rcvd", "syn_sent", "established", "fin_wait1",
                                       "fin_wait2", "close_wait", "closing", "last_ack", "time_wait" };

void diag_streams(const void *drv, void (*line)(const char *s))
{
    const MtDrv *d = (const MtDrv *)drv;
    char b[200];
    uint8_t i;
    if (!d) return;
    snprintf(b, sizeof b, "owners: %u recorded, gone %lu, not recorded (table full) %lu, routines of gone apps skipped %lu\r",
             d->n_own, (unsigned long)d->n_own_gone, (unsigned long)d->n_own_full, (unsigned long)d->n_dead_calls);
    line(b);
    snprintf(b, sizeof b, "swept: by a Control call %lu, by the timer %lu, by the input path %lu; frames/ticks dropped (owner gone, sweep busy) %lu\r",
             (unsigned long)d->n_gone_call, (unsigned long)d->n_gone_timer, (unsigned long)d->n_gone_rx,
             (unsigned long)d->n_dead_drops);
    line(b);
    for (i = 0; i < MT_PB_OWNERS; i++)
        if (d->own[i].lo) {
            snprintf(b, sizeof b, "owner[%u]: partition %08lx-%08lx header %08lx\r", i, (unsigned long)d->own[i].lo,
                     (unsigned long)d->own[i].hi, (unsigned long)d->own[i].h0);
            line(b);
        }
    for (i = 0; i < d->ntcp; i++) {
        const MtStream *s = &d->tcp[i];
        mt_tcp_info in;
        if (!s->used) continue;
        if (s->t) {
            mt_tcp_get_info(s->t, &in);
            snprintf(b, sizeof b, "tcp[%u]: handle %08lx rbuf %08lx a5 %08lx %s local %u remote %lu.%lu.%lu.%lu:%u\r", i,
                     (unsigned long)s->handle, (unsigned long)s->rbuf, (unsigned long)s->a5,
                     (unsigned)in.state <= (unsigned)MT_TCP_TIME_WAIT ? st_name[in.state] : "?", in.lport,
                     (unsigned long)(in.rip >> 24), (unsigned long)(in.rip >> 16 & 255),
                     (unsigned long)(in.rip >> 8 & 255), (unsigned long)(in.rip & 255), in.rport);
        } else {
            snprintf(b, sizeof b, "tcp[%u]: handle %08lx rbuf %08lx a5 %08lx (no connection)\r", i,
                     (unsigned long)s->handle, (unsigned long)s->rbuf, (unsigned long)s->a5);
        }
        line(b);
    }
    for (i = 0; i < d->nudp; i++) {
        const MtUdpStream *u = &d->udp[i];
        if (!u->used) continue;
        snprintf(b, sizeof b, "udp[%u]: handle %08lx rbuf %08lx a5 %08lx loans %u\r", i, (unsigned long)u->handle,
                 (unsigned long)u->rbuf, (unsigned long)u->a5, u->loan_n);
        line(b);
    }
}
