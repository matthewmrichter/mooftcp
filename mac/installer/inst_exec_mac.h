/* inst_exec_mac.h - the Toolbox InstFs table (inst_exec_mac.c; Plan 3 Task 10). */
#ifndef INST_EXEC_MAC_H
#define INST_EXEC_MAC_H

#include "inst_exec.h"

typedef struct { short vref; } InstMacCtx;

enum { INST_MAC_NOT_HFS = 1, INST_MAC_NO_SYSFOLDER = 2 };

/* Fills *fs for the startup disk's blessed System Folder (c holds the volume). 0, an OSErr, or
 * INST_MAC_NOT_HFS / INST_MAC_NO_SYSFOLDER. */
int inst_mac_fs(InstFs *fs, InstMacCtx *c);

#endif
