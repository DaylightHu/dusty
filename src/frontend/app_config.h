#ifndef MC_APP_CONFIG_H
#define MC_APP_CONFIG_H
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    APP_WORKSPACE, APP_WORLD, APP_SCOPE_COUNT
} AppKeyScope;
typedef struct {
    bool vsync;
    int fps_limit, msaa_samples, terminal_font_size;
    int stream_radius, leader_timeout_ms;
    float fov, mouse_sensitivity, move_speed, scroll_lines;
    float terminal_background_opacity;
    char terminal_font[4096];
} AppSettings;

/* A failed reload preserves the complete previous configuration. */
bool app_config_init(void);
bool app_config_reload(void);
bool app_config_ensure_file(void);
const AppSettings *app_config_current(void);
const char *app_config_path(void);
const char *app_config_error(void);
unsigned app_config_revision(void);
size_t app_config_action_count(AppKeyScope scope);
const char *app_config_action_name(AppKeyScope scope, size_t index);
void app_config_describe(AppKeyScope scope, const char *action, char *text, size_t capacity);

/* Snapshot physical input once per GUI frame. */
void app_input_frame(void);
bool app_input_leader(void);
const char *app_input_workspace(bool after_leader);
bool app_input_world(const char *action, bool held);
int app_input_char(void);
void app_input_discard(void);
#endif
