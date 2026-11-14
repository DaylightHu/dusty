#if defined(__linux__) || defined(__APPLE__)
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "workspace_terminal.h"

#if defined(__linux__) || defined(__APPLE__)
#include "app_config.h"
#include "ui_font.h"
#include "rlgl.h"
#include "../../third_party/libvterm/include/vterm.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

enum {
    MAX_ROWS = 256, MAX_COLS = 512, HISTORY_ROWS = 1024,
    READ_BUDGET = 16 * 1024, WRITE_CAPACITY = 128 * 1024, REQUEST_CAPACITY = 4096
};

typedef struct {
    VTermScreenCell *cells;
    int cols;
} HistoryLine;

struct WorkspaceTerminal {
    VTerm *vterm;
    VTermScreen *screen;
    int master, request;
    pid_t shell;
    bool cursor_visible, cursor_blink, alt_screen, focused;
    unsigned int buttons;
    int rows, cols, font_size, cell_width, cell_height, cursor_shape, mouse_mode;
    float scale;
    unsigned char background_alpha;
    unsigned int config_revision;
    VTermPos cursor;
    RenderTexture2D pixels;
    bool dirty[MAX_ROWS];
    HistoryLine history[HISTORY_ROWS];
    size_t history_start, history_count, scroll;
    char pending[WRITE_CAPACITY];
    size_t written, queued;
};

static const Color background = {30, 30, 46, 255};
static const Color foreground = {205, 214, 244, 255};

static void invalidate(WorkspaceTerminal *terminal)
{
    memset(terminal->dirty, 1, sizeof terminal->dirty);
}

static int damage(VTermRect rect, void *data)
{
    WorkspaceTerminal *terminal = data;
    for (int i = rect.start_row; i < rect.end_row && i < terminal->rows; ++i)
        if (i >= 0) terminal->dirty[i] = true;
    return 1;
}

static int move_cursor(VTermPos pos, VTermPos old, int visible, void *data)
{
    (void)old;
    WorkspaceTerminal *terminal = data;
    terminal->cursor = pos;
    terminal->cursor_visible = visible;
    return 1;
}

static int set_property(VTermProp prop, VTermValue *value, void *data)
{
    WorkspaceTerminal *terminal = data;
    switch (prop) {
    case VTERM_PROP_CURSORVISIBLE: terminal->cursor_visible = value->boolean; break;
    case VTERM_PROP_CURSORBLINK: terminal->cursor_blink = value->boolean; break;
    case VTERM_PROP_CURSORSHAPE: terminal->cursor_shape = value->number; break;
    case VTERM_PROP_MOUSE: terminal->mouse_mode = value->number; break;
    case VTERM_PROP_ALTSCREEN:
        terminal->alt_screen = value->boolean;
        terminal->scroll = 0;
        invalidate(terminal);
        break;
    case VTERM_PROP_REVERSE: invalidate(terminal); break;
    default: break;
    }
    return 1;
}

static int clear_history(void *data)
{
    WorkspaceTerminal *terminal = data;
    for (size_t i = 0; i < terminal->history_count; ++i) {
        size_t index = (terminal->history_start + i) % HISTORY_ROWS;
        free(terminal->history[index].cells);
        terminal->history[index] = (HistoryLine){0};
    }
    terminal->history_start = terminal->history_count = terminal->scroll = 0;
    invalidate(terminal);
    return 1;
}

static int push_history(int cols, const VTermScreenCell *cells, void *data)
{
    WorkspaceTerminal *terminal = data;
    if (terminal->alt_screen || cols <= 0 || cols > MAX_COLS) return 0;
    size_t index = (terminal->history_start + terminal->history_count) % HISTORY_ROWS;
    HistoryLine *line = &terminal->history[index];
    if (line->cols != cols) {
        VTermScreenCell *copy = realloc(line->cells, (size_t)cols * sizeof *copy);
        if (!copy) return 0;
        line->cells = copy;
        line->cols = cols;
    }
    memcpy(line->cells, cells, (size_t)cols * sizeof *cells);
    if (terminal->history_count == HISTORY_ROWS)
        terminal->history_start = (terminal->history_start + 1) % HISTORY_ROWS;
    else ++terminal->history_count;
    if (terminal->scroll) {
        if (terminal->scroll < terminal->history_count) ++terminal->scroll;
        invalidate(terminal);
    }
    return 1;
}

static void close_master(WorkspaceTerminal *terminal)
{
    if (terminal->master >= 0) close(terminal->master);
    terminal->master = -1;
    terminal->written = terminal->queued = 0;
}

static void queue_output(const char *bytes, size_t length, void *data)
{
    WorkspaceTerminal *terminal = data;
    if (terminal->master < 0 || length > sizeof terminal->pending - (terminal->queued - terminal->written))
        return;
    if (length > sizeof terminal->pending - terminal->queued) {
        memmove(terminal->pending, terminal->pending + terminal->written,
            terminal->queued - terminal->written);
        terminal->queued -= terminal->written;
        terminal->written = 0;
    }
    memcpy(terminal->pending + terminal->queued, bytes, length);
    terminal->queued += length;
}

static void flush_input(WorkspaceTerminal *terminal)
{
    size_t budget = READ_BUDGET;
    while (budget && terminal->master >= 0 && terminal->written < terminal->queued) {
        size_t length = terminal->queued - terminal->written;
        if (length > budget) length = budget;
        ssize_t count = write(terminal->master, terminal->pending + terminal->written, length);
        if (count > 0) {
            terminal->written += (size_t)count;
            budget -= (size_t)count;
        }
        else if (count < 0 && errno == EINTR) continue;
        else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        else { close_master(terminal); break; }
    }
    if (terminal->written == terminal->queued) terminal->written = terminal->queued = 0;
}

static bool nonblocking(int fd)
{
    return fcntl(fd, F_SETFL, O_NONBLOCK) == 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

static bool send_all(int fd, const char *data, size_t length)
{
    while (length) {
#if defined(MSG_NOSIGNAL)
        ssize_t count = send(fd, data, length, MSG_NOSIGNAL);
#else
        ssize_t count = send(fd, data, length, 0);
#endif
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        data += count;
        length -= (size_t)count;
    }
    return true;
}

static void suppress_sigpipe(int fd)
{
#if defined(SO_NOSIGPIPE)
    int on = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#else
    (void)fd;
#endif
}

int workspace_terminal_request_run(const char *source)
{
    const char *value = getenv("MCMUX_RUN_FD");
    char *end = NULL;
    long fd = value ? strtol(value, &end, 10) : -1;
    size_t length = strlen(source);
    if (fd < 3 || fd > INT32_MAX || *end || !length || length >= REQUEST_CAPACITY) {
        fputs("Run this command from a mcmux terminal pane with a C source path.\n", stderr);
        return 1;
    }
    int endpoints[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, endpoints) < 0) {
        perror("mcmux run reply");
        return 1;
    }
    struct iovec argv = {(char *)source, length + 1};
    union { struct cmsghdr alignment; char data[CMSG_SPACE(2 * sizeof(int))]; } control = {0};
    struct msghdr message = {
        .msg_iov = &argv, .msg_iovlen = 1,
        .msg_control = control.data, .msg_controllen = sizeof control.data,
    };
    struct cmsghdr *entry = CMSG_FIRSTHDR(&message);
    entry->cmsg_level = SOL_SOCKET;
    entry->cmsg_type = SCM_RIGHTS;
    entry->cmsg_len = CMSG_LEN(2 * sizeof(int));
    int descriptors[] = {endpoints[1], STDOUT_FILENO};
    memcpy(CMSG_DATA(entry), descriptors, sizeof descriptors);
    ssize_t count;
    do {
#if defined(MSG_NOSIGNAL)
        count = sendmsg((int)fd, &message, MSG_NOSIGNAL);
#else
        count = sendmsg((int)fd, &message, 0);
#endif
    } while (count < 0 && errno == EINTR);
    close(endpoints[1]);
    if (count != (ssize_t)length + 1) {
        if (count >= 0) errno = EIO;
        perror("mcmux run request");
        close(endpoints[0]);
        return 1;
    }
    char reply[512];
    size_t used = 0;
    while (used < sizeof reply - 1) {
        count = read(endpoints[0], reply + used, sizeof reply - 1 - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        used += (size_t)count;
        if (reply[used - 1] == '\0') {
            close(endpoints[0]);
            if (!reply[0]) return 0;
            fprintf(stderr, "mcmux: %s\n", reply);
            return 1;
        }
    }
    close(endpoints[0]);
    fputs("mcmux closed the run request.\n", stderr);
    return 1;
}

void workspace_terminal_reply(int fd, const char *error)
{
    const char *reply = error ? error : "";
    (void)send_all(fd, reply, strlen(reply) + 1);
    close(fd);
}

static void poll_request(WorkspaceTerminal *terminal, WorkspaceRunRequest run, void *context)
{
    if (!run || terminal->request < 0) return;
    /* Each datagram carries an independent reply channel and an output stream.
     * Neovim can connect its buffer without intercepting the pane's PTY. */
    char source[REQUEST_CAPACITY];
    struct iovec argv = {source, sizeof source};
    union { struct cmsghdr alignment; char data[CMSG_SPACE(2 * sizeof(int))]; } control;
    struct msghdr message = {
        .msg_iov = &argv, .msg_iovlen = 1,
        .msg_control = control.data, .msg_controllen = sizeof control.data,
    };
    ssize_t count = recvmsg(terminal->request, &message, 0);
    if (count < 0) return;
    int descriptors[2], received = 0;
    for (struct cmsghdr *entry = CMSG_FIRSTHDR(&message); entry;
         entry = CMSG_NXTHDR(&message, entry)) {
        if (entry->cmsg_level != SOL_SOCKET || entry->cmsg_type != SCM_RIGHTS) continue;
        if (entry->cmsg_len < CMSG_LEN(0)) continue;
        size_t size = entry->cmsg_len - CMSG_LEN(0);
        for (size_t i = 0; i < size / sizeof(int); ++i) {
            int fd;
            memcpy(&fd, CMSG_DATA(entry) + i * sizeof(int), sizeof fd);
            if (received < 2) descriptors[received++] = fd;
            else close(fd);
        }
    }
    if (received == 2 && !(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) &&
        count > 1 && memchr(source, '\0', (size_t)count) == source + count - 1 &&
        nonblocking(descriptors[0]) && fcntl(descriptors[1], F_SETFD, FD_CLOEXEC) == 0) {
        suppress_sigpipe(descriptors[0]);
        run(context, source, descriptors[0], descriptors[1]);
        return;
    }
    for (int i = 0; i < received; ++i) close(descriptors[i]);
}

static void terminal_size(WorkspaceTerminal *terminal, Rectangle box);

static void child_environment(int fd, const char *config)
{
    char number[24];
    snprintf(number, sizeof number, "%d", fd);
    (void)setenv("MCMUX_RUN_FD", number, 1);
    char *cwd = getcwd(NULL, 0);
    if (cwd) {
        (void)setenv("MCMUX_PROJECT", cwd, 1);
        const char *path = getenv("PATH");
        if (!path || !*path) path = "/usr/bin:/bin";
        const char suffix[] = "/integrations/bin";
        size_t capacity = strlen(cwd) + sizeof suffix + strlen(path) + 1;
        char *task_path = malloc(capacity);
        if (task_path) {
            snprintf(task_path, capacity, "%s%s:%s", cwd, suffix, path);
            (void)setenv("PATH", task_path, 1);
            free(task_path);
        }
        const char *data_dirs = getenv("XDG_DATA_DIRS");
        if (!data_dirs || !*data_dirs) data_dirs = "/usr/local/share:/usr/share";
        const char integrations[] = "/integrations";
        size_t data_capacity = strlen(cwd) + sizeof integrations + strlen(data_dirs) + 1;
        char *terminal_data_dirs = malloc(data_capacity);
        if (terminal_data_dirs) {
            snprintf(terminal_data_dirs, data_capacity, "%s%s:%s", cwd, integrations, data_dirs);
            (void)setenv("XDG_DATA_DIRS", terminal_data_dirs, 1);
            free(terminal_data_dirs);
        }
        free(cwd);
    }
    char executable[4096];
#if defined(__APPLE__)
    uint32_t capacity = sizeof executable;
    if (_NSGetExecutablePath(executable, &capacity) == 0)
        (void)setenv("MCMUX_HOST", executable, 1);
#else
    ssize_t length = readlink("/proc/self/exe", executable, sizeof executable - 1);
    if (length > 0 && length < (ssize_t)sizeof executable - 1) {
        executable[length] = '\0';
        (void)setenv("MCMUX_HOST", executable, 1);
    }
#endif
    if (config) (void)setenv("MCMUX_CONFIG", config, 1);
    (void)setenv("TERM", "xterm-256color", 1);
    (void)setenv("COLORTERM", "truecolor", 1);
    static const char *const parent_terminal[] = {
        "TERM_PROGRAM", "TERM_PROGRAM_VERSION", "TMUX", "KITTY_WINDOW_ID", "KITTY_LISTEN_ON",
    };
    for (size_t i = 0; i < sizeof parent_terminal / sizeof *parent_terminal; ++i)
        (void)unsetenv(parent_terminal[i]);
}

WorkspaceTerminal *workspace_terminal_open(const char *edit_path, Rectangle box, char *error, size_t capacity)
{
    WorkspaceTerminal *terminal = calloc(1, sizeof *terminal);
    if (!terminal) {
        snprintf(error, capacity, "Cannot allocate terminal pane");
        return NULL;
    }
    terminal->master = terminal->request = -1;
    terminal->rows = 24;
    terminal->cols = 80;
    terminal->vterm = vterm_new(terminal->rows, terminal->cols);
    if (!terminal->vterm) {
        errno = ENOMEM;
        goto fail;
    }
    terminal->screen = vterm_obtain_screen(terminal->vterm);
    vterm_set_utf8(terminal->vterm, 1);
    vterm_output_set_callback(terminal->vterm, queue_output, terminal);
    static const VTermScreenCallbacks callbacks = {
        .damage = damage, .movecursor = move_cursor, .settermprop = set_property,
        .sb_pushline = push_history, .sb_clear = clear_history,
    };
    vterm_screen_set_callbacks(terminal->screen, &callbacks, terminal);
    vterm_screen_set_damage_merge(terminal->screen, VTERM_DAMAGE_ROW);
    vterm_screen_enable_altscreen(terminal->screen, 1);
    vterm_screen_enable_reflow(terminal->screen, true);
    VTermColor fg, bg;
    vterm_color_rgb(&fg, foreground.r, foreground.g, foreground.b);
    vterm_color_rgb(&bg, background.r, background.g, background.b);
    vterm_screen_set_default_colors(terminal->screen, &fg, &bg);
    vterm_screen_reset(terminal->screen, 1);
    terminal->cursor_visible = true;
    invalidate(terminal);

    terminal_size(terminal, box);

    int endpoints[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, endpoints) < 0) goto fail;
    suppress_sigpipe(endpoints[1]);
    if (!nonblocking(endpoints[0])) {
        int saved = errno;
        close(endpoints[0]); close(endpoints[1]);
        errno = saved;
        goto fail;
    }
    terminal->request = endpoints[0];
    struct winsize size = {.ws_row = (unsigned short)terminal->rows,
                           .ws_col = (unsigned short)terminal->cols};
    terminal->shell = forkpty(&terminal->master, NULL, NULL, &size);
    if (terminal->shell == 0) {
        close(endpoints[0]);
        child_environment(endpoints[1], edit_path);
        if (edit_path) {
            execlp("nvim", "nvim", "--", edit_path, (char *)NULL);
            static const char note[] = "mcmux: could not open config in nvim; edit MCMUX_CONFIG from this shell.\r\n";
            (void)write(STDERR_FILENO, note, sizeof note - 1);
        }
        const char *shell = getenv("SHELL");
        if (!shell || !*shell) shell = "/bin/sh";
        execl(shell, shell, (char *)NULL);
        static const char message[] = "mcmux: could not start the configured shell\r\n";
        (void)write(STDERR_FILENO, message, sizeof message - 1);
        _exit(127);
    }
    close(endpoints[1]);
    if (terminal->shell < 0 || !nonblocking(terminal->master)) goto fail;
    return terminal;

fail:
    snprintf(error, capacity, "Cannot start terminal: %s", strerror(errno));
    workspace_terminal_close(terminal);
    return NULL;
}

void workspace_terminal_close(WorkspaceTerminal *terminal)
{
    if (!terminal) return;
    close_master(terminal);
    if (terminal->request >= 0) close(terminal->request);
    if (terminal->shell > 0) {
        (void)kill(terminal->shell, SIGKILL);
        while (waitpid(terminal->shell, NULL, 0) < 0 && errno == EINTR) {}
    }
    if (terminal->pixels.id) UnloadRenderTexture(terminal->pixels);
    clear_history(terminal);
    if (terminal->vterm) vterm_free(terminal->vterm);
    free(terminal);
}

bool workspace_terminal_exited(const WorkspaceTerminal *terminal)
{
    return terminal && terminal->shell == 0 && terminal->master < 0;
}

void workspace_terminal_poll(WorkspaceTerminal *terminal, WorkspaceRunRequest run, void *context)
{
    if (terminal->shell > 0) {
        pid_t result = waitpid(terminal->shell, NULL, WNOHANG);
        if (result == terminal->shell || (result < 0 && errno == ECHILD)) terminal->shell = 0;
    }
    if (terminal->master >= 0) {
        char bytes[4096];
        size_t budget = READ_BUDGET;
        while (budget) {
            size_t chunk = budget < sizeof bytes ? budget : sizeof bytes;
            ssize_t count = read(terminal->master, bytes, chunk);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (count <= 0) { close_master(terminal); break; }
            vterm_input_write(terminal->vterm, bytes, (size_t)count);
            budget -= (size_t)count;
        }
        vterm_screen_flush_damage(terminal->screen);
        flush_input(terminal);
    }
    poll_request(terminal, run, context);
}

static int clamp(int value, int low, int high)
{
    if (value < low) return low;
    return value > high ? high : value;
}

static Color terminal_color(WorkspaceTerminal *terminal, VTermColor color)
{
    vterm_screen_convert_color_to_rgb(terminal->screen, &color);
    return (Color){color.rgb.red, color.rgb.green, color.rgb.blue, 255};
}

static VTermScreenCell cell_at(WorkspaceTerminal *terminal, int row, int col)
{
    VTermScreenCell cell = {.width = 1};
    vterm_color_rgb(&cell.fg, foreground.r, foreground.g, foreground.b);
    vterm_color_rgb(&cell.bg, background.r, background.g, background.b);
    cell.bg.type |= VTERM_COLOR_DEFAULT_BG;
    int line = row - (int)terminal->scroll;
    if (line >= 0) {
        vterm_screen_get_cell(terminal->screen, (VTermPos){line, col}, &cell);
    } else {
        size_t index = (terminal->history_start + terminal->history_count + HISTORY_ROWS
            - (size_t)-line) % HISTORY_ROWS;
        HistoryLine *history = &terminal->history[index];
        if (col < history->cols) cell = history->cells[col];
    }
    return cell;
}

static Color cell_background(WorkspaceTerminal *terminal, const VTermScreenCell *cell)
{
    VTermColor color = cell->attrs.reverse ? cell->fg : cell->bg;
    Color result = terminal_color(terminal, color);
    if (VTERM_COLOR_IS_DEFAULT_BG(&color)) result.a = terminal->background_alpha;
    return result;
}

static Color premultiply(Color color)
{
    color.r = (color.r * color.a + 127) / 255;
    color.g = (color.g * color.a + 127) / 255;
    color.b = (color.b * color.a + 127) / 255;
    return color;
}

static void draw_row(WorkspaceTerminal *terminal, int row)
{
    int width = terminal->cell_width, y = row * terminal->cell_height;
    VTermScreenCell cells[MAX_COLS];
    for (int col = 0; col < terminal->cols; ++col)
        cells[col] = cell_at(terminal, row, col);
    /* Paint backgrounds before glyphs so their overhang is preserved. */
    if (terminal->background_alpha < 255) BeginBlendMode(BLEND_CUSTOM);
    for (int col = 0; col < terminal->cols;) {
        VTermScreenCell *cell = &cells[col];
        Color bg = cell_background(terminal, cell);
        int end = col + cell->width;
        while (end < terminal->cols) {
            VTermScreenCell *next = &cells[end];
            Color other = cell_background(terminal, next);
            if (ColorToInt(bg) != ColorToInt(other)) break;
            end += next->width;
        }
        if (bg.a < 255) bg = premultiply(bg);
        DrawRectangle(col * width, y, (end - col) * width, terminal->cell_height, bg);
        col = end;
    }
    // Store premultiplied RGBA including antialiased glyph edges.
    if (terminal->background_alpha < 255) BeginBlendMode(BLEND_CUSTOM_SEPARATE);
    for (int col = 0; col < terminal->cols; ++col) {
        const VTermScreenCell *cell = &cells[col];
        if (cell->chars[0] == UINT32_MAX || cell->attrs.conceal) continue;
        Color fg = terminal_color(terminal, cell->attrs.reverse ? cell->bg : cell->fg);
        int x = col * width;
        for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell->chars[i]; ++i)
            if (cell->chars[i] != ' ')
                ui_terminal_glyph((int)cell->chars[i], x, y, terminal->font_size,
                    cell->attrs.bold, cell->attrs.italic, fg);
        int underline = y + terminal->cell_height - 3;
        if (cell->attrs.underline == VTERM_UNDERLINE_CURLY) {
            for (int i = 0; i < width * cell->width; ++i)
                DrawPixel(x + i, underline + ((i / 2) % 2), fg);
        } else if (cell->attrs.underline) {
            DrawRectangle(x, underline, width * cell->width, 1, fg);
            if (cell->attrs.underline == VTERM_UNDERLINE_DOUBLE)
                DrawRectangle(x, underline + 2, width * cell->width, 1, fg);
        }
        if (cell->attrs.strike) DrawRectangle(x, y + terminal->cell_height / 2, width * cell->width, 1, fg);
    }
    terminal->dirty[row] = false;
}

static void terminal_size(WorkspaceTerminal *terminal, Rectangle box)
{
    const AppSettings *settings = app_config_current();
    int size = settings->terminal_font_size;
    float scale = (float)GetRenderHeight() / GetScreenHeight();
    if (scale < 1) scale = 1;
    int height = (int)((size + 4) * scale + .5f);
    int font_size = (int)(size * scale + .5f);
    int width = ui_terminal_cell_width(font_size, settings->terminal_font);
    int rows = clamp((int)((box.height - 12) * scale) / height, 1, MAX_ROWS);
    int cols = clamp((int)((box.width - 12) * scale) / width, 1, MAX_COLS);
    if (rows == terminal->rows && cols == terminal->cols &&
        scale == terminal->scale && font_size == terminal->font_size &&
        terminal->config_revision == app_config_revision()) return;

    RenderTexture2D pixels = LoadRenderTexture(cols * width, rows * height);
    if (!pixels.id || !pixels.texture.id) {
        if (pixels.id) UnloadRenderTexture(pixels);
        return;
    }
    if (terminal->pixels.id) UnloadRenderTexture(terminal->pixels);
    terminal->pixels = pixels;
    terminal->rows = rows;
    terminal->cols = cols;
    terminal->scale = scale;
    terminal->font_size = font_size;
    terminal->config_revision = app_config_revision();
    terminal->cell_width = width;
    terminal->cell_height = height;
    vterm_set_size(terminal->vterm, rows, cols);
    struct winsize size_info = {.ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols,
        .ws_xpixel = (unsigned short)(cols * width), .ws_ypixel = (unsigned short)(rows * height)};
    if (terminal->master >= 0) (void)ioctl(terminal->master, TIOCSWINSZ, &size_info);
    invalidate(terminal);
}

void workspace_terminal_draw(WorkspaceTerminal *terminal, Rectangle box, bool focused,
                             float background_opacity)
{
    terminal_size(terminal, box);
    Color pane_background = background;
    pane_background.a = (unsigned char)(255 * background_opacity + .5f);
    if (terminal->background_alpha != pane_background.a) {
        terminal->background_alpha = pane_background.a;
        invalidate(terminal);
    }

    bool translucent = pane_background.a < 255;
    if (translucent) {
        rlSetBlendFactors(RL_ONE, RL_ZERO, RL_FUNC_ADD);
        BeginBlendMode(BLEND_CUSTOM);
        pane_background = premultiply(pane_background);
    }
    DrawRectangleRec(box, pane_background);
    if (translucent) EndBlendMode();
    if (terminal->pixels.id) {
        bool redraw = false;
        for (int i = 0; i < terminal->rows; ++i) redraw |= terminal->dirty[i];
        if (redraw) {
            BeginTextureMode(terminal->pixels);
            rlSetBlendFactorsSeparate(RL_SRC_ALPHA, RL_ONE_MINUS_SRC_ALPHA,
                                      RL_ONE, RL_ONE_MINUS_SRC_ALPHA, RL_FUNC_ADD, RL_FUNC_ADD);
            if (!translucent) BeginBlendMode(BLEND_CUSTOM_SEPARATE);
            for (int i = 0; i < terminal->rows; ++i)
                if (terminal->dirty[i]) draw_row(terminal, i);
            EndBlendMode();
            EndTextureMode();
        }
        Texture2D texture = terminal->pixels.texture;
        Rectangle area = {box.x + 6, box.y + 6,
                           texture.width / terminal->scale, texture.height / terminal->scale};
        BeginScissorMode((int)box.x + 1, (int)box.y + 1, (int)box.width - 2, (int)box.height - 2);
        // Replace the pane fill with the cached cells without blending the background twice.
        if (translucent) BeginBlendMode(BLEND_CUSTOM);
        DrawTexturePro(texture, (Rectangle){0, 0, texture.width, -texture.height},
                       area, (Vector2){0}, 0, WHITE);
        if (translucent) EndBlendMode();
        if (!workspace_terminal_exited(terminal) && terminal->cursor_visible && !terminal->scroll &&
            (!terminal->cursor_blink || !focused || (int)(GetTime() * 2) % 2 == 0)) {
            float w = terminal->cell_width / terminal->scale;
            float h = terminal->cell_height / terminal->scale;
            Rectangle cursor = {area.x + terminal->cursor.col * w, area.y + terminal->cursor.row * h, w, h};
            if (terminal->cursor_shape == VTERM_PROP_CURSORSHAPE_UNDERLINE)
                DrawRectangleRec((Rectangle){cursor.x, cursor.y+h-2, w, 2}, foreground);
            else if (terminal->cursor_shape == VTERM_PROP_CURSORSHAPE_BAR_LEFT)
                DrawRectangleRec((Rectangle){cursor.x, cursor.y, 2, h}, foreground);
            else DrawRectangleLinesEx(cursor, 1, foreground);
        }
        EndScissorMode();
    }
    DrawRectangleLinesEx(box, 1, (Color){69,71,90,255});
    if (focused) DrawRectangle((int)box.x, (int)box.y, (int)box.width, 1, (Color){137,220,235,255});
}

static VTermModifier modifiers(void)
{
    VTermModifier mod = VTERM_MOD_NONE;
    if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) mod |= VTERM_MOD_SHIFT;
    if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) mod |= VTERM_MOD_CTRL;
    if (IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT)) mod |= VTERM_MOD_ALT;
    return mod;
}

static void scroll_by(WorkspaceTerminal *terminal, int lines)
{
    size_t next = (size_t)clamp((int)terminal->scroll + lines, 0, (int)terminal->history_count);
    if (terminal->scroll == next) return;
    terminal->scroll = next;
    invalidate(terminal);
}

static bool pressed(int key)
{
    return IsKeyPressed(key) || IsKeyPressedRepeat(key);
}

void workspace_terminal_input(WorkspaceTerminal *terminal)
{
    if (!terminal || terminal->master < 0) return;
    VTermModifier mod = modifiers();
    bool shift = mod & VTERM_MOD_SHIFT, ctrl = mod & VTERM_MOD_CTRL;
    if (!terminal->alt_screen && shift && (pressed(KEY_PAGE_UP) || pressed(KEY_PAGE_DOWN))) {
        scroll_by(terminal, pressed(KEY_PAGE_UP) ? terminal->rows : -terminal->rows);
        return;
    }
    if (shift && ctrl && IsKeyPressed(KEY_V)) {
        const char *clipboard = GetClipboardText();
        size_t space = sizeof terminal->pending - (terminal->queued - terminal->written);
        if (clipboard && space >= 16 && strlen(clipboard) <= space - 16) {
            vterm_keyboard_start_paste(terminal->vterm);
            queue_output(clipboard, strlen(clipboard), terminal);
            vterm_keyboard_end_paste(terminal->vterm);
            scroll_by(terminal, -(int)terminal->scroll);
            flush_input(terminal);
        }
        return;
    }
    static const struct { int ray; VTermKey term; } keys[] = {
        {KEY_ENTER,VTERM_KEY_ENTER},{KEY_TAB,VTERM_KEY_TAB},
        {KEY_BACKSPACE,VTERM_KEY_BACKSPACE},{KEY_ESCAPE,VTERM_KEY_ESCAPE},
        {KEY_UP,VTERM_KEY_UP},{KEY_DOWN,VTERM_KEY_DOWN},{KEY_LEFT,VTERM_KEY_LEFT},
        {KEY_RIGHT,VTERM_KEY_RIGHT},{KEY_INSERT,VTERM_KEY_INS},{KEY_DELETE,VTERM_KEY_DEL},
        {KEY_HOME,VTERM_KEY_HOME},{KEY_END,VTERM_KEY_END},{KEY_PAGE_UP,VTERM_KEY_PAGEUP},
        {KEY_PAGE_DOWN,VTERM_KEY_PAGEDOWN},{KEY_KP_ENTER,VTERM_KEY_KP_ENTER},
    };
    size_t before = terminal->queued - terminal->written;
    for (size_t i = 0; i < sizeof keys / sizeof *keys; ++i)
        if (pressed(keys[i].ray)) vterm_keyboard_key(terminal->vterm, keys[i].term, mod);
    for (int key = KEY_F1; key <= KEY_F12; ++key)
        if (pressed(key)) vterm_keyboard_key(terminal->vterm, VTERM_KEY_FUNCTION(key-KEY_F1+1), mod);
    if (ctrl) {
        /* Keep the conventional control bytes for shells and readline. */
        for (int key = KEY_A; key <= KEY_Z; ++key)
            if (pressed(key)) {
                char code = (char)(key - KEY_A + 1);
                if (mod & VTERM_MOD_ALT) queue_output("\033", 1, terminal);
                queue_output(&code, 1, terminal);
            }
        if (pressed(KEY_SPACE)) queue_output("\0", 1, terminal);
        if (pressed(KEY_LEFT_BRACKET)) queue_output("\033", 1, terminal);
        if (pressed(KEY_BACKSLASH)) queue_output("\034", 1, terminal);
        if (pressed(KEY_RIGHT_BRACKET)) queue_output("\035", 1, terminal);
        if (shift && pressed(KEY_SIX)) queue_output("\036", 1, terminal);
        if (shift && pressed(KEY_MINUS)) queue_output("\037", 1, terminal);
    }
    int character;
    while ((character = app_input_char()) != 0)
        if (!ctrl && character >= 32 && character != 127)
            vterm_keyboard_unichar(terminal->vterm, (uint32_t)character, mod);
    if (terminal->queued - terminal->written > before)
        scroll_by(terminal, -(int)terminal->scroll);
    flush_input(terminal);
}

void workspace_terminal_focus(WorkspaceTerminal *terminal, bool focused)
{
    if (terminal->focused == focused) return;
    terminal->focused = focused;
    VTermState *state = vterm_obtain_state(terminal->vterm);
    if (focused) vterm_state_focus_in(state);
    else {
        for (int i = 0; i < 3; ++i)
            if (terminal->buttons & (1u << i))
                vterm_mouse_button(terminal->vterm, i + 1, false, VTERM_MOD_NONE);
        terminal->buttons = 0;
        vterm_state_focus_out(state);
    }
}

void workspace_terminal_pointer(WorkspaceTerminal *terminal, Rectangle box, Vector2 point)
{
    if (!terminal->pixels.id) return;
    float wheel = GetMouseWheelMove();
    if ((wheel || terminal->scroll) && (!terminal->mouse_mode || terminal->scroll ||
        IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT))) {
        if (!terminal->alt_screen) scroll_by(terminal, wheel > 0 ? 3 : wheel < 0 ? -3 : 0);
        return;
    }
    if (!terminal->mouse_mode || terminal->master < 0) return;
    int row = clamp((int)((point.y - box.y - 6) * terminal->scale / terminal->cell_height), 0, terminal->rows-1);
    int col = clamp((int)((point.x - box.x - 6) * terminal->scale / terminal->cell_width), 0, terminal->cols-1);
    VTermModifier mod = modifiers();
    vterm_mouse_move(terminal->vterm, row, col, mod);
    const int buttons[] = {MOUSE_BUTTON_LEFT, MOUSE_BUTTON_MIDDLE, MOUSE_BUTTON_RIGHT};
    for (int i = 0; i < 3; ++i) {
        unsigned int mask = 1u << i;
        if (IsMouseButtonPressed(buttons[i])) {
            terminal->buttons |= mask;
            vterm_mouse_button(terminal->vterm, i+1, true, mod);
        }
        if ((terminal->buttons & mask) && !IsMouseButtonDown(buttons[i])) {
            terminal->buttons &= ~mask;
            vterm_mouse_button(terminal->vterm, i+1, false, mod);
        }
    }
    if (wheel) {
        int button = wheel > 0 ? 4 : 5;
        vterm_mouse_button(terminal->vterm, button, true, mod);
        vterm_mouse_button(terminal->vterm, button, false, mod);
    }
    flush_input(terminal);
}

#else
#include <stdio.h>

WorkspaceTerminal *workspace_terminal_open(const char *edit_path, Rectangle box, char *error, size_t capacity)
{
    (void)edit_path;
    (void)box;
    snprintf(error, capacity, "Terminal panes require Linux or macOS");
    return NULL;
}
void workspace_terminal_close(WorkspaceTerminal *terminal) { (void)terminal; }
bool workspace_terminal_exited(const WorkspaceTerminal *terminal) { (void)terminal; return true; }
void workspace_terminal_poll(WorkspaceTerminal *terminal, WorkspaceRunRequest run, void *context)
{ (void)terminal; (void)run; (void)context; }
void workspace_terminal_draw(WorkspaceTerminal *terminal, Rectangle box, bool focused, float background_opacity)
{ (void)terminal; (void)box; (void)focused; (void)background_opacity; }
void workspace_terminal_focus(WorkspaceTerminal *terminal, bool focused)
{ (void)terminal; (void)focused; }
void workspace_terminal_input(WorkspaceTerminal *terminal) { (void)terminal; }
void workspace_terminal_pointer(WorkspaceTerminal *terminal, Rectangle box, Vector2 point)
{ (void)terminal; (void)box; (void)point; }
void workspace_terminal_reply(int fd, const char *error) { (void)fd; (void)error; }
int workspace_terminal_request_run(const char *source)
{ (void)source; fputs("Terminal panes require Linux or macOS.\n", stderr); return 1; }
#endif
