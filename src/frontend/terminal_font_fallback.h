#ifndef MC_TERMINAL_FONT_FALLBACK_H
#define MC_TERMINAL_FONT_FALLBACK_H

#include "raylib.h"

void terminal_font_fallback_prepare(const char *path, int size, unsigned revision);
void terminal_font_fallback_unload(void);
bool terminal_font_fallback_glyph(int codepoint, int max_width, int max_height,
                                  Texture2D *atlas, Rectangle *source);

#endif
