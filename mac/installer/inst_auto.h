/* inst_auto.h - the unattended installer's config file "MoofTCPInstall.cfg" (portable, host-tested:
 * tests/test_inst_auto.c). One line of lower-case words:
 *     install [lite] [prefs]      Install (the MoofTCP file this installer holds for the variant)
 *     remove                      Remove
 *     reinstall [lite] [prefs]    Remove, then Install: how a candidate build replaces another of any version
 * `prefs`: first copy the file "MoofTCP Prefs" from the installer's own folder over the System Folder's.
 * `lite` and `prefs` may come in either order, each at most once. The line ends at CR, LF or the end of
 * the buffer; after it only blanks and line ends may follow. */
#ifndef INST_AUTO_H
#define INST_AUTO_H

#include <stdint.h>

enum { IA_NONE = 0, IA_INSTALL, IA_REMOVE, IA_REINSTALL };

typedef struct {
    uint8_t action;      /* IA_* */
    uint8_t lite;        /* 1: the Lite build */
    uint8_t prefs;       /* 1: copy "MoofTCP Prefs" first */
} InstAuto;

/* 0 = ok (*a filled), -1 = bad or empty (*a is IA_NONE). */
int inst_auto_parse(const char *buf, long len, InstAuto *a);

#endif
