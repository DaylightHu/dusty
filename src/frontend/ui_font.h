#ifndef MC_UI_FONT_H
#define MC_UI_FONT_H

#include "raylib.h"

bool ui_font_init(void);
void ui_font_unload(void);
void ui_font_set_scale(float scale);
void ui_text_draw(const char *text, int x, int y, int size, Color color);
int ui_text_width(const char *text, int size);

int ui_terminal_cell_width(int size, const char *path);
void ui_terminal_glyph(int codepoint, Rectangle cell, int size,
                       bool bold, bool italic, Color color);
void ui_terminal_font_unload(void);
#endif
