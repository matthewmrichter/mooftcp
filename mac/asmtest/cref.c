/* cref.c - the portable C checksum/copy of core/mt_util.c under the names cref_cksum_add and
 * cref_cksum_copy, the reference AsmTest compares the asm against (MT_ASM_CKSUM is not
 * defined here, so the C bodies are compiled; the rest of mt_util.c comes along unused). */
#define mt_cksum_add cref_cksum_add
#define mt_cksum_copy cref_cksum_copy
#include "mt_util.c"
