#ifndef SRR_PORT_MENU_H
#define SRR_PORT_MENU_H

// Port settings menu (L + R + START). Settings live in PORT_SETTINGS_PATH and
// are applied at boot and whenever they change in the menu.

enum { PORT_RES_SD, PORT_RES_HD, PORT_RES_QHD, PORT_RES_COUNT };
enum { PORT_QUALITY_LOW, PORT_QUALITY_MEDIUM, PORT_QUALITY_HIGH, PORT_QUALITY_COUNT };

// Current render size handed to Unity and used to map touch input.
extern int port_screen_w, port_screen_h;

void port_settings_load(void);   // before gl_bridge_init()
void port_menu_init(void);       // after gl_bridge_init()
// Called once per frame on the Unity thread, before input_poll(). Returns 1
// while the menu owns the controls (the game must not read input).
int port_menu_tick(void);

int port_quality(void);              // PORT_QUALITY_*
int port_unity_quality_level(void);  // QualitySettings level for port_quality()
int port_texture_limit(void);        // masterTextureLimit for port_quality()
int port_target_fps(void);           // 30 or 60
int port_character_shader_original(void);

// mono_probe.c: game-side settings, run on the Unity thread.
int mono_port_light_mode(void);      // -1 while the save data is not loaded
int mono_port_set_light_mode(int on);
void mono_port_quality_apply(void);
void mono_port_frame_rate_apply(void);

#endif
