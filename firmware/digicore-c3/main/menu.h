// On-device settings menu, styled after the digimon-passport panel: an ivory
// card with a dark olive border, a yellow bar on the selected row, the label on
// the left and the value right-aligned.
//
// The menu deliberately owns no timer and no input polling: app_main decides
// when it opens (long B), feeds it presses and paints it.  That keeps the
// "which frame does the guest see" question answered in one place.
#pragma once

#include <stdint.h>

enum { MENU_ITEMS = 7 };
enum {
    MENU_IDX_AUTO_OFF = 0,
    MENU_IDX_BRIGHT   = 1,
    MENU_IDX_VOLUME   = 2,
    MENU_IDX_AUTOSAVE = 3,
    MENU_IDX_SAVE_OUT = 4,
    MENU_IDX_SAVE_IN  = 5,
    MENU_IDX_ABOUT    = 6,
};

// Rows that are commands rather than values.  menu_press_b() on one of them
// queues the request here; app_main drains it with menu_take_action() so the
// menu module never has to know how a snapshot is stored.
enum { MENU_ACTION_NONE = 0, MENU_ACTION_SAVE = 1, MENU_ACTION_LOAD = 2 };
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

// Debug aid: dump the next `frames` menu pushes to the console as downsampled
// hex rows ("B <band> <y> <pixels>").  The menu paints straight to the panel
// and never touches fb[], so this is the only way to check its output from the
// serial console.  0 turns it back off.
void menu_set_capture(int frames);

// ---- A+C confirmation card ------------------------------------------------
//
// Long A raises this card instead of injecting the chord.  The cartridge's
// title screen drops a pad that reads A+C, so the chord has to be an explicit
// second step: while the card is up the guest sees no key at all, B sends the
// chord, A or C dismisses it.
void menu_confirm_set(bool on);
bool menu_confirm_is_open();
void menu_confirm_present(uint8_t *strip, int w, int h, const MenuBackdrop *bg);
