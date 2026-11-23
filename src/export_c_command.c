#include "dusty_commands.h"
#include "mcmux.h"

#include <ctype.h>
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

static char *skip_space(char *text)
{
    while (isspace((unsigned char)*text)) ++text;
    return text;
}

static char *model_argument(char **cursor)
{
    char *value = skip_space(*cursor);
    if (!*value) return NULL;

    char *end = value;
    if (*value == '"' || *value == '\'') {
        char quote = *value++;
        end = strchr(value, quote);
        if (!end || end == value || (end[1] && !isspace((unsigned char)end[1])))
            return NULL;
    } else {
        while (*end && !isspace((unsigned char)*end)) ++end;
    }
    if (*end) *end++ = '\0';
    *cursor = end;
    return value;
}

int dusty_export_driver(const char *driver)
{
    size_t length = strlen(driver);
    if (length < 3 || driver[length - 2] != '.' ||
        tolower((unsigned char)driver[length - 1]) != 'c') {
        fputs("One argument must be a C driver. To export a structure, "
              "use dusty export-c INPUT.mcs OUTPUT.c [SYMBOL].\n", stderr);
        return 2;
    }
    FILE *source = fopen(driver, "r");
    if (!source) {
        fprintf(stderr, "Cannot read driver '%s': %s\n", driver, strerror(errno));
        return 1;
    }

    static const char marker[] = "mcmux-model:";
    char line[4096];
    char annotation[sizeof line];
    bool found = false;
    int status = 0;
    while (fgets(line, sizeof line, source)) {
        char *value = skip_space(line);
        bool is_model = false;
        if (value[0] == '/' && value[1] == '/') {
            value = skip_space(value + 2);
            is_model = strncmp(value, marker, sizeof marker - 1) == 0;
        }
        if (!strchr(line, '\n') && !feof(source)) {
            int next = fgetc(source);
            if (next != '\n' && next != EOF) {
                if (is_model) {
                    fprintf(stderr, "%s: model declaration is too long\n", driver);
                    status = 2;
                    break;
                }
                do { next = fgetc(source); } while (next != '\n' && next != EOF);
            }
        }
        if (!is_model) continue;
        if (found) {
            fprintf(stderr, "%s: expected only one // mcmux-model: declaration\n", driver);
            status = 2;
            break;
        }
        strcpy(annotation, value + sizeof marker - 1);
        found = true;
    }
    bool read_failed = ferror(source) != 0;
    if (fclose(source) != 0) read_failed = true;
    if (read_failed) {
        fprintf(stderr, "Cannot read driver '%s'\n", driver);
        return 1;
    }
    if (status) return status;

    char *cursor = annotation;
    if (found) {
        char *input = model_argument(&cursor);
        char *output = input ? model_argument(&cursor) : NULL;
        char *symbol = output ? model_argument(&cursor) : NULL;
        if (symbol && !*skip_space(cursor)) return dusty_export_c(input, output, symbol);
    }
    fprintf(stderr, "%s: expected // mcmux-model: INPUT.mcs OUTPUT.c SYMBOL\n", driver);
    return 2;
}
