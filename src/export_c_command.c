#include "dusty_commands.h"
#include "mcmux.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#define export_process_id _getpid
#else
#include <unistd.h>
#define export_process_id getpid
#endif

static bool publish_model(const char *temporary, const char *output)
{
#ifdef _WIN32
    return MoveFileExA(temporary, output, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return rename(temporary, output) == 0;
#endif
}

int dusty_export_c(const char *input, const char *output, const char *symbol)
{
    if (!symbol) symbol = "mcmux_circuit";
    size_t length = strlen(output);
    if (length < 3 || strcmp(output + length - 2, ".c") != 0) {
        fprintf(stderr, "output must end in .c\n");
        return 2;
    }
    StructureResult status;
    Structure *structure = structure_read(input, &status);
    if (!structure) {
        fprintf(stderr, "cannot read structure: %s\n", structure_result_name(status));
        return 1;
    }
    StructureInfo info = structure_info(structure);
    structure_destroy(structure);
    int64_t x = (int64_t)info.origin.x + info.width - 1;
    int64_t y = (int64_t)info.origin.y + info.height - 1;
    int64_t z = (int64_t)info.origin.z + info.length - 1;
    if (x > INT_MAX || y > INT_MAX || z > INT_MAX) return 1;
    World *world = structure_load(input, &status);
    if (!world) {
        fprintf(stderr, "cannot load structure: %s\n", structure_result_name(status));
        return 1;
    }
    size_t capacity = length + 32;
    char *temporary = malloc(capacity);
    if (!temporary) {
        world_destroy(world);
        fputs("Cannot allocate the output path\n", stderr);
        return 1;
    }
    snprintf(temporary, capacity, "%s.tmp.%lu", output, (unsigned long)export_process_id());
    FILE *out = fopen(temporary, "wx");
    if (!out) {
        fprintf(stderr, "Cannot write '%s': %s\n", temporary, strerror(errno));
        free(temporary);
        world_destroy(world);
        return 1;
    }
    RsResult result = sim_write_c(world, info.origin, (BlockPos){(int)x,(int)y,(int)z},
                                  out, symbol);
    bool ok = result == RS_OK;
    if (fclose(out) != 0) ok = false;
    world_destroy(world);
    if (ok) ok = publish_model(temporary, output);
    if (!ok) {
        (void)remove(temporary);
        fprintf(stderr, "C export failed: %s\n",
                rs_result_name(result == RS_OK ? RS_IO_ERROR : result));
        free(temporary);
        return 1;
    }
    free(temporary);
    printf("generated %s (model: %s)\n", output, symbol);
    return 0;
}
