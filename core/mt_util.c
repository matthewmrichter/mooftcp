#include "mt_util.h"

/* All utilities are static inline in mt_util.h; this unit keeps the header
 * compiled under the strict flags and gives mtcore a source from day one. */

uint32_t mt_cksum_add(uint32_t sum, const uint8_t *p, uint32_t len) {
    while (len > 1) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len) sum += (uint32_t)p[0] << 8;
    return sum;
}

uint16_t mt_cksum_fold(uint32_t sum) {
    sum = (sum & 0xFFFFu) + (sum >> 16);
    sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)~sum;
}

uint32_t mt_cksum_odd(uint32_t part) {
    part = (part & 0xFFFFu) + (part >> 16);
    part = (part & 0xFFFFu) + (part >> 16);
    return ((part << 8) | (part >> 8)) & 0xFFFFu;
}

uint32_t mt_cksum_copy(uint32_t sum, uint8_t *dst, const uint8_t *src, uint32_t len) {
    while (len > 1) {
        uint8_t a = src[0], b = src[1];
        dst[0] = a;
        dst[1] = b;
        sum += ((uint32_t)a << 8) | b;
        src += 2;
        dst += 2;
        len -= 2;
    }
    if (len) {
        dst[0] = src[0];
        sum += (uint32_t)src[0] << 8;
    }
    return sum;
}

void mt_ring_init(mt_ring *r, uint8_t *mem, uint32_t size) {
    r->base = mem;
    r->size = size;
    r->head = 0;
    r->used = 0;
}

uint32_t mt_ring_free(const mt_ring *r) { return r->size - r->used; }

/* Copy n bytes into the ring starting at physical index pos (< size), wrapping. */
static void ring_put(mt_ring *r, uint32_t pos, const uint8_t *p, uint32_t n) {
    uint32_t first = r->size - pos, i;
    if (first > n) first = n;
    for (i = 0; i < first; i++) r->base[pos + i] = p[i];
    for (i = first; i < n; i++) r->base[i - first] = p[i];
}

uint32_t mt_ring_write_at(mt_ring *r, uint32_t off, const uint8_t *p, uint32_t n) {
    uint32_t fr = mt_ring_free(r), pos;
    if (off >= fr) return 0;
    if (n > fr - off) n = fr - off;
    pos = r->head + r->used;
    if (pos >= r->size) pos -= r->size;
    pos += off;
    if (pos >= r->size) pos -= r->size;
    ring_put(r, pos, p, n);
    return n;
}

uint32_t mt_ring_write(mt_ring *r, const uint8_t *p, uint32_t n) {
    n = mt_ring_write_at(r, 0, p, n);
    r->used += n;
    return n;
}

uint32_t mt_ring_peek(const mt_ring *r, uint32_t off, const uint8_t **p) {
    uint32_t pos, run;
    if (off >= r->used) { *p = r->base; return 0; }
    pos = r->head + off;
    if (pos >= r->size) pos -= r->size;
    run = r->size - pos;
    if (run > r->used - off) run = r->used - off;
    *p = r->base + pos;
    return run;
}

void mt_ring_consume(mt_ring *r, uint32_t n) {
    if (n > r->used) n = r->used;
    r->used -= n;
    r->head += n;
    if (r->head >= r->size) r->head -= r->size;
}

uint32_t mt_ring_read(mt_ring *r, uint8_t *dst, uint32_t n) {
    uint32_t got = 0;
    while (got < n) {
        const uint8_t *p;
        uint32_t run = mt_ring_peek(r, 0, &p), i;
        if (run == 0) break;
        if (run > n - got) run = n - got;
        for (i = 0; i < run; i++) dst[got + i] = p[i];
        got += run;
        mt_ring_consume(r, run);
    }
    return got;
}
