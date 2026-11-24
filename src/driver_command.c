/* nob bootstraps from one source file and already provides these definitions. */
#ifndef NOB_H_
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#endif
#define NOB_IMPLEMENTATION
#define NOB_UNSTRIP_PREFIX
#include "../nob.h"
#endif

#include "dusty_commands.h"

#include <ctype.h>
#ifdef _WIN32
#include <process.h>
static const char driver_suffix[] = ".exe";
#define driver_process_id _getpid
#else
static const char driver_suffix[] = "";
#define driver_process_id getpid
#endif

static int run_driver_binary(const char *path)
{
#ifdef _WIN32
    const char *const arguments[] = {path, NULL};
    intptr_t status = _spawnv(_P_WAIT, path, arguments);
    if (status != -1) return (int)status;
#else
    char *const arguments[] = {(char *)path, NULL};
    execv(path, arguments);
#endif
    nob_log(NOB_ERROR, "Cannot run driver '%s': %s", path, strerror(errno));
    return 1;
}

static char *user_source_path(const char *source)
{
#ifdef _WIN32
    char *absolute=_fullpath(NULL,source,0);
#else
    char *absolute=realpath(source,NULL);
#endif
    if (!absolute) {
        nob_log(NOB_ERROR,"cannot resolve source %s: %s",source,strerror(errno));
        return NULL;
    }
#ifdef _WIN32
    for (char *p=absolute; *p; ++p) if (*p=='\\') *p='/';
#endif
    size_t length=strlen(absolute);
    if (length<2 || absolute[length-2]!='.' ||
        tolower((unsigned char)absolute[length-1])!='c' ||
        nob_get_file_type(absolute)!=NOB_FILE_REGULAR) {
        nob_log(NOB_ERROR,"expected a C source file: %s",source);
        free(absolute);
        return NULL;
    }
    return absolute;
}

static bool make_parent_directories(const char *path)
{
    char *directory = malloc(strlen(path) + 1);
    if (!directory) return false;
    strcpy(directory, path);
    bool ok=true;
    for (char *p=directory; *p; ++p) if (*p=='/') {
        *p='\0';
        if (!nob_mkdir_if_not_exists(directory)) { ok=false; break; }
        *p='/';
    }
    free(directory);
    return ok;
}

int dusty_driver(const char *source, const char *cc, bool run)
{
    if (!cc || !*cc) {
        cc = getenv("CC");
#ifdef _WIN32
        if (!cc || !*cc) cc = "clang";
#else
        if (!cc || !*cc) cc = "cc";
#endif
    }
    char *absolute=user_source_path(source);
    if (!absolute) return 1;
    int status = 1;
    const char *root=nob_get_current_dir_temp();
    if (!root) goto done;
    size_t root_length=strlen(root);
    if (strlen(absolute)<root_length) goto outside_project;
    for (size_t i=0; i<root_length; ++i) {
        unsigned char a=(unsigned char)root[i],b=(unsigned char)absolute[i];
#ifdef _WIN32
        if (a=='\\') a='/';
        a=(unsigned char)tolower(a); b=(unsigned char)tolower(b);
#endif
        if (a!=b) goto outside_project;
    }
    const char *relative=absolute+root_length;
    bool root_separator=root_length && root[root_length-1]=='/';
#ifdef _WIN32
    root_separator=root_separator || (root_length && root[root_length-1]=='\\');
#endif
    if (!root_separator) {
        if (*relative!='/') goto outside_project;
        ++relative;
    }
    if (!*relative) goto outside_project;
    const char *stem=nob_temp_sprintf("build/programs/%.*s",(int)strlen(relative)-2,relative);
    const char *output=nob_temp_sprintf("%s%s",stem,driver_suffix);
    const char *temporary = nob_temp_sprintf("build/program-tmp/%.*s-%lu%s",
        (int)strlen(relative) - 2, relative, (unsigned long)driver_process_id(), driver_suffix);
    if (!nob_file_exists("build/libmcmux.a")) {
        nob_log(NOB_ERROR, "Cannot find build/libmcmux.a; build Voxilog from the project root first");
        goto done;
    }
    if (!make_parent_directories(output) || !make_parent_directories(temporary)) goto done;
    Nob_Cmd command = {0};
    nob_cmd_append(&command, cc,"-std=c11","-O2","-Wall","-Wextra",
        "-Iinclude","-x","c",absolute,"-x","none","build/libmcmux.a","-o",temporary);
#ifndef _WIN32
    nob_cmd_append(&command,"-lm");
#endif
    bool compiled = nob_cmd_run(&command);
    free(command.items);
    if (!compiled) {
        if (nob_file_exists(temporary)) (void)nob_delete_file(temporary);
        goto done;
    }
    if (!nob_rename(temporary,output)) {
        if (nob_file_exists(temporary)) (void)nob_delete_file(temporary);
        goto done;
    }
    nob_log(NOB_INFO,"built %s",output);
    status = run ? run_driver_binary(output) : 0;
    goto done;
outside_project:
    nob_log(NOB_ERROR,"source must be inside the project directory: %s",source);
done:
    free(absolute);
    return status;
}
