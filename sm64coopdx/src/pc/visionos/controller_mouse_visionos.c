/*
 * Pointer input for visionOS, replacing src/pc/controller/controller_mouse.c.
 *
 * djui's menus are written against a mouse, and gaze-plus-pinch is close enough
 * to one that feeding it into the same globals makes the whole existing UI work
 * without touching djui. The app pushes events in through
 * mariovision_pointer_* rather than the backend polling for them.
 *
 * Relative (captured cursor) mode has no counterpart on visionOS - there is no
 * cursor to warp and no way to hide the system one - so the mouselook path
 * stays inert. First-person camera control comes from the right stick instead.
 */

#include <stdbool.h>
#include <stdint.h>

#include "pc/controller/controller_mouse.h"
#include "mariovision_platform.h"

bool mouse_init_ok = true;

u32 mouse_buttons = 0;
s32 mouse_x = 0;
s32 mouse_y = 0;

u32 mouse_window_buttons = 0;
u32 mouse_window_buttons_pressed = 0;
u32 mouse_window_buttons_released = 0;
s32 mouse_window_x = 0;
s32 mouse_window_y = 0;

u32 mouse_scroll_timestamp = 0;
f32 mouse_scroll_x = 0.0f;
f32 mouse_scroll_y = 0.0f;

bool mouse_relative_enabled = false;

// Set by the app, consumed on the next controller_mouse_read_window().
static u32 sPendingPressed = 0;
static u32 sPendingReleased = 0;

void mariovision_pointer_moved(int32_t x, int32_t y) {
    mouse_window_x = x;
    mouse_window_y = y;
}

void mariovision_pointer_button(bool down) {
    if (down) {
        sPendingPressed |= L_MOUSE_BUTTON;
        mouse_window_buttons |= L_MOUSE_BUTTON;
    } else {
        sPendingReleased |= L_MOUSE_BUTTON;
        mouse_window_buttons &= ~L_MOUSE_BUTTON;
    }
}

void controller_mouse_read_window(void) {
    // Edge flags are latched by the app between frames and cleared once the
    // game has seen them, which is what the SDL backend's press/release
    // bookkeeping amounts to.
    mouse_window_buttons_pressed = sPendingPressed;
    mouse_window_buttons_released = sPendingReleased;
    sPendingPressed = 0;
    sPendingReleased = 0;
}

void controller_mouse_read_relative(void) {
    // No relative pointer on visionOS; deltas stay at zero.
    mouse_x = 0;
    mouse_y = 0;
    mouse_buttons = mouse_window_buttons;
}

void controller_mouse_enter_relative(void) {
    mouse_relative_enabled = false;
}

void controller_mouse_leave_relative(void) {
    mouse_relative_enabled = false;
}

void mouse_on_scroll(float x, float y) {
    mouse_scroll_x = x;
    mouse_scroll_y = y;
    mouse_scroll_timestamp++;
}
