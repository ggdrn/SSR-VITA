#ifndef SRR_PORT_MENU_H
#define SRR_PORT_MENU_H

// Port settings. They are edited on a screen shown before the game loads
// (LiveArea "Settings" button, or SELECT held at launch), stored in
// ux0:data/srr/port_settings.txt and applied only at boot.

enum { PORT_RES_SD, PORT_RES_HD, PORT_RES_QHD, PORT_RES_COUNT };
enum { PORT_QUALITY_LOW, PORT_QUALITY_DEFAULT, PORT_QUALITY_COUNT };

// Render size handed to vitaGL and Unity, and used to map touch input.
extern int port_screen_w, port_screen_h;

void port_settings_load(void);       // first thing at boot
int port_settings_requested(int argc, char *argv[]);  // LiveArea link, restart arg or SELECT held
void port_settings_poll(void);       // in game: LiveArea Settings tapped -> restart into it
int port_settings_screen(void);      // 1: start the game, 0: exit
void port_shader_progress(unsigned phase, unsigned done, unsigned total, unsigned prepared);
void port_menu_init(void);           // after gl_bridge_init()

int port_quality(void);              // PORT_QUALITY_*
int port_unity_quality_level(void);  // QualitySettings level for port_quality()
int port_texture_limit(void);        // masterTextureLimit for port_quality()
int port_material_shaders(void);    // lighting/reflections/material variants; default on
int port_target_fps(void);           // 30 or 60

// mono_probe.c, on the Unity thread.
void mono_port_quality_apply(void);

#endif
