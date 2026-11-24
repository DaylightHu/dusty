#if defined(__APPLE__)
#define _XOPEN_SOURCE 700
#endif

#include "frontend/gui.h"
#include "dusty_commands.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include "raylib.h"
#include <limits.h>

static bool configure_mac_bundle(void)
{
    const char *directory = GetApplicationDirectory();
    const char suffix[] = "/Contents/MacOS/";
    size_t length = strlen(directory), suffix_length = sizeof(suffix) - 1;
    if (length < suffix_length || strcmp(directory + length - suffix_length, suffix) != 0)
        return true;

    /* Repository launches use the working tree's saves. */
    if (FileExists("nob.c")) return true;

    const char *user_home = getenv("HOME");
    if (!user_home || !*user_home) return false;
    char data_directory[PATH_MAX];
    int size = snprintf(data_directory, sizeof(data_directory),
                        "%s/Library/Application Support/mcmux", user_home);
    return size > 0 && (size_t)size < sizeof(data_directory) &&
           MakeDirectory(data_directory) == 0 && ChangeDirectory(data_directory);
}
#endif

static const char save_directory[] = "saves";

static void print_usage(FILE *output)
{
    fputs("Voxilog - redstone circuit workbench\n"
          "Usage:\n"
          "  voxi list         List saved worlds\n"
          "  voxi create NAME  Create a world without opening a window\n"
          "  voxi load NAME    Load a world and open its 3D view\n"
          "  voxi delete NAME  Permanently delete a world and its directory\n"
          "  voxi run-c DRIVER.c  Build and run a C driver from the project root\n"
          "  voxi export-c DRIVER.c  Export the model declared in a C driver\n"
          "  voxi export-c INPUT.mcs OUTPUT.c [SYMBOL]  Export a C circuit model\n"
          "  voxi --help       Show this help\n", output);
}

static int world_error(const char *action, const char *name, GameSessionResult result)
{
    fprintf(stderr, "Cannot %s world '%s': %s\n", action, name,
            game_session_result_name(result));
    return EXIT_FAILURE;
}

static int list_worlds(void)
{
    GameSaveList saves;
    GameSessionResult result = game_session_list_saves(save_directory, &saves);
    if (result != GAME_SESSION_OK && result != GAME_SESSION_TOO_MANY_SAVES) {
        fprintf(stderr, "Cannot list worlds: %s\n", game_session_result_name(result));
        return EXIT_FAILURE;
    }

    for (size_t i = 0; i < saves.count; ++i) puts(saves.names[i]);
    if (result == GAME_SESSION_TOO_MANY_SAVES)
        fprintf(stderr, "Only the first %d saves are listed\n", GAME_SESSION_MAX_SAVES);
    return EXIT_SUCCESS;
}

static bool names_equal(const char *left, const char *right)
{
    while (*left && *right) {
        if (tolower((unsigned char)*left++) != tolower((unsigned char)*right++))
            return false;
    }
    return *left == *right;
}

static int create_world(const char *name, const char *path)
{
    GameSaveList saves;
    GameSessionResult result = game_session_list_saves(save_directory, &saves);
    if (result != GAME_SESSION_OK && result != GAME_SESSION_TOO_MANY_SAVES)
        return world_error("create", name, result);

    for (size_t i = 0; i < saves.count; ++i) {
        if (names_equal(saves.names[i], name)) {
            fprintf(stderr, "World '%s' already exists\n", name);
            return EXIT_FAILURE;
        }
    }

    GameSession *session = game_session_create(path, world_config_default(), &result);
    if (!session) return world_error("create", name, result);

    // Reserve this directory before saving so simultaneous creates cannot
    // overwrite a save, including an unlisted or unreadable one.
    char directory[GAME_SESSION_PATH_CAPACITY];
    size_t length = (size_t)(strrchr(path, '/') - path);
    memcpy(directory, path, length);
    directory[length] = '\0';
#ifdef _WIN32
    int created = _mkdir(directory);
#else
    int created = mkdir(directory, 0755);
#endif
    if (created != 0) {
        fprintf(stderr, "Cannot create world directory '%s': %s\n", directory, strerror(errno));
        game_session_destroy(session);
        return EXIT_FAILURE;
    }

    result = game_session_save(session);
    game_session_destroy(session);
    if (result != GAME_SESSION_OK) {
#ifdef _WIN32
        (void)_rmdir(directory);
#else
        (void)rmdir(directory);
#endif
        return world_error("save new", name, result);
    }
    printf("Created world '%s' at %s\n", name, path);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 &&
        (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")))) {
        print_usage(stdout);
        return EXIT_SUCCESS;
    }

    if (argc == 3 && !strcmp(argv[1], "run-c"))
        return dusty_driver(argv[2], NULL, true);
    if (!strcmp(argv[1], "export-c")) {
        if (argc == 3) return dusty_export_driver(argv[2]);
        if (argc == 4 || argc == 5)
            return dusty_export_c(argv[2], argv[3], argc == 5 ? argv[4] : NULL);
        print_usage(stderr);
        return 2;
    }

    /* Accept the previous world prefix for existing shell commands. */
    if (argc > 2 && !strcmp(argv[1], "world")) {
        --argc;
        ++argv;
    }
    bool list = argc == 2 && !strcmp(argv[1], "list");
    bool create = argc == 3 && !strcmp(argv[1], "create");
    bool load = argc == 3 && (!strcmp(argv[1], "load") || !strcmp(argv[1], "view"));
    bool deleting = argc == 3 && !strcmp(argv[1], "delete");
    if (!list && !create && !load && !deleting) {
        print_usage(stderr);
        return EXIT_FAILURE;
    }

#if defined(__APPLE__)
    if (!configure_mac_bundle()) {
        fputs("Cannot open the application data directory\n", stderr);
        return EXIT_FAILURE;
    }
#endif
    if (list) return list_worlds();

    const char *name = argv[2];
    if (deleting) return dusty_delete_world(save_directory, name);
    char path[GAME_SESSION_PATH_CAPACITY];
    GameSessionResult result = game_session_make_path(save_directory, name, path);
    if (result != GAME_SESSION_OK) return world_error(create ? "create" : "load", name, result);
    if (create) return create_world(name, path);

    GameSession *session = game_session_load(path, &result);
    if (!session) return world_error("load", name, result);
    int exit_code = gui_run(session, name);
    game_session_destroy(session);
    return exit_code;
}
