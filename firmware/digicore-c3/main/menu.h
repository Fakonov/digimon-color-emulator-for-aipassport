// On-device settings menu, styled after the digimon-passport panel: an ivory
// card with a dark olive border, a yellow bar on the selected row, the label on
// the left and the value right-aligned.
//
// The menu deliberately owns no timer and no input polling: app_main decides
// when it opens (long A), feeds it presses and paints it.  That keeps the
// "which frame does the guest see" question answered in one place.
#pragma once

#include <stdint.h>

// The first two items are the button combos the retail handset cannot produce
// on its own: the three keys share one resistor ladder, so a real A+C or A+B
// can never be pressed.  The official manual lists two of them as real features
// (A+C on the clock screen changes the time, A+B on the grave hatches a new
// egg), so they live as command rows instead of gestures.
//
// TIME LAPSE is the emulator's answer to "let it grow faster": the ROM already
// carries a 60x flag (RAM 0x0D03, internal ROM 0x5C10 adds +0x3C seconds per
// carry), and feeding the software RTC extra half-second ticks lets the host
// pick any rate between 1x and 60x.  Growth, hunger, sleep and death all run on
// the same second-carry pulse, so they accelerate together and keep their
// original order - unlike editing the clock, which the ROM explicitly does not
// let affect growth.
// Seven rows.  The firmware version used to be an eighth row (ABOUT/VERSION),
// but the card already prints it as a muted tag in the top-right corner next to
// the SETTINGS title, so the row only repeated what the header said (removed in
// v1.17.1).
enum { MENU_ITEMS = 7 };
enum {
    MENU_IDX_CHORD    = 0,   // SEND A+C
    MENU_IDX_CHORD_AB = 1,   // SEND A+B
    MENU_IDX_LAPSE    = 2,   // TIME LAPSE: 1x / 15x / 60x / 300x
    MENU_IDX_AUTO_OFF = 3,
    MENU_IDX_BRIGHT   = 4,
    MENU_IDX_VOLUME   = 5,
    MENU_IDX_AUTOSAVE = 6,
};
// SAVE OUT / SAVE IN were rows 6 and 7 until 2026-11-25.  A manual load restores
// the newest snapshot, which is the autosave one, and those are taken while the
// guest sits in its ROM park loop: the CPU cannot resume there, so the load
// replayed power-on and the game restarted.  It also fought the autosave ring
// (one "save" could be overwritten by the next autosave 60 s later), so the two
// rows were removed.  The autosave covers the power-cut case; the console still
// has `D` and `U` for bench work.

// Rows that are commands rather than values.  menu_press_b() on one of them
// queues the request here; app_main drains it with menu_take_action() so the
// menu module never has to know how a snapshot is stored or how a chord is
// injected.
enum {
    MENU_ACTION_NONE = 0,
    MENU_ACTION_CHORD = 1,
    MENU_ACTION_CHORD_AB = 2,
};
int  menu_take_action();      // returns the queued action and clears it

// Reads the persisted values out of NVS.  Safe to call once at boot; on any
// failure the compile-time defaults stay in place.
void menu_load();

void menu_open();
void menu_close();
bool menu_is_open();

void menu_press_a();          // move the cursor to the next item
void menu_press_b();          // advance the selected item's value
void menu_press_c();          // leave the menu

int  menu_auto_off_sec();     // 16 = follow the machine, 0 = never sleep
int  menu_bright_pct();       // 20..100
int  menu_volume_pct();       // 0..100
int  menu_autosave_sec();     // 0 = off, otherwise the guest-state snapshot period

// Game-time multiplier: 1 (off), 15, 60 or 300.  Above 60 the host also sets
// the ROM's own 60x flag, so one injected carry becomes a whole minute.
int  menu_time_lapse_x();
// Console/test hook: pick a mode by index (0..3) and persist it.  Returns the
// multiplier that is now selected.
int  menu_set_time_lapse_idx(int idx);
int  menu_time_lapse_idx();

// Where the emulator's framebuffer sits on the panel.  The menu pushes whole
// scanlines, so the pixels it does not own have to come from somewhere: filling
// them black would erase the game behind the card.  app_main describes the
// window once and the menu samples it, upscaling the same way blit_frame() does.
struct MenuBackdrop
{
    const uint16_t *pixels;   // guest framebuffer, fb[y * stride + x]
    int stride;               // pixels per framebuffer row (128)
    int x, y;                 // window origin inside the framebuffer
    int w, h;                 // window size in framebuffer pixels
    int out_x, out_y;         // where that window lands on the panel
    int scale;                // panel pixels per framebuffer pixel
};

// Pushes the menu straight to the panel instead of drawing into fb[].
//
// Drawing into fb[] does not work: the guest only repaints the pixels it
// animates, so the settings text stays on screen after the menu closes, and
// there is no DRAM left (10 KB free) for the 38 KB copy of the window that
// restoring it would need.  `strip` is the caller's tile-transfer scratch
// buffer; it holds w * 2 * 24 bytes, which is why the menu goes out in bands of
// 24 rows.  `bg` supplies the pixels outside the card; pass nullptr for black.
// Closing the menu only requires re-pushing fb[] once.
void menu_present(uint8_t *strip, int w, int h, const MenuBackdrop *bg);

// One line of text in the panel's top margin, above the guest window.  Used to
// say the real machine would already be dark, which only happens when the
// emulator's auto-off is set longer than the machine's 16 s.  `strip` needs
// 2 * width * 8 bytes and width is the whole panel width (320).
void menu_banner_present(uint8_t *strip, int width, const char *text);

// Single-line time-lapse notice: 240x22 in the 24 px strip below the guest
// window (the window ends at y = 216 of 240), so it covers neither the pet nor
// the bottom status-icon row.  It is modal while it is up: C stops the lapse,
// B only dismisses the notice, everything else is swallowed.  It is redrawn
// every frame on top of the live picture, which is cheap at this size
// (5.3 k pixels against the 37 k of the blit).  `strip` needs 2 * 240 * 22.
void menu_lapse_toast_present(uint8_t *strip, int w, int h, const char *speed);

// Standby card: one line of white text on black, drawn inside the guest window
// only - the brick wall, the metal rim and the battery corner stay as they are,
// so this reads as the game picture being replaced rather than the machine
// being off.  (x,y,w,h) is the window on the panel (192x192 at 64,24).  Pushed
// straight at the panel in 24-row bands, so `strip` needs 2 * w * 24 bytes; the
// caller must not blit the window again until it wakes.
void menu_standby_present(uint8_t *strip, int x, int y, int w, int h, const char *text);

// Debug aid: dump the next `frames` menu pushes to the console as downsampled
// hex rows ("B <band> <y> <pixels>").  The menu paints straight to the panel
// and never touches fb[], so this is the only way to check its output from the
// serial console.  0 turns it back off.
void menu_set_capture(int frames);

// ---- A+C confirmation card (compiled out: C3_CONFIRM_CARD = 0) -------------
//
// Superseded by row 0 of the settings menu: the chord is a command inside the
// card now, so nothing raises this modal any more - menu_confirm_set(true) has
// no caller, menu_confirm_is_open() is constant false, and the 1,868 B card
// body could never paint (R7 audit).  menu.cpp compiles it out unless
// -DC3_CONFIRM_CARD=1; the two predicates stay so call sites do not need an
// #if of their own.
void menu_confirm_set(bool on);
bool menu_confirm_is_open();
void menu_confirm_present(uint8_t *strip, int w, int h, const MenuBackdrop *bg);
