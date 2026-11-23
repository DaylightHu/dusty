#ifndef MC_GUI_H
#define MC_GUI_H

#include "rs/session.h"

/* The caller owns the loaded session for the duration of the viewer. */
int gui_run(GameSession *session, const char *world_name);
const char *gui_asset_path(const char *relative);

#endif
