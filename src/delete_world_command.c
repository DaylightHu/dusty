#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "dusty_commands.h"
#include "rs/session.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <stdlib.h>

static char *join_path(const char *parent, const char *name)
{
    size_t length = strlen(parent) + strlen(name) + 2;
    char *path = malloc(length);
    if (path) snprintf(path, length, "%s/%s", parent, name);
    else SetLastError(ERROR_OUTOFMEMORY);
    return path;
}

static bool remove_contents(const char *path, const char *keep)
{
    char *pattern = join_path(path, "*");
    if (!pattern) return false;
    WIN32_FIND_DATAA entry;
    HANDLE search = FindFirstFileA(pattern, &entry);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;

    DWORD error;
    for (;;) {
        const char *name = entry.cFileName;
        if (strcmp(name, ".") && strcmp(name, "..") && (!keep || _stricmp(name, keep))) {
            char *child = join_path(path, name);
            if (!child) { error = GetLastError(); break; }
            DWORD attributes = GetFileAttributesA(child);
            bool ok = false;
            if (attributes != INVALID_FILE_ATTRIBUTES) {
                if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
                    ok = ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                          remove_contents(child, NULL)) && RemoveDirectoryA(child);
                } else {
                    ok = DeleteFileA(child);
                }
            }
            error = ok ? ERROR_SUCCESS : GetLastError();
            free(child);
            if (!ok) break;
        }
        if (!FindNextFileA(search, &entry)) {
            error = GetLastError();
            if (error == ERROR_NO_MORE_FILES) error = ERROR_SUCCESS;
            break;
        }
    }
    FindClose(search);
    SetLastError(error);
    return error == ERROR_SUCCESS;
}

static bool real_directory(const char *path)
{
    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        SetLastError(ERROR_DIRECTORY);
        return false;
    }
    return true;
}

static bool remove_world(const char *saves, const char *name, const char *file)
{
    if (!real_directory(saves)) return false;
    char *path = join_path(saves, name);
    if (!path) return false;
    char *save = join_path(path, file);
    bool ok = false;
    if (save && real_directory(path)) {
        DWORD attributes = GetFileAttributesA(save);
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
                SetLastError(ERROR_INVALID_DATA);
            else
                ok = remove_contents(path, file) && DeleteFileA(save) &&
                     RemoveDirectoryA(path);
        }
    }
    DWORD error = GetLastError();
    free(save);
    free(path);
    SetLastError(error);
    return ok;
}
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static bool remove_contents(int directory, const char *keep);

static bool remove_entry(int parent, const char *name)
{
    struct stat info;
    if (fstatat(parent, name, &info, AT_SYMLINK_NOFOLLOW) < 0) return false;
    if (!S_ISDIR(info.st_mode)) return unlinkat(parent, name, 0) == 0;

    int child = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (child < 0) return false;
    bool ok = remove_contents(child, NULL);
    int error = errno;
    close(child);
    if (!ok) {
        errno = error;
        return false;
    }
    return unlinkat(parent, name, AT_REMOVEDIR) == 0;
}

static bool remove_contents(int directory, const char *keep)
{
    int descriptor = dup(directory);
    if (descriptor < 0) return false;
    DIR *stream = fdopendir(descriptor);
    if (!stream) {
        int error = errno;
        close(descriptor);
        errno = error;
        return false;
    }

    /* Delete the save last so a partially deleted world can be retried. */
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(stream);
        if (!entry) break;
        const char *name = entry->d_name;
        if (strcmp(name, ".") && strcmp(name, "..") && (!keep || strcmp(name, keep)) &&
            !remove_entry(directory, name)) break;
    }
    int error = errno;
    if (closedir(stream) != 0 && !error) error = errno;
    errno = error;
    return error == 0;
}

static bool remove_world(const char *saves, const char *name, const char *file)
{
    int parent = open(saves, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent < 0) return false;
    int world = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    bool ok = false;
    if (world >= 0) {
        struct stat info;
        if (fstatat(world, file, &info, AT_SYMLINK_NOFOLLOW) == 0) {
            if (!S_ISREG(info.st_mode)) errno = EINVAL;
            else if (remove_contents(world, file) && unlinkat(world, file, 0) == 0)
                ok = unlinkat(parent, name, AT_REMOVEDIR) == 0;
        }
        int error = errno;
        close(world);
        errno = error;
    }
    int error = errno;
    close(parent);
    errno = error;
    return ok;
}
#endif

int dusty_delete_world(const char *save_directory, const char *name)
{
    if (!game_session_valid_name(name)) {
        fprintf(stderr, "Cannot delete world '%s': invalid world name\n", name);
        return 1;
    }
    char file[GAME_SESSION_NAME_CAPACITY + sizeof ".mcw"];
    snprintf(file, sizeof(file), "%s.mcw", name);
    if (!remove_world(save_directory, name, file)) {
#ifdef _WIN32
        fprintf(stderr, "Cannot delete world '%s' (Windows error %lu)\n",
                name, (unsigned long)GetLastError());
#else
        fprintf(stderr, "Cannot delete world '%s': %s\n", name, strerror(errno));
#endif
        return 1;
    }
    printf("Deleted world '%s'\n", name);
    return 0;
}
