#ifndef MC_APP_CONFIG_H
#define MC_APP_CONFIG_H

#include <stdbool.h>

typedef struct {
    bool vsync;
    int fps_limit, msaa_samples, stream_radius;
    float fov, mouse_sensitivity, move_speed;
} AppSettings;

bool app_config_init(void);
const AppSettings *app_config_current(void);
const char *app_config_error(void);

/* Snapshot physical input once per GUI frame. */
void app_input_frame(void);
bool app_input_world(const char *action, bool held);
int app_input_char(void);

#endif
