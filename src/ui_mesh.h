#pragma once

// Removes NGUI's per-rebuild mesh array copies. Call after linking and
// before the libraries' constructors (Unity registers its icalls later).
void ui_mesh_install(void);
// Mesh setter calls served from the shared NGUI buffers so far.
unsigned ui_mesh_clamped_calls(void);
// The mono_add_internal_call interposer once installed, else NULL.
const void *ui_mesh_icall_registrar(void);
