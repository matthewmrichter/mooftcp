#include <string.h>
#include "pbfake.h"
#include "mt_int.h"

uint32_t pbf_now;
pbf_ev pbf_log[PBF_LOG];
int pbf_nlog;
int pbf_bad;
int pbf_irq, pbf_irq_max, pbf_irq_calls;
int pbf_in_int;

static uint64_t g_mem[PBF_SIZE / 8];
static uint32_t g_used;
#define NPROC 64
#define PROC_BASE 0xF0000000u
static pbf_compl_fn g_compl[NPROC];
static pbf_asr_fn g_asr[NPROC];
static int g_ncompl, g_nasr;

void pbf_reset(void) {
    memset(g_mem, 0, g_used);
    g_used = 0;
    g_ncompl = g_nasr = 0;
    pbf_nlog = 0;
    pbf_bad = 0;
    pbf_irq = pbf_irq_max = pbf_irq_calls = 0;
    pbf_in_int = 0;
    pbf_now = 1000;
}

void *pbf_alloc(uint32_t n) {
    uint32_t a = (g_used + 7u) & ~7u;
    if (n > PBF_SIZE || a > PBF_SIZE - n) return NULL;
    g_used = a + n;
    memset((uint8_t *)g_mem + a, 0, n);
    return (uint8_t *)g_mem + a;
}

MacPtr32 pbf_compl(pbf_compl_fn f) {
    if (g_ncompl >= NPROC) return 0;
    g_compl[g_ncompl] = f;
    return PROC_BASE + 4u * (uint32_t)g_ncompl++;
}

MacPtr32 pbf_asr(pbf_asr_fn f) {
    if (g_nasr >= NPROC) return 0;
    g_asr[g_nasr] = f;
    return PROC_BASE + 0x1000u + 4u * (uint32_t)g_nasr++;
}

MacPtr32 pbf_a(const void *p) {
    const uint8_t *b = (const uint8_t *)g_mem;
    if (!p) return 0;
    if ((const uint8_t *)p < b || (const uint8_t *)p >= b + PBF_SIZE) { pbf_bad++; return 0; }
    return PBF_BASE + (uint32_t)((const uint8_t *)p - b);
}

void *pbf_p(MacPtr32 a) {
    if (!a) return NULL;
    if (a < PBF_BASE || a - PBF_BASE >= PBF_SIZE) { pbf_bad++; return NULL; }
    return (uint8_t *)g_mem + (a - PBF_BASE);
}

/* ---- mt_os.h ---- */

uint32_t mt_os_ms(void) { return pbf_now; }
void *mt_os_ptr(MacPtr32 a) { return pbf_p(a); }
MacPtr32 mt_os_addr(const void *p) { return pbf_a(p); }

static void log_add(const pbf_ev *e) {
    if (pbf_nlog < PBF_LOG) pbf_log[pbf_nlog] = *e;
    pbf_nlog++;
}

void mt_os_complete(MtAnyPb *pb, int16_t err) {
    pbf_ev e;
    MacPtr32 c = pb->hdr.ioCompletion;
    memset(&e, 0, sizeof e);
    pb->hdr.ioResult = err;                     /* final before the routine runs (matrix 150) */
    e.kind = 'C';
    e.pb = pbf_a(pb);
    e.res = err;
    e.cs = pb->hdr.csCode;
    log_add(&e);
    if (!c) return;
    if (c < PROC_BASE || c >= PROC_BASE + 4u * NPROC || (c & 3u)) { pbf_bad++; return; }
    pbf_in_int++;
    if (g_compl[(c - PROC_BASE) / 4u]) g_compl[(c - PROC_BASE) / 4u](pb);
    pbf_in_int--;
}

void mt_os_asr(const MtStreamAsr *a, uint16_t event, uint16_t reason, MacPtr32 icmp) {
    pbf_ev e;
    MacPtr32 c = a->proc;
    memset(&e, 0, sizeof e);
    e.kind = 'A';
    e.stream = a->stream;
    e.ev = event;
    e.reason = reason;
    e.user = a->user;
    e.icmp = icmp;
    log_add(&e);
    if (c < PROC_BASE + 0x1000u || c >= PROC_BASE + 0x1000u + 4u * NPROC || (c & 3u)) {
        pbf_bad++;                              /* the glue must never call ASR 0 or garbage */
        return;
    }
    pbf_in_int++;
    if (g_asr[(c - PROC_BASE - 0x1000u) / 4u])
        g_asr[(c - PROC_BASE - 0x1000u) / 4u](a->stream, event, a->user, reason, icmp);
    pbf_in_int--;
}

int mt_os_irq_save(void) {
    int prev = pbf_irq;
    pbf_irq++;
    pbf_irq_calls++;
    if (pbf_irq > pbf_irq_max) pbf_irq_max = pbf_irq;
    return prev;
}

void mt_os_irq_restore(int saved) {
    if (saved != pbf_irq - 1) pbf_bad++;        /* restores must nest */
    pbf_irq = saved;
}

int mt_os_in_interrupt(void) { return pbf_in_int > 0; }

/* ---- harness ---- */

static void compl_nop(MtAnyPb *pb) { (void)pb; }
static MacPtr32 g_nop;

void pbt_init(pbt *x, int configured) {
    mt_limits lim;
    mt_link link;
    mt_ifconfig c;
    uint32_t n;
    void *mem;
    pbf_reset();
    g_nop = pbf_compl(compl_nop);
    mt_limits_default(&lim);
    fake_init(&x->fl, &link);
    n = mt_pb_size(&lim);
    mem = pbf_alloc(n);
    x->d = mt_pb_init(mem, n, &lim, &link, 1);
    if (!x->d) return;
    memset(&c, 0, sizeof c);
    c.addr = configured ? T_ME : 0;
    c.mask = 0xFFFFFF00u;
    c.gateway = T_GW;
    c.mtu = 1500;
    memcpy(c.mac, T_ME_MAC, 6);
    mt_pb_set_config(x->d, &c);
    if (configured) mt_arp_insert(x->d->s, T_PEER, T_PEER_MAC, 0);
}

TCPiopb *pbt_pb(int16_t cs, StreamPtr s) {
    TCPiopb *pb = (TCPiopb *)pbf_alloc(sizeof(TCPiopb));
    if (!pb) return NULL;
    pb->ioCompletion = g_nop;
    pb->csCode = cs;
    pb->tcpStream = s;
    return pb;
}

int16_t pbt_call(pbt *x, TCPiopb *pb) {
    int16_t r = mt_pb_control(x->d, (MtAnyPb *)pb);
    if (r != inProgress && r != MT_PB_NOTOPEN)            /* mt_pb.h result contract 3 and 4 */
        mt_os_complete((MtAnyPb *)pb, r);
    return r;
}

int pbf_count(char kind) {
    int i, n = 0;
    for (i = 0; i < pbf_nlog && i < PBF_LOG; i++) n += pbf_log[i].kind == kind;
    return n;
}

int pbf_find(char kind, MacPtr32 x, int from) {
    int i;
    for (i = from; i < pbf_nlog && i < PBF_LOG; i++)
        if (pbf_log[i].kind == kind && (kind == 'C' ? pbf_log[i].pb : pbf_log[i].stream) == x)
            return i;
    return -1;
}

StreamPtr pbt_create(pbt *x, uint32_t buflen, MacPtr32 asr, MacPtr32 user) {
    TCPiopb *pb = pbt_pb(TCPCreate, 0);
    uint8_t *buf = (uint8_t *)pbf_alloc(buflen);
    if (!pb || !buf) return 0;
    pb->csParam.create.rcvBuff = pbf_a(buf);
    pb->csParam.create.rcvBuffLen = buflen;
    pb->csParam.create.notifyProc = asr;
    pb->csParam.create.userDataPtr = user;
    if (pbt_call(x, pb) != 0) return 0;
    return pb->tcpStream;
}

int pbt_last_tcp(pbt *x, tcpv *v) {
    int i;
    for (i = x->fl.n - 1; i >= 0; i--)
        if (view_tcp(x->fl.f[i], x->fl.len[i], v)) return 1;
    return 0;
}

void pbt_peer(pbt *x, uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags, uint16_t win,
              const void *data, uint16_t len) {
    static const uint8_t mss_opt[4] = {2, 4, 0x05, 0xB4};
    uint8_t f[1600];
    uint16_t n = mk_tcp(f, T_PEER, PBT_PEER_PORT, T_ME, dport, seq, ack, flags, win,
                        (flags & TF_SYN) ? mss_opt : NULL, (flags & TF_SYN) ? 4 : 0,
                        (const uint8_t *)data, len);
    mt_pb_input(x->d, f, n);
}

TCPiopb *pbt_open(pbt *x, StreamPtr s, uint16_t lport) {
    TCPiopb *pb = pbt_pb(TCPActiveOpen, s);
    if (!pb) return NULL;
    pb->csParam.open.remoteHost = T_PEER;
    pb->csParam.open.remotePort = PBT_PEER_PORT;
    pb->csParam.open.localPort = lport;
    return pbt_call(x, pb) == inProgress ? pb : NULL;
}

int pbt_connect(pbt *x, StreamPtr s, pbt_conn *c) {
    TCPiopb *pb;
    tcpv v;
    fake_clear(&x->fl);
    pb = pbt_open(x, s, 0);
    if (!pb || !pbt_last_tcp(x, &v) || v.flags != TF_SYN) return -1;
    c->lport = v.sport;
    c->ps = 5000;
    c->ms = v.seq + 1;
    pbt_peer(x, c->lport, c->ps, c->ms, TF_SYN | TF_ACK, 8192, NULL, 0);
    c->ps++;
    fake_clear(&x->fl);
    return pb->ioResult == 0 ? 0 : -2;
}

StreamPtr pbt_est(pbt *x, MacPtr32 asr, MacPtr32 user, pbt_conn *c) {
    StreamPtr s = pbt_create(x, 8192, asr, user);
    if (!s || pbt_connect(x, s, c) != 0) return 0;
    return s;
}

TCPiopb *pbt_send(pbt *x, StreamPtr s, const void *data, uint16_t len, int push) {
    TCPiopb *pb = pbt_pb(TCPSend, s);
    wdsEntry *w = (wdsEntry *)pbf_alloc(2 * sizeof(wdsEntry));
    uint8_t *b = (uint8_t *)pbf_alloc(len ? len : 1);
    wdsEntry e[2];
    if (!pb || !w || !b) return NULL;
    memcpy(b, data, len);
    e[0].length = len; e[0].ptr = pbf_a(b);
    e[1].length = 0; e[1].ptr = 0;
    memcpy(w, e, sizeof e);
    pb->csParam.send.wdsPtr = pbf_a(w);
    pb->csParam.send.pushFlag = (uint8_t)push;
    pbt_call(x, pb);
    return pb;
}

TCPiopb *pbt_rcv(pbt *x, StreamPtr s, uint16_t len, int8_t timeout_s) {
    TCPiopb *pb = pbt_pb(TCPRcv, s);
    uint8_t *b = (uint8_t *)pbf_alloc(len ? len : 1);
    if (!pb || !b) return NULL;
    pb->csParam.receive.rcvBuff = pbf_a(b);
    pb->csParam.receive.rcvBuffLen = len;
    pb->csParam.receive.commandTimeoutValue = timeout_s;
    pbt_call(x, pb);
    return pb;
}

int16_t pbt_simple(pbt *x, int16_t cs, StreamPtr s) {
    TCPiopb *pb = pbt_pb(cs, s);
    if (!pb) return -9999;
    return pbt_call(x, pb);
}
