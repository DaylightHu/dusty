#ifndef MC_WORKSPACE_DRIVER_H
#define MC_WORKSPACE_DRIVER_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int process, reply;
    bool cancelling;
    double deadline;
} WorkspaceDriver;

bool workspace_driver_validate(const char *source, char *error, size_t capacity);
void workspace_driver_reject(int reply, int output, const char *error);
bool workspace_driver_start(WorkspaceDriver *driver, const char *source, int reply, int output);
void workspace_driver_poll(WorkspaceDriver *driver, double now);
void workspace_driver_cancel(WorkspaceDriver *driver, double now);
void workspace_driver_close(WorkspaceDriver *driver);

#endif
