/*
 * The C interface the MarioVision app target drives the game through.
 *
 * On desktop, coopdx owns the run loop: gWindowApi->main_loop(produce_one_frame)
 * never returns. On visionOS the app owns it instead - a CADisplayLink in phase
 * one, a CompositorServices frame loop in phase two - so the loop is inverted
 * and the app calls in here once per frame.
 */

#ifndef MARIOVISION_PLATFORM_H
#define MARIOVISION_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//-----------------------------------------------------------------------------
// Backends the visionOS build selects instead of the SDL/OpenGL ones
//-----------------------------------------------------------------------------

struct GfxWindowManagerAPI;
struct AudioAPI;
struct ControllerAPI;

extern struct GfxWindowManagerAPI gfx_visionos_wm_api;
extern struct AudioAPI audio_avf;
extern struct ControllerAPI controller_visionos;

//-----------------------------------------------------------------------------
// Frame loop
//-----------------------------------------------------------------------------

/// Tells the window manager backend how large the drawable is. Call before the
/// first frame and on every resize; gfx_pc reads this to size its viewport.
void mariovision_set_drawable_size(uint32_t width, uint32_t height);

/// Publishes the compositor's refresh rate, which pc_main.c uses to decide how
/// many frames to draw per game tick.
void mariovision_set_refresh_rate(int hz);

/// Boots the game: creates the Metal device and starts coopdx on its own
/// thread. Call once at app launch, after the ROM is in place. No window is
/// needed - the game loads and simply drops frames until a layer arrives.
void mariovision_boot(void);

#ifdef __OBJC__
@class CAMetalLayer;
/// Hands the backend the layer to render into. Callable any time after
/// mariovision_boot, from the main thread.
void mariovision_set_metal_layer(CAMetalLayer *layer);

/// Detaches `layer` if it is still the active one. Safe to call from every
/// teardown path - a stale layer never clears a newer one.
void mariovision_clear_metal_layer(CAMetalLayer *layer);
#endif

/// Whether the game window is currently providing a layer. Doubles as "is the
/// game visible": audio is gated on it, and the launcher uses it to tell
/// "running in its window" from "running with the window closed".
bool mariovision_has_layer(void);

/// Window layer present or portal mode active - "can anyone see the game".
bool mariovision_is_visible(void);

/// Loaded but invisible: game loop and audio thread nap in this state.
bool mariovision_should_idle(void);

/// Called by mariovision_boot; not part of the app-facing surface.
void mariovision_start_game_thread(void);

//-----------------------------------------------------------------------------
// Pointer input
//
// Gaze plus pinch on visionOS behaves like a mouse, which is exactly what djui's
// menus expect, so it is fed into the same globals controller_mouse.c would.
// Coordinates are in drawable pixels, origin top-left.
//-----------------------------------------------------------------------------

void mariovision_pointer_moved(int32_t x, int32_t y);
void mariovision_pointer_button(bool down);
void mariovision_pointer_scroll(float x, float y);

//-----------------------------------------------------------------------------
// Native interface
//
// With this on, coopdx's own startup menus - language, host settings, mod list
// - are not drawn, and the app's SwiftUI surfaces drive that part instead. The
// in-game UI (pause, chat, Lua HUD) is untouched either way.
//
// The default lives in mariovision_ui.c as MARIOVISION_NATIVE_UI_DEFAULT.
//-----------------------------------------------------------------------------

bool mariovision_native_ui_enabled(void);
void mariovision_set_native_ui_enabled(bool enabled);

/// Loaded and sitting in the main menu - the Start button has something to do.
bool mariovision_game_ready(void);

/// Loaded and playing.
bool mariovision_game_running(void);

/// The player asked for the menu (R on the keyboard, Start on a pad). Set from
/// the input layer, taken by the interface on its next poll - which then brings
/// the launcher back rather than opening a second one.
void mariovision_request_menu(void);

/// Whether the R shortcut may raise that request right now. It may not during
/// play: R is a game button, and on a pad it is the right shoulder.
bool mariovision_menu_key_allowed(void);
bool mariovision_take_menu_request(void);

/// Starts a single-player game, always skipping the intro cutscene.
/// Queued; runs on the game thread at the next frame.
void mariovision_start_game(void);

/// Starts save file `slot` (1...4), always skipping the intro cutscene.
/// Queued; runs on the game thread at the next frame.
void mariovision_start_save(int slot);
/// Erases save file `slot` (1...4). Queued; runs on the game thread.
void mariovision_erase_save(int slot);
/// Star count for a file, or -1 before the engine has published a snapshot.
/// Safe to call from the UI thread.
int mariovision_save_stars(int slot);
/// Whether that file already has progress. Safe to call from the UI thread.
bool mariovision_save_exists(int slot);
/// True while an erase is queued or in progress.
bool mariovision_save_busy(void);

/// The languages the bundle ships, from lang/*.ini.
/// `key` is the file's own name ("German"), `display` the translated one.
/// The strings are copied out: the list can be rebuilt on the game thread the
/// moment a language is applied.
int  mariovision_language_count(void);
void mariovision_language_key(int index, char *out, int outSize);
void mariovision_language_display(int index, char *out, int outSize);
int  mariovision_language_current(void);

/// Queued; runs on the game thread at the next frame.
void mariovision_language_select(int index);

/// Runs the queued commands and keeps the menu suppression in sync.
/// Called by the window backend once per game iteration - not from the app.
void mariovision_ui_pump(void);

#ifdef __cplusplus
}
#endif

#endif // MARIOVISION_PLATFORM_H
