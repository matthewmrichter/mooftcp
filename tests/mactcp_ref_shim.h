/* Small stand-ins for the Mac Types.h names the reference MacTCP header needs, so
 * the reference header can be compiled for layout only (test_mactcp_layout.c
 * feeds this and the reference header to "gcc -m32 -ffreestanding -S").
 * -m32 gives 4-byte pointers and longs like the 68k; pack(2) gives the 68k
 * 2-byte struct alignment. Nothing here is used at run time. */
#ifndef MACTCP_REF_SHIM_H
#define MACTCP_REF_SHIM_H

#pragma pack(2)

#define __TYPES__ 1               /* the reference header skips its own Types.h include */
#define pascal                    /* Pascal calling convention keyword: irrelevant here */
#define offsetof(t, m) __builtin_offsetof(t, m)

typedef unsigned char Boolean;
typedef char *Ptr;
typedef unsigned char *StringPtr;
typedef short OSErr;
struct QElem;
typedef struct QElem QElem;

#endif
