/* mt_os_mac.h - registration of the gate used by mt_os_irq_save / mt_os_irq_restore. */
#ifndef MT_OS_MAC_H
#define MT_OS_MAC_H

#include "mt_gate.h"
#include "mt_stackmark.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Call once before the first mt_os_irq_save (the driver does it at open); NULL unregisters. */
void mt_os_mac_set_gate(MtGate *g);

#ifdef __cplusplus
}
#endif

#endif
