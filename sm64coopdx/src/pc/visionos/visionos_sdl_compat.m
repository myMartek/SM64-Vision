/*
 * Native implementations of the SDL2 surface declared in visionos_sdl_compat.h.
 *
 * See that header for the list of call sites each function serves.
 */

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include <mach/mach_time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "visionos_sdl_compat.h"

//-----------------------------------------------------------------------------
// Memory / paths
//-----------------------------------------------------------------------------

void SDL_free(void *mem) {
    free(mem);
}

char *SDL_GetPrefPath(const char *org, const char *app) {
    (void) org; // Apple platforms ignore the organisation, same as SDL does.

    @autoreleasepool {
        NSArray<NSString *> *dirs = NSSearchPathForDirectoriesInDomains(
            NSApplicationSupportDirectory, NSUserDomainMask, YES);
        if (dirs.count == 0) { return NULL; }

        NSString *base = dirs.firstObject;
        NSString *full = (app && *app) ? [base stringByAppendingPathComponent:@(app)] : base;

        NSError *error = nil;
        if (![NSFileManager.defaultManager createDirectoryAtPath:full
                                    withIntermediateDirectories:YES
                                                     attributes:nil
                                                          error:&error]) {
            NSLog(@"SDL_GetPrefPath: could not create %@: %@", full, error);
            return NULL;
        }

        // SDL contract: heap string with a trailing separator, freed by SDL_free.
        NSString *withSlash = [full stringByAppendingString:@"/"];
        const char *utf8 = withSlash.UTF8String;
        return utf8 ? strdup(utf8) : NULL;
    }
}

//-----------------------------------------------------------------------------
// Message box
//-----------------------------------------------------------------------------

static char *sPendingAlert = NULL;

int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, void *window) {
    (void) flags;
    (void) window;

    NSLog(@"[alert] %s: %s", title ? title : "", message ? message : "");

    free(sPendingAlert);
    sPendingAlert = NULL;

    if (message) {
        if (title && *title) {
            asprintf(&sPendingAlert, "%s\n\n%s", title, message);
        } else {
            sPendingAlert = strdup(message);
        }
    }
    return 0;
}

const char *visionos_take_pending_alert(void) {
    return sPendingAlert;
}

//-----------------------------------------------------------------------------
// Timing
//-----------------------------------------------------------------------------

static mach_timebase_info_data_t sTimebase;

static void ensure_timebase(void) {
    if (sTimebase.denom == 0) { mach_timebase_info(&sTimebase); }
}

Uint64 SDL_GetPerformanceFrequency(void) {
    return 1000000000ull; // we report in nanoseconds
}

Uint64 SDL_GetPerformanceCounter(void) {
    ensure_timebase();
    return mach_absolute_time() * sTimebase.numer / sTimebase.denom;
}

Uint32 SDL_GetTicks(void) {
    return (Uint32) (SDL_GetPerformanceCounter() / 1000000ull);
}

void SDL_Delay(Uint32 ms) {
    struct timespec ts = {
        .tv_sec  = ms / 1000,
        .tv_nsec = (long) (ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);
}

//-----------------------------------------------------------------------------
// Display
//-----------------------------------------------------------------------------

// Vision Pro renders at 90 Hz by default; the app layer overrides this with the
// rate the compositor actually reports.
static int sRefreshRate = 90;

void visionos_set_display_refresh_rate(int hz) {
    if (hz > 0) { sRefreshRate = hz; }
}

int SDL_GetCurrentDisplayMode(int displayIndex, SDL_DisplayMode *mode) {
    (void) displayIndex;
    if (!mode) { return -1; }

    // Width/height are the game's internal framebuffer size, not a physical
    // display - the compositor decides the real resolution. Only refresh_rate
    // is read by the caller (pc_main.c get_display_refresh_rate).
    mode->format = 0;
    mode->w = 0;
    mode->h = 0;
    mode->refresh_rate = sRefreshRate;
    mode->driverdata = NULL;
    return 0;
}

//-----------------------------------------------------------------------------
// Joysticks
//-----------------------------------------------------------------------------

int SDL_NumJoysticks(void) {
    @autoreleasepool {
        return (int) GCController.controllers.count;
    }
}

const char *SDL_JoystickNameForIndex(int deviceIndex) {
    @autoreleasepool {
        NSArray<GCController *> *controllers = GCController.controllers;
        if (deviceIndex < 0 || deviceIndex >= (int) controllers.count) { return NULL; }

        // Callers treat this as a borrowed string that stays valid while the
        // menu is drawn, so cache it rather than handing out an autoreleased
        // buffer.
        static char name[128];
        NSString *vendor = controllers[deviceIndex].vendorName ?: @"Controller";
        snprintf(name, sizeof(name), "%s", vendor.UTF8String);
        return name;
    }
}

//-----------------------------------------------------------------------------
// Keyboard names
//
// A Bluetooth keyboard can be paired with a Vision Pro, so key binds are not
// meaningless - but they are a corner case. This covers the keys someone would
// plausibly bind and returns "" for the rest, which is exactly what SDL does
// for unnamed scancodes and what controller_bind_mapping.c already handles.
//-----------------------------------------------------------------------------

static const char *scancode_name(SDL_Scancode sc) {
    static const char *letters[] = {
        "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
        "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    };
    static const char *digits[] = {
        "1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
    };

    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
        return letters[sc - SDL_SCANCODE_A];
    }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0) {
        return digits[sc - SDL_SCANCODE_1];
    }

    switch (sc) {
        case SDL_SCANCODE_RETURN:       return "Return";
        case SDL_SCANCODE_ESCAPE:       return "Escape";
        case SDL_SCANCODE_BACKSPACE:    return "Backspace";
        case SDL_SCANCODE_TAB:          return "Tab";
        case SDL_SCANCODE_SPACE:        return "Space";
        case SDL_SCANCODE_MINUS:        return "-";
        case SDL_SCANCODE_EQUALS:       return "=";
        case SDL_SCANCODE_LEFTBRACKET:  return "[";
        case SDL_SCANCODE_RIGHTBRACKET: return "]";
        case SDL_SCANCODE_BACKSLASH:    return "\\";
        case SDL_SCANCODE_SEMICOLON:    return ";";
        case SDL_SCANCODE_APOSTROPHE:   return "'";
        case SDL_SCANCODE_GRAVE:        return "`";
        case SDL_SCANCODE_COMMA:        return ",";
        case SDL_SCANCODE_PERIOD:       return ".";
        case SDL_SCANCODE_SLASH:        return "/";
        case SDL_SCANCODE_CAPSLOCK:     return "CapsLock";
        case SDL_SCANCODE_RIGHT:        return "Right";
        case SDL_SCANCODE_LEFT:         return "Left";
        case SDL_SCANCODE_DOWN:         return "Down";
        case SDL_SCANCODE_UP:           return "Up";
        case SDL_SCANCODE_LCTRL:        return "L Ctrl";
        case SDL_SCANCODE_LSHIFT:       return "L Shift";
        case SDL_SCANCODE_LALT:         return "L Alt";
        case SDL_SCANCODE_LGUI:         return "L GUI";
        case SDL_SCANCODE_RCTRL:        return "R Ctrl";
        case SDL_SCANCODE_RSHIFT:       return "R Shift";
        case SDL_SCANCODE_RALT:         return "R Alt";
        case SDL_SCANCODE_RGUI:         return "R GUI";
        case SDL_SCANCODE_INSERT:       return "Insert";
        case SDL_SCANCODE_HOME:         return "Home";
        case SDL_SCANCODE_END:          return "End";
        case SDL_SCANCODE_PAGEUP:       return "PageUp";
        case SDL_SCANCODE_PAGEDOWN:     return "PageDown";
        case SDL_SCANCODE_DELETE:       return "Delete";
        default:                        return "";
    }
}

const char *SDL_GetScancodeName(SDL_Scancode scancode) {
    if (scancode < 0 || scancode >= SDL_NUM_SCANCODES) { return ""; }
    return scancode_name(scancode);
}

SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode scancode) {
    // Signals "no distinct keycode"; controller_bind_mapping.c then sticks with
    // the scancode name, which is what we want for every key we do name.
    (void) scancode;
    return 0;
}

const char *SDL_GetKeyName(SDL_Keycode key) {
    (void) key;
    return "";
}

//-----------------------------------------------------------------------------
// Audio subsystem - deliberately inert, see the header.
//-----------------------------------------------------------------------------

Uint32 SDL_WasInit(Uint32 flags) {
    (void) flags;
    return 0;
}

int SDL_InitSubSystem(Uint32 flags) {
    (void) flags;
    return -1;
}

int SDL_OpenAudioDevice(const char *device, int iscapture,
                        const SDL_AudioSpec *desired, SDL_AudioSpec *obtained,
                        int allowed_changes) {
    (void) device; (void) iscapture; (void) desired; (void) obtained; (void) allowed_changes;
    return 0; // 0 means "failed to open" in SDL's device-id contract
}

void SDL_PauseAudioDevice(int dev, int pause_on) {
    (void) dev; (void) pause_on;
}
