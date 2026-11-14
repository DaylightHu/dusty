#ifndef MC_WORKSPACE_TERMINAL_H
#define MC_WORKSPACE_TERMINAL_H

#include "raylib.h"
#include <stddef.h>

typedef struct WorkspaceTerminal WorkspaceTerminal;
typedef void (*WorkspaceRunRequest)(void *context, const char *source, int reply, int output);

WorkspaceTerminal *workspace_terminal_open(const char *edit_path, Rectangle box, char *error, size_t capacity);
void workspace_terminal_close(WorkspaceTerminal *terminal);
void workspace_terminal_poll(WorkspaceTerminal *terminal, WorkspaceRunRequest run, void *context);
void workspace_terminal_draw(WorkspaceTerminal *terminal, Rectangle box, bool focused, float background_opacity);
void workspace_terminal_focus(WorkspaceTerminal *terminal, bool focused);
void workspace_terminal_input(WorkspaceTerminal *terminal);
void workspace_terminal_pointer(WorkspaceTerminal *terminal, Rectangle box, Vector2 point);
bool workspace_terminal_exited(const WorkspaceTerminal *terminal);
void workspace_terminal_reply(int fd, const char *error);
int workspace_terminal_request_run(const char *source);

#endif
