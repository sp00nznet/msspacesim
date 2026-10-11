/* No companion program: what every program but spacesim links (see
 * lifted.h, Program, and src/companion_ss1.c). In the static library, so a
 * program that defines its own companion() never pulls this in. */
#include "lifted.h"

const Program *companion(void) { return 0; }
