#ifndef DUSTY_COMMANDS_H
#define DUSTY_COMMANDS_H

#include <stdbool.h>

int dusty_delete_world(const char *save_directory, const char *name);
int dusty_export_c(const char *input, const char *output, const char *symbol);
int dusty_export_driver(const char *driver);
int dusty_driver(const char *source, const char *compiler, bool run);

#endif
