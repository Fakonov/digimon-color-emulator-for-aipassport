// On-device settings menu, styled after the digimon-passport panel: an ivory
// card with a dark olive border, a yellow bar on the selected row, the label on
// the left and the value right-aligned.
//
// The menu deliberately owns no timer and no input polling: app_main decides
// when it opens (long B), feeds it presses and paints it.  That keeps the
// "which frame does the guest see" question answered in one place.
#pragma once

#include <stdint.h>

enum { MENU_ITEMS = 5 };
enum {
    MENU_IDX_AUTO_OFF = 0,
    MENU_IDX_BRIGHT   = 1,
    MENU_IDX_VOLUME   = 2,
    MENU_IDX_AUTOSAVE = 3,
    MENU_IDX_ABOUT    = 4,
};

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

// Pushes the menu straight to the panel instead of drawing into fb[].
//
// Drawing into fb[] does not work: the guest only repaints the pixels it
// animates, so the settings text stays on screen after the menu closes, and
// there is no DRAM left (10 KB free) for the 38 KB copy of the window that
// restoring it would need.  `strip` is the caller's tile-transfer scratch
// buffer; it holds w * 2 * 24 bytes, which is why the menu goes out in bands of
// 24 rows.  Closing the menu only requires re-pushing fb[] once.
void menu_present(uint8_t *strip, int w, int h);

// One line of text in the panel's top margin, above the guest window.  Used to
// say the real machine would already be dark, which only happens when the
// emulator's auto-off is set longer than the machine's 16 s.  `strip` needs
// 2 * width * 8 bytes and width is the whole panel width (320).
void menu_banner_present(uint8_t *strip, int width, const char *text);
