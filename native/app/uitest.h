// Headless UI test infrastructure.
//
// `--uitest <project.txt|SCRIPT.UNI> [outdir]` runs a scripted regression
// through the REAL render loop: document ops (edit/add/move/delete/undo/redo/
// asm), filter states, tab switching (via the public ImGuiTabItemFlags
// API — headless mouse clicks can't select unselected tabs due to a
// vendored-TabBar quirk, but the io-injection Click step is available for
// widgets that do respond), per-slot budget invariants, and framebuffer
// dumps (the DX11 backbuffer is read back and written as PNGs into outdir).
// Checks go to <outdir>/uitest.log; the exit code is 0 only if all passed.
//
// `--screenshot <out.png> [project]` pumps ~90 frames, saves one PNG, exits.
#pragma once
#include <cstddef>
#include <string>

struct App;

// Active tab index rendered by main.cpp (0 = Dialogue editor, 1 = Direct
// script); set during UI rendering, read by uitest steps.
extern int g_activeTab;

// Test hook: force-select a tab via ImGuiTabItemFlags_SetSelected on the
// next frame (0 = none, 1 = Direct script, 2 = Dialogue editor).
extern int g_forceTab;

// Set by uitest steps / screenshot mode; consumed by main.cpp's
// after-render hook, which reads the backbuffer into this path.
extern std::string g_pendingDump;

// Tab geometry recorded by main.cpp during rendering (client coords):
// g_tabX0[i] = CENTER x of tab i, g_tabY0/g_tabY1 = tab bar top/bottom.
// (Infrastructure for future coordinate-based click tests.)
extern float g_tabX0[2];
extern float g_tabY0;
extern float g_tabY1;

// --uitest mode entry (main.cpp runs the loop; uitest drives it per frame).
void uitest_begin(App& a, const std::string& outdir);
bool uitest_finished();
void uitest_on_frame(App& a);   // called before the message pump each frame
int uitest_summary();           // prints check totals; returns exit code

// --screenshot mode helper: request a framebuffer dump on the next frame.
void uitest_request_dump(const std::string& path);

// main.cpp: registers the app window (kept for future window-message tests).
void uitest_set_hwnd(void* hwnd);

// Encodes a BGRA framebuffer (rowPitch = source stride) as a PNG file.
void uitest_save_png(const char* path, const unsigned char* bgra,
                     unsigned int w, unsigned int h, size_t rowPitch);

// editor_ui.cpp test hooks.
int ui_visible_row_count(App& a);
bool ui_visible_rows_match_kind(App& a, int kindFilter); // 1=0x118 2=0x11A 3=0x118|0x119
int ui_visible_advance_count(App& a);                    // visible 0x119 rows
const char* ui_ru_buffer();                              // detail-panel RU buffer
int ui_sel_row_index(App& a);                            // selection in visible rows
bool ui_ru_field(int* x, int* y);                        // RU field center (client coords)
