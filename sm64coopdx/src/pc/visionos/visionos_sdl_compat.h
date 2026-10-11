/*
 * SDL2 compatibility surface for visionOS.
 *
 * Declares exactly the SDL symbols that the sources kept in the visionOS build
 * still reference, and nothing more. Every one of them is implemented natively
 * in visionos_sdl_compat.c / .m against Apple frameworks.
 *
 * Call sites, for reference:
 *   src/pc/platform.c                    paths, message box
 *   src/pc/pc_main.c                     display refresh rate
 *   src/pc/crash_handler.c               crash sound (inert here)
 *   src/pc/controller/controller_bind_mapping.c  scancode/keycode names
 *   src/pc/djui/djui_lua_profiler.c      high resolution timing
 *   src/pc/djui/djui_panel_controls.c    joystick enumeration
 */

#ifndef MARIOVISION_SDL_COMPAT_H
#define MARIOVISION_SDL_COMPAT_H

#include <stdint.h>
#include <stddef.h>

#include "SDL2/SDL_stdinc.h"
#include "SDL2/SDL_scancode.h"
#include "SDL2/SDL_keycode.h"

#ifdef __cplusplus
extern "C" {
#endif

//-----------------------------------------------------------------------------
// Memory / paths
//-----------------------------------------------------------------------------

// SDL hands out heap strings that the caller frees with SDL_free. We do the
// same with malloc so the existing ownership rules keep working unchanged.
void  SDL_free(void *mem);

// Application Support/sm64coopdx inside the app container. `org` is ignored on
// Apple platforms, matching SDL's own behaviour there.
char *SDL_GetPrefPath(const char *org, const char *app);

//-----------------------------------------------------------------------------
// Message box
//-----------------------------------------------------------------------------

#define SDL_MESSAGEBOX_ERROR   0x00000010
#define SDL_MESSAGEBOX_WARNING 0x00000020

// There is no modal alert we can raise from arbitrary threads before the
// SwiftUI scene exists, so this logs and hands the message to the app layer,
// which surfaces it once a scene is up.
int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, void *window);

//-----------------------------------------------------------------------------
// Timing
//-----------------------------------------------------------------------------

Uint64 SDL_GetPerformanceFrequency(void);
Uint64 SDL_GetPerformanceCounter(void);
Uint32 SDL_GetTicks(void);
void   SDL_Delay(Uint32 ms);

//-----------------------------------------------------------------------------
// Display
//-----------------------------------------------------------------------------

typedef struct SDL_DisplayMode {
    Uint32 format;
    int w;
    int h;
    int refresh_rate;
    void *driverdata;
} SDL_DisplayMode;

// Reports the compositor's refresh rate. The app layer publishes the real value
// via visionos_set_display_refresh_rate(); until then this reports 90.
int SDL_GetCurrentDisplayMode(int displayIndex, SDL_DisplayMode *mode);

//-----------------------------------------------------------------------------
// Joysticks (GameController framework)
//-----------------------------------------------------------------------------

int         SDL_NumJoysticks(void);
const char *SDL_JoystickNameForIndex(int deviceIndex);

//-----------------------------------------------------------------------------
// Keyboard names
//-----------------------------------------------------------------------------

const char *SDL_GetScancodeName(SDL_Scancode scancode);
SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode scancode);
const char *SDL_GetKeyName(SDL_Keycode key);

//-----------------------------------------------------------------------------
// Audio subsystem - referenced only by the crash handler's crash jingle, which
// has no counterpart here. Declared so crash_handler.c compiles; the init call
// always fails, so the whole block is inert.
//-----------------------------------------------------------------------------

#define SDL_INIT_AUDIO 0x00000010u
#define AUDIO_S16      0x8010
#define AUDIO_S16SYS   AUDIO_S16

typedef void (*SDL_AudioCallback)(void *userdata, Uint8 *stream, int len);

typedef struct SDL_AudioSpec {
    int freq;
    Uint16 format;
    Uint8 channels;
    Uint8 silence;
    Uint16 samples;
    Uint16 padding;
    Uint32 size;
    SDL_AudioCallback callback;
    void *userdata;
} SDL_AudioSpec;

Uint32 SDL_WasInit(Uint32 flags);
int    SDL_InitSubSystem(Uint32 flags);
int    SDL_OpenAudioDevice(const char *device, int iscapture,
                           const SDL_AudioSpec *desired, SDL_AudioSpec *obtained,
                           int allowed_changes);
void   SDL_PauseAudioDevice(int dev, int pause_on);

//-----------------------------------------------------------------------------
// Hooks the visionOS app layer uses to feed real values into the above.
//-----------------------------------------------------------------------------

void visionos_set_display_refresh_rate(int hz);

// Last message passed to SDL_ShowSimpleMessageBox, or NULL. Owned by the compat
// layer; valid until the next call.
const char *visionos_take_pending_alert(void);

#ifdef __cplusplus
}
#endif

#endif // MARIOVISION_SDL_COMPAT_H
