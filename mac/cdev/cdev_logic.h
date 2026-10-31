/* cdev_logic.h - the control panel's decisions, without the Toolbox (Plan 3 Task 8 fix round 1).
 *
 * Everything here is pure and host-tested (tests/test_cdev_logic.c); cdev_main.c is the Toolbox
 * shell around it. Strings are built as Pascal strings (length byte first, at most 255 bytes).
 *
 *  - Messages: cdl_problem_text gives the sentence for a cfe_validate result.
 *  - The check line: cdl_check_key is what the line shows; redraw only when the key changes. The IP
 *    message depends on whether the IP field is empty ("required" vs "invalid"), so that is part
 *    of the key.
 *  - Field caps: 15 characters for the four addresses, 63 for the domain (cdl_field_cap); a typed
 *    or pasted character that would exceed the cap is refused (cdl_insert_ok).
 *  - Save / close decisions: cdl_save_action (Save button) and cdl_close_action (closeDev).
 *  - The draft: edits the user could not save (invalid at close) are kept in resource 'MCFD' 128
 *    of "MoofTCP Prefs" (cdl_draft_encode/decode) and offered again at the next initDev. The driver
 *    reads only 'MCFG' 128, so a draft never changes the running configuration. */
#ifndef CDEV_LOGIC_H
#define CDEV_LOGIC_H

#include <stddef.h>
#include <stdint.h>
#include "cfg_edit.h"

/* The Control Panel's Pascal arguments as they lie on the stack above the return address
 * (cdev_start.s passes their address): CPDialog, cdevStorage, theEvent, CPanelID, numItems,
 * item, message. Pointers are 4 bytes on the 68k. */
typedef struct {
    void *dlg;             /* DialogPtr */
    long storage;
    void *ev;              /* EventRecord * */
    short panel_id;
    short num_items;
    short item;
    short message;
} CdevArgs;

#if defined(__m68k__)
/* The layout cdev_start.s relies on: a mismatch is a compile error (negative array size). */
typedef char cdl_args_dlg[offsetof(CdevArgs, dlg) == 0 ? 1 : -1];
typedef char cdl_args_storage[offsetof(CdevArgs, storage) == 4 ? 1 : -1];
typedef char cdl_args_ev[offsetof(CdevArgs, ev) == 8 ? 1 : -1];
typedef char cdl_args_panel[offsetof(CdevArgs, panel_id) == 12 ? 1 : -1];
typedef char cdl_args_num[offsetof(CdevArgs, num_items) == 14 ? 1 : -1];
typedef char cdl_args_item[offsetof(CdevArgs, item) == 16 ? 1 : -1];
typedef char cdl_args_msg[offsetof(CdevArgs, message) == 18 ? 1 : -1];
typedef char cdl_args_size[sizeof(CdevArgs) == 20 ? 1 : -1];
#else
/* Host: pointer sizes differ; the order of the members must still be the stack order. */
typedef char cdl_args_order[(offsetof(CdevArgs, dlg) < offsetof(CdevArgs, storage) &&
                             offsetof(CdevArgs, storage) < offsetof(CdevArgs, ev) &&
                             offsetof(CdevArgs, ev) < offsetof(CdevArgs, panel_id) &&
                             offsetof(CdevArgs, panel_id) + 2 == offsetof(CdevArgs, num_items) &&
                             offsetof(CdevArgs, num_items) + 2 == offsetof(CdevArgs, item) &&
                             offsetof(CdevArgs, item) + 2 == offsetof(CdevArgs, message)) ? 1 : -1];
#endif

/* field index, as CFE_F_*: 1 IP .. 5 domain */
int cdl_field_cap(int field);                 /* 15 or 63; 0 for an unknown field */
/* 1 if inserting n characters into a field of cap with cur characters, sel of them selected
 * (replaced), stays within the cap. */
int cdl_insert_ok(int cur, int sel, int n, int cap);
/* 1 for a key that inserts a character (not a control/arrow key, not delete). */
int cdl_key_inserts(unsigned char c);

void cdl_pcat(unsigned char *ps, const char *c);          /* append a C string, capped at 255 */
int cdl_fields_equal(const CfeFields *a, const CfeFields *b);

void cdl_problem_text(const CfeFields *f, int rc, const CfeError *e, unsigned char *ps);
int cdl_check_key(const CfeFields *f, int rc, const CfeError *e);   /* 0 = nothing to show */

/* Save button: */
enum { CDL_SAVE_WRITE = 1,     /* valid: write */
       CDL_SAVE_ASK_WARN,      /* valid with the off-subnet warning: ask, then write */
       CDL_SAVE_REFUSE };      /* invalid: alert naming the field, select it, keep the text */
int cdl_save_action(int rc);

/* closeDev: */
enum { CDL_CLOSE_NOTHING = 1,  /* nothing changed (or the same invalid draft is already kept) */
       CDL_CLOSE_DROP_DRAFT,   /* the fields are back to the saved values: delete a stale draft */
       CDL_CLOSE_ASK,          /* valid unsaved edits: ask Save / Don't Save (closeDev cannot cancel) */
       CDL_CLOSE_KEEP_DRAFT }; /* invalid unsaved edits: alert, keep them as the draft */
/* draft: the draft read at initDev, NULL if there was none. rc: cfe_validate(cur). */
int cdl_close_action(const CfeFields *cur, const CfeFields *saved, const CfeFields *draft, int rc);

/* The draft resource: version byte 1, then the five fields as Pascal strings. Returns the length,
 * 0 if cap is too small. decode: 0 on success, -1 if not a usable draft (f all empty). */
uint32_t cdl_draft_encode(const CfeFields *f, uint8_t *out, uint32_t cap);
int cdl_draft_decode(const uint8_t *p, uint32_t n, CfeFields *f);

#endif
