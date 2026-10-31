/* mt_stackmark.h - interrupt-time stack depth measurement (Plan 3 Task 1).
 *
 * Interrupt-time code (the protocol handler, the Time Manager task, completion routines and ASRs
 * called from them) runs on the interrupted application's stack. MtStackMark, kept at the end of
 * MtRes, records how deep the driver goes:
 *   low    lowest stack pointer seen at any mark (absolute address, 0 = none yet)
 *   entry  stack pointer at the outermost entry (enter with nest == 0) of the episode that reached
 *          the greatest depth
 *   used   largest (episode entry - sp) seen at any mark: the stack the driver, its call-outs and
 *          nested driver calls took below that entry. MoofDiag prints low, entry and used.
 * An episode is one outermost entry (gate poke from the handler, gate tick from the Time Manager
 * task, or an application Control call) up to its matching leave; nested entries only deepen it.
 * The counters are single-owner volatile words: a mark only lowers/raises its own figure, and an
 * interrupt that nests enter/leave inside a mark leaves nest unchanged, so no mask is needed.
 *
 * MT_STACK_PAINT=N (measurement builds only, e.g. MT_EXTRA_CFLAGS=-DMT_STACK_PAINT=4096 for
 * scripts/build-mac.sh): the outermost enter also paints N bytes below the stack pointer with a
 * pattern (interrupts masked for the loop, about 3 ms for 4096 bytes on a 68000), the matching leave
 * scans for the lowest overwritten word and `deep` keeps the largest (entry - lowest) seen. The marks
 * above only see the stack pointer at their call sites, `deep` also sees the leaf frames between them.
 *
 * Host builds, and MT_STACK_MARK=0 builds, get empty inline functions. */
#ifndef MT_STACKMARK_H
#define MT_STACKMARK_H

#include <stdint.h>

#ifndef MT_STACK_MARK
#define MT_STACK_MARK 1
#endif
#ifndef MT_STACK_PAINT
#define MT_STACK_PAINT 0
#endif

typedef struct MtStackMark {
    volatile uint32_t low;
    volatile uint32_t entry;
    volatile uint32_t used;
    volatile uint32_t base;      /* sp at the current episode's outermost entry */
    volatile uint32_t n_enter;   /* episodes counted */
    volatile uint32_t deep;      /* MT_STACK_PAINT builds: deepest stack word touched below the entry (bytes) */
    volatile uint32_t paint_lo;  /* lowest painted address of the current episode (0 = not painted) */
    volatile uint16_t nest;
    uint16_t          pad;
} MtStackMark;

#if MT_STACK_MARK && defined(MT_OS_MAC)
#ifdef __cplusplus
extern "C" {
#endif
void mt_os_stack_mark(void);            /* record the stack pointer (safe at interrupt time) */
void mt_os_stack_enter(void);           /* outermost entry starts an episode; also marks */
void mt_os_stack_leave(void);
void mt_os_mac_set_stack(MtStackMark *m);   /* NULL unregisters */
#ifdef __cplusplus
}
#endif
#else
static inline void mt_os_stack_mark(void) {}
static inline void mt_os_stack_enter(void) {}
static inline void mt_os_stack_leave(void) {}
#endif

#endif
