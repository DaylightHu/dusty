#include "block_internal.h"

/* All concrete colors have the same solid, conductive behavior. */
#define CONCRETE_DEF(color) \
    const BlockDef rs_##color##_concrete_def = { \
        .name = #color "_concrete", \
        .full_cube = true, \
        .conductor = true, \
        .supports_wire = true, \
        .push_reaction = PUSH_NORMAL \
    }

CONCRETE_DEF(blue);
CONCRETE_DEF(gray);
CONCRETE_DEF(green);
CONCRETE_DEF(purple);
CONCRETE_DEF(yellow);
