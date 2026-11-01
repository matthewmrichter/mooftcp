/* diag_streams.h - MoofDiag's stream and owner listing (diag_streams.c; Plan 3 Task 12). */
#ifndef DIAG_STREAMS_H
#define DIAG_STREAMS_H
/* drv = the driver's MtDrv (MtRes.d); one call of line() per text line (CR terminated). */
void diag_streams(const void *drv, void (*line)(const char *s));
#endif
