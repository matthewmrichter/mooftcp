/* dnrp.r - the resolver resource, shared by the "MacTCP" cdev file (moof.r) and the file
 * "MacTCP DNR" (dnrfile.r): `dnrp` 0 named "DNR", locked, application heap (attr 0x10, as the real
 * one's). The code is mac/dnr (flat Retro68 block, entry at offset 0 = Retro68's trampoline). */
#include "Retro68.r"

type 'dnrp' {
    RETRO68_CODE_TYPE
};

resource 'dnrp' (0, "DNR", locked) {
    dontBreakAtEntry, $$read("../dnr/MoofDnr.flt");
};
