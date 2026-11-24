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
CONCRETE_DEF(black);
CONCRETE_DEF(brown);
CONCRETE_DEF(cyan);
CONCRETE_DEF(light_blue);
CONCRETE_DEF(light_gray);
CONCRETE_DEF(lime);
CONCRETE_DEF(magenta);
CONCRETE_DEF(orange);
CONCRETE_DEF(pink);
CONCRETE_DEF(red);
