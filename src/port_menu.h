#ifndef SRR_PORT_MENU_H
#define SRR_PORT_MENU_H

// Port settings. They are edited on a screen shown before the game loads
// (LiveArea "Settings" button, or SELECT held at launch), stored in
// ux0:data/srr/port_settings.txt and applied only at boot.

enum { PORT_RES_SD, PORT_RES_HD, PORT_RES_QHD, PORT_RES_COUNT };
enum { PORT_QUALITY_LOW, PORT_QUALITY_MEDIUM, PORT_QUALITY_HIGH, PORT_QUALITY_COUNT };

// Render size handed to vitaGL and Unity, and used to map touch input.
extern int port_screen_w, port_screen_h;

void port_settings_load(void);       // first thing at boot
int port_settings_requested(void);   // LiveArea link or SELECT held
int port_settings_screen(void);      // 1: start the game, 0: exit
void port_menu_init(void);           // after gl_bridge_init()

int port_quality(void);              // PORT_QUALITY_*
int port_unity_quality_level(void);  // QualitySettings level for port_quality()
int port_texture_limit(void);        // masterTextureLimit for port_quality()
int port_target_fps(void);           // 30 or 60
int port_character_shader_original(void);

// mono_probe.c, on the Unity thread.
void mono_port_quality_apply(void);

#endif
