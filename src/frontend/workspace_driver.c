#if defined(__linux__) || defined(__APPLE__)
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#define _XOPEN_SOURCE 700
#endif
#include "workspace_driver.h"
#include "workspace_terminal.h"

#if defined(__linux__) || defined(__APPLE__)
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

bool workspace_driver_validate(const char *source, char *error, size_t capacity)
{
    struct stat info;
    size_t length = strlen(source);
    if (source[0] != '/' || length < 3 || strcmp(source + length - 2, ".c") ||
        stat(source, &info) != 0 || !S_ISREG(info.st_mode)) {
        snprintf(error, capacity, "Expected an absolute path to a saved C file");
        return false;
    }
    if (access("./nob", X_OK) != 0 || access("nob.c", R_OK) != 0) {
        snprintf(error, capacity, "Start mcmux from the project root with a compiled nob");
        return false;
    }
    char *file = realpath(source, NULL);
    char *root = getcwd(NULL, 0);
    size_t root_length = root ? strlen(root) : 0;
    bool valid = file && root && strncmp(file, root, root_length) == 0 &&
        (root_length == 1 || file[root_length] == '/');
    free(file);
    free(root);
    if (!valid) snprintf(error, capacity, "C file must be inside the mcmux project");
    return valid;
}

void workspace_driver_reject(int reply, int output, const char *error)
{
    close(output);
    workspace_terminal_reply(reply, error);
}

bool workspace_driver_start(WorkspaceDriver *driver, const char *source, int reply, int output)
{
    pid_t pid = fork();
    if (pid == 0) {
        int input = open("/dev/null", O_RDONLY);
        if (input < 0 || dup2(input, STDIN_FILENO) < 0 || setpgid(0, 0) != 0 ||
            dup2(output, STDOUT_FILENO) < 0 || dup2(output, STDERR_FILENO) < 0)
            _exit(126);
        if (input != STDIN_FILENO) close(input);
        close(output);
        close(reply);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        (void)unsetenv("MCMUX_RUN_FD");
        const char *const arguments[] = {"./nob", "run-c", source, NULL};
        execv(arguments[0], (char *const *)arguments);
        static const char message[] = "mcmux: could not execute ./nob\n";
        (void)write(STDERR_FILENO, message, sizeof message - 1);
        _exit(127);
    }
    close(output);
    if (pid < 0) {
        workspace_terminal_reply(reply, "Could not start the simulation driver");
        return false;
    }
    (void)setpgid(pid, pid);
    *driver = (WorkspaceDriver){.process = pid, .reply = reply};
    return true;
}

static void finish(WorkspaceDriver *driver, const char *error)
{
    workspace_terminal_reply(driver->reply, error);
    *driver = (WorkspaceDriver){0};
}

void workspace_driver_poll(WorkspaceDriver *driver, double now)
{
    if (!driver->process) return;
    if (driver->cancelling && now >= driver->deadline)
        (void)kill(-driver->process, SIGKILL);
    int status;
    pid_t result = waitpid(driver->process, &status, WNOHANG);
    if (result == 0 || (result < 0 && errno == EINTR)) return;
    if (driver->cancelling) {
        (void)kill(-driver->process, SIGKILL);
        finish(driver, "Driver cancelled");
    } else if (result < 0) {
        finish(driver, "Could not obtain the simulation driver's exit status");
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        finish(driver, NULL);
    } else {
        char message[96];
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        snprintf(message, sizeof message, "Simulation driver exited with status %d", code);
        finish(driver, message);
    }
}

void workspace_driver_cancel(WorkspaceDriver *driver, double now)
{
    if (!driver->process || driver->cancelling) return;
    (void)kill(-driver->process, SIGTERM);
    driver->cancelling = true;
    driver->deadline = now + 0.25;
}

void workspace_driver_close(WorkspaceDriver *driver)
{
    if (!driver->process) return;
    (void)kill(-driver->process, SIGKILL);
    while (waitpid(driver->process, NULL, 0) < 0 && errno == EINTR) {}
    finish(driver, "mcmux closed");
}
#else
#include <stdio.h>
bool workspace_driver_validate(const char *source, char *error, size_t capacity)
{ (void)source; snprintf(error, capacity, "Terminal driver unavailable"); return false; }
void workspace_driver_reject(int reply, int output, const char *error)
{ (void)reply; (void)output; (void)error; }
bool workspace_driver_start(WorkspaceDriver *driver, const char *source, int reply, int output)
{ (void)driver; (void)source; (void)reply; (void)output; return false; }
void workspace_driver_poll(WorkspaceDriver *driver, double now) { (void)driver; (void)now; }
void workspace_driver_cancel(WorkspaceDriver *driver, double now) { (void)driver; (void)now; }
void workspace_driver_close(WorkspaceDriver *driver) { (void)driver; }
#endif
