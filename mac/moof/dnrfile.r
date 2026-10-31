/* "MacTCP DNR": the file apps look the resolver up in (type cdev, creator mtcp, in the System
 * Folder; the real one is created by MacTCP). Only `dnrp` 0 and a version: Plan 3's installer
 * creates it from the cdev's own copy. */
#include "MacTypes.r"
#include "dnrp.r"

resource 'vers' (1) {
    0x00, 0x10, development, 0x00, verUS,
    "0.1d0",
    "MoofTCP DNR 0.1d0 (interim, Plan 2)"
};
