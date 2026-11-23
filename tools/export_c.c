#include "../src/dusty_commands.h"

#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s INPUT.mcs OUTPUT.c [SYMBOL]\n", argv[0]);
        return 2;
    }
    return dusty_export_c(argv[1], argv[2], argc == 4 ? argv[3] : NULL);
}
