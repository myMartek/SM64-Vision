/*
 * Gamepad backend for visionOS, implementing src/pc/controller/controller_api.h.
 *
 * visionOS supports Bluetooth gamepads through the GameController framework,
 * which is what makes Mario actually playable - gaze and pinch cannot stand in
 * for an analog stick.
 *
 * Button indices deliberately mirror SDL2's SDL_GameControllerButton numbering
 * so that coopdx's existing bind system, its saved configs and its "press a
 * button to bind" UI all keep working unchanged: a bind is stored as
 * VK_BASE_SDL_GAMEPAD + <SDL button index>, and this backend reports the same
 * numbers.
 */

#import <GameController/GameController.h>

#include <math.h>
#include <string.h>

#include "pc/controller/controller_api.h"
#include "pc/controller/controller_sdl.h" // VK_BASE_SDL_GAMEPAD, VK_LTRIGGER, VK_RTRIGGER
#include "pc/configfile.h"
#include "pc/djui/djui_interactable.h"
#include "mariovision_platform.h"
#include "mariovision_hands.h"

#include <ultra64.h>

// SDL2's SDL_GameControllerButton values, which the bind system is written
// against. Only the ones a GameController device can report are listed.
enum {
    SDL_BTN_A = 0,
    SDL_BTN_B = 1,
    SDL_BTN_X = 2,
    SDL_BTN_Y = 3,
    SDL_BTN_BACK = 4,
    SDL_BTN_GUIDE = 5,
    SDL_BTN_START = 6,
    SDL_BTN_LEFTSTICK = 7,
    SDL_BTN_RIGHTSTICK = 8,
    SDL_BTN_LEFTSHOULDER = 9,
    SDL_BTN_RIGHTSHOULDER = 10,
    SDL_BTN_DPAD_UP = 11,
    SDL_BTN_DPAD_DOWN = 12,
    SDL_BTN_DPAD_LEFT = 13,
    SDL_BTN_DPAD_RIGHT = 14,
};

#define MAX_BUTTONS 32
#define AXIS_THRESHOLD 0x4000
#define MAX_JOY_BINDS 64

static bool sButtons[MAX_BUTTONS];
static bool sPrevButtons[MAX_BUTTONS];
static uint32_t sLastPressed = VK_INVALID;

// [button index, N64 button mask], rebuilt from configfile by reconfig().
static uint32_t sJoyBinds[MAX_JOY_BINDS][2];
static uint32_t sNumJoyBinds = 0;

static GCController *sController;

//-----------------------------------------------------------------------------
// Binds
//-----------------------------------------------------------------------------

static void add_binds(uint32_t mask, const unsigned int *binds) {
    for (int i = 0; i < MAX_BINDS; i++) {
        if (binds[i] >= VK_BASE_SDL_GAMEPAD && binds[i] < VK_BASE_SDL_MOUSE) {
            if (sNumJoyBinds >= MAX_JOY_BINDS) { return; }
            sJoyBinds[sNumJoyBinds][0] = binds[i] - VK_BASE_SDL_GAMEPAD;
            sJoyBinds[sNumJoyBinds][1] = mask;
            sNumJoyBinds++;
        }
    }
}

static void controller_visionos_reconfig(void) {
    memset(sJoyBinds, 0, sizeof(sJoyBinds));
    sNumJoyBinds = 0;

    // Same table as controller_sdl_reconfig in controller_sdl.c.
    add_binds(A_BUTTON,     configKeyA);
    add_binds(B_BUTTON,     configKeyB);
    add_binds(X_BUTTON,     configKeyX);
    add_binds(Y_BUTTON,     configKeyY);
    add_binds(Z_TRIG,       configKeyZ);
    add_binds(STICK_UP,     configKeyStickUp);
    add_binds(STICK_LEFT,   configKeyStickLeft);
    add_binds(STICK_DOWN,   configKeyStickDown);
    add_binds(STICK_RIGHT,  configKeyStickRight);
    add_binds(U_CBUTTONS,   configKeyCUp);
    add_binds(L_CBUTTONS,   configKeyCLeft);
    add_binds(D_CBUTTONS,   configKeyCDown);
    add_binds(R_CBUTTONS,   configKeyCRight);
    add_binds(L_TRIG,       configKeyL);
    add_binds(R_TRIG,       configKeyR);
    add_binds(START_BUTTON, configKeyStart);
    add_binds(U_JPAD,       configKeyDUp);
    add_binds(D_JPAD,       configKeyDDown);
    add_binds(L_JPAD,       configKeyDLeft);
    add_binds(R_JPAD,       configKeyDRight);
}

//-----------------------------------------------------------------------------
// Device
//-----------------------------------------------------------------------------

static void pick_controller(void) {
    NSArray<GCController *> *controllers = GCController.controllers;
    if (controllers.count == 0) {
        sController = nil;
        return;
    }

    // Prefer the controller the system considers current - the one that last
    // received input. KeynoteEnvViewer reads it this way, and it is what makes
    // the right pad win when the simulator's virtual controller or a second
    // device is also in the list.
    GCController *current = GCController.current;
    if (current != nil && current.extendedGamepad != nil) {
        sController = current;
        return;
    }

    // configGamepadNumber selects among several pads, matching the desktop
    // backend; out of range falls back to the first one.
    NSUInteger index = (NSUInteger) configGamepadNumber;
    if (index >= controllers.count) { index = 0; }
    sController = controllers[index];
}

/// Reports what the GameController framework is actually handing us. A pad that
/// the system keeps for its own UI navigation looks identical to a pad that is
/// not connected, so this is worth having in the log.
static void log_input_devices(void) {
    NSArray<GCController *> *controllers = GCController.controllers;
    NSLog(@"[controller] %lu controller(s), keyboard: %@",
          (unsigned long) controllers.count,
          GCKeyboard.coalescedKeyboard ? @"yes" : @"no");

    for (GCController *c in controllers) {
        NSLog(@"[controller]   \"%@\" category=%@ extendedGamepad=%@",
              c.vendorName ?: @"?", c.productCategory ?: @"?",
              c.extendedGamepad ? @"yes" : @"NO");
    }
}

static void controller_visionos_init(void) {
    [GCController startWirelessControllerDiscoveryWithCompletionHandler:^{}];

    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    [center addObserverForName:GCControllerDidConnectNotification
                        object:nil
                         queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification *note) {
        pick_controller();
        log_input_devices();
    }];
    [center addObserverForName:GCControllerDidDisconnectNotification
                        object:nil
                         queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification *note) {
        pick_controller();
        log_input_devices();
    }];

    pick_controller();
    log_input_devices();
    controller_visionos_reconfig();
}

//-----------------------------------------------------------------------------
// Keyboard
//
// Basic control for a paired Bluetooth keyboard, and for the simulator, which
// forwards the Mac keyboard as a GCKeyboard:
//
//   W A S D   walk
//   Space     jump (A)
//   arrows    camera (C buttons)
//
// Deliberately just this much - the rest of the N64 pad stays on the gamepad.
//-----------------------------------------------------------------------------

static bool key_pressed(GCKeyboardInput *kb, GCKeyCode code) {
    GCControllerButtonInput *button = [kb buttonForKeyCode:code];
    return button != nil && button.pressed;
}

/// Adds keyboard state to `pad`. The walk keys override the stick axes rather
/// than being added to them, so a resting gamepad does not cancel them out.
static void apply_keyboard(OSContPad *pad, int16_t *leftx, int16_t *lefty) {
    GCKeyboardInput *kb = GCKeyboard.coalescedKeyboard.keyboardInput;
    if (kb == nil) { return; }

    // Axis space is SDL's, where +y points down - the same convention
    // update_analog_stick and the gamepad path use.
    int16_t x = 0, y = 0;
    if (key_pressed(kb, GCKeyCodeKeyA)) { x -= 0x7FFF; }
    if (key_pressed(kb, GCKeyCodeKeyD)) { x += 0x7FFF; }
    if (key_pressed(kb, GCKeyCodeKeyW)) { y -= 0x7FFF; }
    if (key_pressed(kb, GCKeyCodeKeyS)) { y += 0x7FFF; }

    if (x != 0 || y != 0) {
        *leftx = x;
        *lefty = y;
    }

    // One-shot confirmation that key events actually reach the app - a keyboard
    // the system does not hand over looks exactly like one nobody is typing on.
    static bool sReportedInput = false;
    if (!sReportedInput && (x != 0 || y != 0 || key_pressed(kb, GCKeyCodeSpacebar))) {
        sReportedInput = true;
        NSLog(@"[controller] keyboard input received");
    }

    if (key_pressed(kb, GCKeyCodeSpacebar)) { pad->button |= A_BUTTON; }

    // R raises the menu request here as well as in djui_panel_pause_create.
    // Routing it only through there was wrong: with djui switched off - which
    // is the whole point of the native interface - that function is never
    // reached, and the key stopped working. Two callers are harmless, the
    // request is a flag that is taken once.
    {
        static bool sMenuHeld = false;
        const bool down = key_pressed(kb, GCKeyCodeKeyR);
        if (down && !sMenuHeld && mariovision_menu_key_allowed()) { mariovision_request_menu(); }
        sMenuHeld = down;
    }

    if (key_pressed(kb, GCKeyCodeLeftArrow))  { pad->button |= L_CBUTTONS; }
    if (key_pressed(kb, GCKeyCodeRightArrow)) { pad->button |= R_CBUTTONS; }
    if (key_pressed(kb, GCKeyCodeUpArrow))    { pad->button |= U_CBUTTONS; }
    if (key_pressed(kb, GCKeyCodeDownArrow))  { pad->button |= D_CBUTTONS; }
}

//-----------------------------------------------------------------------------
// Reading
//-----------------------------------------------------------------------------

static void update_button(int index, bool pressed) {
    if (index < 0 || index >= MAX_BUTTONS) { return; }
    sButtons[index] = pressed;

    const bool changed = pressed != sPrevButtons[index];
    if (!changed) { return; }
    sPrevButtons[index] = pressed;

    if (pressed) {
        sLastPressed = (uint32_t) index;
        djui_interactable_on_key_down(VK_BASE_SDL_GAMEPAD + index);
    } else {
        djui_interactable_on_key_up(VK_BASE_SDL_GAMEPAD + index);
    }
}

/// Same deadzone and rescaling as update_analog_stick in controller_sdl.c, so
/// the feel matches the desktop build and configStickDeadzone keeps its meaning.
static void update_analog_stick(s8 *stick_x, s8 *stick_y, int16_t input_x, int16_t input_y) {
    float magnitude_sq = (float) (input_x * input_x) + (float) (input_y * input_y);
    float deadzone = configStickDeadzone * DEADZONE_STEP;

    if (magnitude_sq > (deadzone * deadzone)) {
        float magnitude = sqrtf(magnitude_sq);
        float dir_x = (float) input_x / magnitude;
        float dir_y = (float) input_y / magnitude;
        float scale = 1.f / fmaxf(fabsf(dir_x), fabsf(dir_y));
        float max_magnitude = 0x8000 * scale;

        magnitude -= deadzone;
        magnitude *= max_magnitude / (max_magnitude - deadzone);
        magnitude /= 0x100;
        magnitude = fminf(magnitude, scale * 127.f);

        *stick_x = dir_x * magnitude;
        *stick_y = -dir_y * magnitude;
    }
}

static int16_t to_axis(float value) {
    // GameController reports -1..1; the bind and deadzone code is written
    // against SDL's -32768..32767.
    float scaled = value * 32767.0f;
    if (scaled > 32767.0f)  { scaled = 32767.0f; }
    if (scaled < -32768.0f) { scaled = -32768.0f; }
    return (int16_t) scaled;
}

static s16 invert_s16(s16 value) {
    return (value == -0x8000) ? 0x7FFF : -value;
}

static void apply_stick_options(int16_t *leftx, int16_t *lefty, int16_t *rightx, int16_t *righty) {
    if (configStick.rotateLeft) {
        s16 tmp = *leftx;
        *leftx = invert_s16(*lefty);
        *lefty = tmp;
    }
    if (configStick.rotateRight) {
        s16 tmp = *rightx;
        *rightx = invert_s16(*righty);
        *righty = tmp;
    }
    if (configStick.invertLeftX)  { *leftx = invert_s16(*leftx); }
    if (configStick.invertLeftY)  { *lefty = invert_s16(*lefty); }
    if (configStick.invertRightX) { *rightx = invert_s16(*rightx); }
    if (configStick.invertRightY) { *righty = invert_s16(*righty); }
}

// Set when the N64 movement stick is the direction pad itself, so that pad is
// not also treated as the camera. Compared, never messaged.
static __unsafe_unretained GCController *sN64DpadController = nil;
static bool sN64DpadIsStick = false;

// The Switch Online N64 pad has no extendedGamepad, and its one analog stick
// is not always the "Left Thumbstick" key. C buttons are the other cluster
// (right stick / SWITCH_N64_C_BUTTONS) and must not be read as walk.
static bool string_has(NSString *s, NSString *part) {
    if (s.length == 0 || part.length == 0) { return false; }
    return [s rangeOfString:part options:NSCaseInsensitiveSearch].location != NSNotFound;
}

static bool name_is_movement_stick(NSString *name) {
    if (name.length == 0) { return false; }
    if (string_has(name, @"right")) { return false; }
    if ([name.lowercaseString hasPrefix:@"r."]) { return false; }
    if (string_has(name, @"c_button") || string_has(name, @"c-button") || string_has(name, @"c button")) {
        return false;
    }
    if (string_has(name, @"c-kn") || string_has(name, @"direction") || string_has(name, @"d-pad")
        || string_has(name, @"dpad")) {
        return false;
    }
    if (string_has(name, @"button")) { return false; }
    if (string_has(name, @"thumbstick") || string_has(name, @"steuer") || string_has(name, @"joystick")) {
        return true;
    }
    return string_has(name, @"stick");
}

static bool element_is_movement_stick(NSString *key, NSString *localized, NSString *symbol) {
    return name_is_movement_stick(key) || name_is_movement_stick(localized) || name_is_movement_stick(symbol);
}

static bool source_is_dpad(NSString *source) {
    return [source isEqualToString:@"Direction Pad"]
        || [source isEqualToString:@"Cardinal Direction Pad"]
        || [source isEqualToString:@"GCInputDirectionPad"]
        || [source isEqualToString:@"micro.dpad"];
}

static bool controller_looks_like_n64(GCController *controller) {
    if (controller == nil) { return false; }
    NSString *blob = [NSString stringWithFormat:@"%@ %@", controller.vendorName ?: @"", controller.productCategory ?: @""];
    if (string_has(blob, @"N64") || string_has(blob, @"Nintendo 64") || string_has(blob, @"64 Controller")) {
        return true;
    }
    GCPhysicalInputProfile *profile = controller.physicalInputProfile;
    for (NSString *key in profile.elements) {
        GCControllerElement *element = profile.elements[key];
        if (string_has(key, @"N64") || string_has(element.sfSymbolsName, @"N64")
            || string_has(element.localizedName, @"Steuerstick")
            || string_has(element.localizedName, @"Control Stick")) {
            return true;
        }
    }
    for (id<GCPhysicalInputElement> element in controller.input.elements) {
        if (string_has(element.sfSymbolsName, @"N64") || string_has(element.localizedName, @"Steuerstick")
            || string_has(element.localizedName, @"Control Stick")) {
            return true;
        }
        for (NSString *alias in element.aliases) {
            if (string_has(alias, @"N64")) { return true; }
        }
    }
    return false;
}

static bool stick_moved(int16_t x, int16_t y) {
    const int64_t deadzone = (int64_t) configStickDeadzone * (int64_t) DEADZONE_STEP;
    const int64_t mag2 = (int64_t) x * (int64_t) x + (int64_t) y * (int64_t) y;
    return mag2 > deadzone * deadzone;
}

static void legacy_dpad_sdl(GCControllerDirectionPad *pad, int16_t *x, int16_t *y) {
    *x = to_axis(pad.xAxis.value);
    *y = to_axis(-pad.yAxis.value); // GC +y is up, SDL +y is down
}

static void consider_stick(int16_t x, int16_t y, NSString *source,
                           int *bestMag, int16_t *bestX, int16_t *bestY, NSString **bestSource) {
    const int mag = abs(x) + abs(y);
    if (mag > *bestMag) {
        *bestMag = mag;
        *bestX = x;
        *bestY = y;
        *bestSource = source;
    }
}

static bool axis_pair_base(NSString *key, NSString **base) {
    NSString *suffix = @" X Axis";
    if (![key hasSuffix:suffix]) { return false; }
    *base = [key substringToIndex:key.length - suffix.length];
    return true;
}

/// Movement stick in SDL axis space. Named stick elements win. An analog
/// direction pad or a bare X/Y pair is used when no named stick exists, and
/// also on an N64 pad whose leftThumbstick is present but never moves.
/// Digital pads are not used, so a real D-pad still clicks rather than walking.
static bool read_movement_stick(GCController *controller, bool allowFallback,
                                int16_t *outX, int16_t *outY, NSString **outSource) {
    *outX = 0;
    *outY = 0;
    if (outSource != NULL) { *outSource = @"none"; }

    int namedMag = -1, analogMag = -1;
    int16_t namedX = 0, namedY = 0, analogX = 0, analogY = 0;
    NSString *namedSource = nil, *analogSource = nil;
    bool foundNamed = false;

    GCExtendedGamepad *extended = controller.extendedGamepad;
    if (extended != nil) {
        foundNamed = true;
        int16_t x, y;
        legacy_dpad_sdl(extended.leftThumbstick, &x, &y);
        consider_stick(x, y, @"extendedGamepad.leftThumbstick", &namedMag, &namedX, &namedY, &namedSource);
    }

    GCPhysicalInputProfile *profile = controller.physicalInputProfile;
    if (profile != nil) {
        for (NSString *key in [profile.dpads.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
            GCControllerDirectionPad *pad = profile.dpads[key];
            if (!element_is_movement_stick(key, pad.localizedName, pad.sfSymbolsName)) { continue; }
            foundNamed = true;
            int16_t x, y;
            legacy_dpad_sdl(pad, &x, &y);
            consider_stick(x, y, key, &namedMag, &namedX, &namedY, &namedSource);
        }
        for (NSString *key in [profile.elements.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
            NSString *base = nil;
            if (!axis_pair_base(key, &base)) { continue; }
            GCControllerElement *element = profile.elements[key];
            if (![element isKindOfClass:[GCControllerAxisInput class]]) { continue; }
            if (!element_is_movement_stick(base, element.localizedName, element.sfSymbolsName)
                && !name_is_movement_stick(key)) {
                continue;
            }
            GCControllerElement *yElement = profile.elements[[base stringByAppendingString:@" Y Axis"]];
            if (![yElement isKindOfClass:[GCControllerAxisInput class]]) { continue; }
            foundNamed = true;
            const int16_t x = to_axis(((GCControllerAxisInput *) element).value);
            const int16_t y = to_axis(-((GCControllerAxisInput *) yElement).value);
            consider_stick(x, y, key, &namedMag, &namedX, &namedY, &namedSource);
        }
    }

    id<GCDevicePhysicalInput> input = controller.input;
    if (input != nil) {
        for (id<GCPhysicalInputElement> element in input.elements) {
            bool match = element_is_movement_stick(nil, element.localizedName, element.sfSymbolsName);
            NSString *alias = nil;
            for (NSString *name in element.aliases) {
                if (alias == nil) { alias = name; }
                if (name_is_movement_stick(name)) { match = true; alias = name; }
            }
            if (!match) { continue; }
            int16_t x = 0, y = 0;
            bool got = false;
            if ([element conformsToProtocol:@protocol(GCDirectionPadElement)]) {
                const GCPoint2 v = ((id<GCDirectionPadElement>) element).xyAxes.value;
                x = to_axis(v.x);
                y = to_axis(-v.y);
                got = true;
            } else if ([element conformsToProtocol:@protocol(GCAxis2DInput)]) {
                const GCPoint2 v = ((id<GCAxis2DInput>) element).value;
                x = to_axis(v.x);
                y = to_axis(-v.y);
                got = true;
            }
            if (!got) { continue; }
            foundNamed = true;
            consider_stick(x, y, alias ?: element.localizedName ?: @"modern stick",
                           &namedMag, &namedX, &namedY, &namedSource);
        }
    }

    // allowFallback is the N64 pad, whose stick may sit behind a stub
    // leftThumbstick. Every other pad only falls through when it has no named
    // stick, so a Joy-Con's thumbstick is never replaced by its D-pad.
    if (allowFallback || !foundNamed) {
        if (controller.microGamepad != nil && controller.microGamepad.allowsRotation) {
            controller.microGamepad.allowsRotation = NO;
        }
        GCControllerDirectionPad *pads[3] = { nil, nil, nil };
        NSString *padNames[3] = { @"Direction Pad", @"Cardinal Direction Pad", @"micro.dpad" };
        if (profile != nil) {
            pads[0] = profile.dpads[@"Direction Pad"];
            pads[1] = profile.dpads[@"Cardinal Direction Pad"];
        }
        if (controller.microGamepad != nil) { pads[2] = controller.microGamepad.dpad; }
        for (int i = 0; i < 3; i++) {
            GCControllerDirectionPad *pad = pads[i];
            if (pad == nil || !pad.xAxis.analog) { continue; }
            int16_t x, y;
            legacy_dpad_sdl(pad, &x, &y);
            consider_stick(x, y, padNames[i], &analogMag, &analogX, &analogY, &analogSource);
        }
        if (profile != nil) {
            GCControllerAxisInput *xa = profile.axes[@"X Axis"];
            GCControllerAxisInput *ya = profile.axes[@"Y Axis"];
            if (xa != nil && ya != nil) {
                consider_stick(to_axis(xa.value), to_axis(-ya.value), @"X Axis/Y Axis",
                               &analogMag, &analogX, &analogY, &analogSource);
            }
        }
        if (input != nil) {
            GCControllerDirectionPad *legacyDpad = profile != nil ? profile.dpads[@"Direction Pad"] : nil;
            const bool legacyDigital = legacyDpad != nil && !legacyDpad.xAxis.analog;
            id direction = input[GCInputDirectionPad];
            if (!legacyDigital && [direction conformsToProtocol:@protocol(GCDirectionPadElement)]) {
                const GCPoint2 v = ((id<GCDirectionPadElement>) direction).xyAxes.value;
                consider_stick(to_axis(v.x), to_axis(-v.y), @"GCInputDirectionPad",
                               &analogMag, &analogX, &analogY, &analogSource);
            }
        }
    }

    const bool namedMoving = foundNamed && stick_moved(namedX, namedY);
    const bool analogMoving = analogSource != nil && stick_moved(analogX, analogY);
    const bool preferAnalog = analogMoving && (!namedMoving || analogMag > namedMag);
    if (preferAnalog) {
        *outX = analogX;
        *outY = analogY;
        if (outSource != NULL) { *outSource = analogSource; }
        return true;
    }
    if (foundNamed) {
        *outX = namedX;
        *outY = namedY;
        if (outSource != NULL) { *outSource = namedSource ?: @"stick"; }
        return true;
    }
    return false;
}

/// One line, the first time this pad's stick is accepted. Enough to see which
/// element drove Mario; the per-element inventory stays off.
static void n64_note_once(GCController *controller, NSString *source) {
    static NSMutableSet<NSString *> *seen = nil;
    if (seen == nil) { seen = [NSMutableSet set]; }
    NSString *key = [NSString stringWithFormat:@"%p", controller];
    if ([seen containsObject:key]) { return; }
    [seen addObject:key];
    NSLog(@"[N64Stick] name=\"%@\" category=\"%@\" chosen=%@",
          controller.vendorName ?: @"?", controller.productCategory ?: @"?", source ?: @"?");
}

/// Read whatever has no extended gamepad, by the names it answers to.
///
/// The names are not guessed. They were read off the device by the capture
/// screen, which is what that screen is for: a Nintendo 64 controller lists
/// "Button A", "Left Shoulder", "Direction Pad" and the rest, and prints its
/// own labels in the element names - Z-Knopf on the left trigger, C-Knoepfe on
/// the right thumbstick, Steuerstick on the left.
static int16_t sProfileLeftX = 0, sProfileLeftY = 0;
static int16_t sProfileRightX = 0, sProfileRightY = 0;

/// Read whatever has no extended gamepad, by the names it answers to.
///
/// The names are not guessed. The capture screen read them off the device: a
/// Nintendo 64 controller lists "Button A", "Left Shoulder", "Direction Pad"
/// and the rest, and prints its own labels in the element names - Z-Knopf on
/// the left trigger, C-Knoepfe on the right thumbstick, Steuerstick on the left.
///
/// Straight into pad->button, and not through update_button.
///
/// That was the first attempt and it looked like the buttons were not being
/// read at all: this runs first, and the extended-gamepad path that follows
/// writes every button again from the DualSense, which is holding none of them
/// down. Setting a button and then being told it is up is indistinguishable
/// from never having set it. The C buttons survived only because they leave by
/// another door - the stick, which is merged at the end.
///
/// The bindings here are fixed rather than configurable, and deliberately so.
/// This is support for one named controller, and its buttons have the game's
/// own names printed on them.
static void read_profile_pads(OSContPad *pad) {
    sProfileLeftX = 0;
    sProfileLeftY = 0;
    sProfileRightX = 0;
    sProfileRightY = 0;
    sN64DpadIsStick = false;
    sN64DpadController = nil;

    for (GCController *each in GCController.controllers) {
        const bool n64 = controller_looks_like_n64(each);
        const bool extended = each.extendedGamepad != nil;
        const bool spatial = [each.productCategory isEqualToString:GCProductCategorySpatialController];

        int16_t lx = 0, ly = 0;
        NSString *source = nil;
        bool foundStick = false;
        // Sense controllers are read in read_spatial. Scanning them here would
        // let a resting stick overwrite another pad.
        if (!spatial && (n64 || !extended)) {
            foundStick = read_movement_stick(each, n64, &lx, &ly, &source);
            const bool fallbackSource = source_is_dpad(source)
                || [source isEqualToString:@"X Axis/Y Axis"];
            // N64, and a pad whose only axis is an analog direction pad, use
            // the config deadzone so a partial push still walks. A named stick
            // on any other pad keeps the half-scale gate.
            const bool use = (n64 || fallbackSource) ? stick_moved(lx, ly)
                : (abs(lx) > AXIS_THRESHOLD || abs(ly) > AXIS_THRESHOLD);
            if (foundStick && use) {
                sProfileLeftX = lx;
                sProfileLeftY = ly;
                if (source_is_dpad(source)) {
                    sN64DpadIsStick = true;
                    sN64DpadController = each;
                }
                if (n64 || fallbackSource) { n64_note_once(each, source); }
            }
        }

        if (extended) { continue; }
        GCPhysicalInputProfile *profile = each.physicalInputProfile;
        if (profile == nil) { continue; }

        #define PBTN(name) (profile.buttons[@name] != nil && profile.buttons[@name].pressed)

        if (PBTN("Button A")) { pad->button |= A_BUTTON; }
        if (PBTN("Button B")) { pad->button |= B_BUTTON; }
        if (PBTN("Left Trigger")) { pad->button |= Z_TRIG; }   // the Z-Knopf
        if (PBTN("Left Shoulder")) { pad->button |= L_TRIG; }
        if (PBTN("Right Shoulder")) { pad->button |= R_TRIG; }
        if (PBTN("Button Menu")) { pad->button |= START_BUTTON; }

        GCControllerDirectionPad *dpad = profile.dpads[@"Direction Pad"];
        if (dpad != nil && !(sN64DpadIsStick && sN64DpadController == each)) {
            if (dpad.up.pressed) { pad->button |= U_JPAD; }
            if (dpad.down.pressed) { pad->button |= D_JPAD; }
            if (dpad.left.pressed) { pad->button |= L_JPAD; }
            if (dpad.right.pressed) { pad->button |= R_JPAD; }
        }

        // Both sticks are handed on rather than written here, and by exactly
        // the same route the extended pads use. Writing pad->stick_x directly
        // was the other half of the trouble: the caller has its own conversion,
        // and going round it gave a stick that only leaned left and back.
        GCControllerDirectionPad *lstick = profile.dpads[@"Left Thumbstick"];
        GCControllerDirectionPad *rstick = profile.dpads[@"Right Thumbstick"];
        if (!foundStick && lstick != nil) {
            const int16_t x = to_axis(lstick.xAxis.value);
            const int16_t y = to_axis(-lstick.yAxis.value);  // GC is y-up, SDL y-down
            if (abs(x) > AXIS_THRESHOLD || abs(y) > AXIS_THRESHOLD) {
                sProfileLeftX = x;
                sProfileLeftY = y;
            }
        }
        // The C buttons arrive as the right stick, which is already the camera
        // here - which is what they were for.
        if (rstick != nil) {
            const int16_t x = to_axis(rstick.xAxis.value);
            const int16_t y = to_axis(-rstick.yAxis.value);
            if (abs(x) > AXIS_THRESHOLD || abs(y) > AXIS_THRESHOLD) {
                sProfileRightX = x;
                sProfileRightY = y;
            }
        }

        #undef PBTN
    }
}

static void read_gamepad(OSContPad *pad, int16_t *outLeftX, int16_t *outLeftY,
                         int16_t *outRightX, int16_t *outRightY) {
    if (configDisableGamepads) { return; }

    if (sController == nil || ![GCController.controllers containsObject:sController]) {
        pick_controller();
    }

    // Pads without an extended gamepad, before the one with it.
    //
    // The Nintendo 64 controller offers none, and everything below asks for one
    // and gives up when it is missing - which is why that pad did nothing at
    // all while the capture screen saw every button it has. That screen reads
    // the physical input profile, which every controller has.
    //
    // Added beside the existing path rather than replacing it. Replacing it was
    // tried and took the DualSense down with it: at the end of a long evening,
    // a rewrite of the one input path that works is a poor trade for a pad that
    // does not.
    read_profile_pads(pad);

    GCExtendedGamepad *gamepad = sController.extendedGamepad;
    if (gamepad == nil) {
        // The N64 pad lands here. Its buttons were applied above; its stick
        // was left in sProfile* and has to be published, or Mario never moves.
        int16_t lx = sProfileLeftX, ly = sProfileLeftY, rx = sProfileRightX, ry = sProfileRightY;
        apply_stick_options(&lx, &ly, &rx, &ry);
        *outLeftX = lx;
        *outLeftY = ly;
        *outRightX = rx;
        *outRightY = ry;
        return;
    }

    update_button(SDL_BTN_A, gamepad.buttonA.pressed);
    update_button(SDL_BTN_B, gamepad.buttonB.pressed);
    update_button(SDL_BTN_X, gamepad.buttonX.pressed);
    update_button(SDL_BTN_Y, gamepad.buttonY.pressed);
    update_button(SDL_BTN_LEFTSHOULDER, gamepad.leftShoulder.pressed);
    update_button(SDL_BTN_RIGHTSHOULDER, gamepad.rightShoulder.pressed);

    // The right shoulder is what coopdx's prompt calls "R", and it is the
    // button the player actually reaches for - the keyboard hook alone left
    // anyone on a pad with no way back to the menu, and so no way out of a mode
    // once the launcher had closed. Edge-triggered, like the key.
    {
        static bool sMenuHeld = false;
        const bool down = gamepad.rightShoulder.pressed;
        if (down && !sMenuHeld && mariovision_menu_key_allowed()) { mariovision_request_menu(); }
        sMenuHeld = down;
    }
    // When the N64 stick is this direction pad, a push must walk rather than
    // also fire the d-pad binds. Every other pad keeps the mapping below.
    const bool dpadIsStick = sN64DpadIsStick && sController == sN64DpadController;
    update_button(SDL_BTN_DPAD_UP, !dpadIsStick && gamepad.dpad.up.pressed);
    update_button(SDL_BTN_DPAD_DOWN, !dpadIsStick && gamepad.dpad.down.pressed);
    update_button(SDL_BTN_DPAD_LEFT, !dpadIsStick && gamepad.dpad.left.pressed);
    update_button(SDL_BTN_DPAD_RIGHT, !dpadIsStick && gamepad.dpad.right.pressed);
    update_button(SDL_BTN_START, gamepad.buttonMenu.pressed);
    update_button(SDL_BTN_BACK, gamepad.buttonOptions.pressed);
    update_button(SDL_BTN_GUIDE, gamepad.buttonHome.pressed);
    update_button(SDL_BTN_LEFTSTICK, gamepad.leftThumbstickButton.pressed);
    update_button(SDL_BTN_RIGHTSTICK, gamepad.rightThumbstickButton.pressed);

    int16_t leftx  = to_axis(gamepad.leftThumbstick.xAxis.value);
    int16_t lefty  = to_axis(-gamepad.leftThumbstick.yAxis.value);  // GC is y-up, SDL y-down
    int16_t rightx = to_axis(gamepad.rightThumbstick.xAxis.value);
    int16_t righty = to_axis(-gamepad.rightThumbstick.yAxis.value);
    int16_t ltrig  = to_axis(gamepad.leftTrigger.value);
    int16_t rtrig  = to_axis(gamepad.rightTrigger.value);

    apply_stick_options(&leftx, &lefty, &rightx, &righty);

    update_button(VK_LTRIGGER - VK_BASE_SDL_GAMEPAD, ltrig > AXIS_THRESHOLD);
    update_button(VK_RTRIGGER - VK_BASE_SDL_GAMEPAD, rtrig > AXIS_THRESHOLD);

    // The Nintendo 64 pad, which needs one line and no more.
    //
    // Measured on the device rather than guessed: it vends itself under the
    // ordinary names - Button A, Left Shoulder, Direction Pad - so everything
    // above already lands where it should. Its own labels come through in the
    // element names, which is how one knows what each stands for:
    //
    //   Z-Knopf          -> Left Trigger
    //   L-Taste, R-Taste -> Left Shoulder, Right Shoulder
    //   C-Knoepfe        -> Right Thumbstick (SWITCH_N64_C_BUTTONS)
    //   Steuerstick      -> Left Thumbstick  (SWITCH_N64_THUMBSTICK)
    //   Startknopf       -> Button Menu
    //
    // The C buttons arriving as the right stick is a gift: that is already the
    // camera here, which is what they were for.
    //
    // Only Z has to be said out loud. On this pad it is a plain button with a Z
    // printed on it, not an analogue trigger, and leaving it to whatever the
    // config happens to bind the left trigger to would be leaving the one
    // button the controller is named for to chance.
    if ([sController.productCategory containsString:@"N64"]) {
        if (gamepad.leftTrigger.pressed) { pad->button |= Z_TRIG; }
    }

    uint32_t buttons_down = 0;
    for (uint32_t i = 0; i < sNumJoyBinds; i++) {
        if (sButtons[sJoyBinds[i][0]]) { buttons_down |= sJoyBinds[i][1]; }
    }
    pad->button |= buttons_down;

    // Same one-shot confirmation as for the keyboard: says whether the system
    // is handing the pad's input to the app at all, rather than keeping it for
    // its own focus navigation.
    static bool sReportedInput = false;
    if (!sReportedInput && (buttons_down != 0 || abs(leftx) > AXIS_THRESHOLD || abs(lefty) > AXIS_THRESHOLD)) {
        sReportedInput = true;
        NSLog(@"[controller] gamepad input received (buttons=0x%x stick=%d,%d)",
              buttons_down, leftx, lefty);
    }

    const uint32_t xstick = buttons_down & STICK_XMASK;
    const uint32_t ystick = buttons_down & STICK_YMASK;
    if (xstick == STICK_LEFT)       { pad->stick_x = -128; }
    else if (xstick == STICK_RIGHT) { pad->stick_x = 127; }
    if (ystick == STICK_DOWN)       { pad->stick_y = -128; }
    else if (ystick == STICK_UP)    { pad->stick_y = 127; }

    // Fixed extra mappings on top of the configurable binds: Square doubles as
    // B alongside Circle, and both left shoulder inputs reach Z - crouching
    // should not depend on remembering which of L1/L2 the config maps.
    if (gamepad.buttonX.pressed) { pad->button |= B_BUTTON; }
    if (gamepad.leftShoulder.pressed || gamepad.leftTrigger.value > 0.5f) {
        pad->button |= Z_TRIG;
    }

    // A pad read through its profile has no place of its own to put a stick,
    // so it leaves it here, and takes precedence only while it is being moved.
    // Options were already applied to the extended axes; apply them to the
    // profile axes too, or an inverted N64 stick would ignore the setting.
    const bool profileLeft = (sProfileLeftX != 0 || sProfileLeftY != 0);
    const bool profileRight = (sProfileRightX != 0 || sProfileRightY != 0);
    int16_t plx = sProfileLeftX, ply = sProfileLeftY, prx = sProfileRightX, pry = sProfileRightY;
    if (profileLeft || profileRight) { apply_stick_options(&plx, &ply, &prx, &pry); }
    *outLeftX = profileLeft ? plx : leftx;
    *outLeftY = profileLeft ? ply : lefty;
    *outRightX = profileRight ? prx : rightx;
    *outRightY = profileRight ? pry : righty;
}

//-----------------------------------------------------------------------------
// Tracked controllers
//
// Declaring the spatial profile changed how the system presents a Sense pair:
// instead of one coalesced gamepad it hands over two separate controllers, and
// a single one of those cannot fill an extended gamepad profile. read_gamepad
// therefore finds nothing and walking stops - which is exactly what happened
// the first time this was tried on the headset.
//
// The element API answers regardless of profile, so the sticks and buttons are
// read from there instead. Left stick walks, right stick turns, matching what
// the pair already did as one gamepad.
//-----------------------------------------------------------------------------

/// A button's value.
///
/// The element is not the input: GameController's element protocols hand out
/// the readable part separately, so a button answers through pressedInput and a
/// thumbstick through xyAxes. Testing the element itself for conformance was
/// the first attempt and it silently found nothing at all - which on the
/// headset looked exactly like sticks that did not work.
static float element_value(id<GCDevicePhysicalInput> input, NSString *name) {
    id element = input[name];
    if ([element conformsToProtocol:@protocol(GCButtonElement)]) {
        return ((id<GCButtonElement>) element).pressedInput.value;
    }
    if ([element conformsToProtocol:@protocol(GCLinearInput)]) {
        return ((id<GCLinearInput>) element).value;
    }
    return 0.0f;
}

static bool element_pressed(id<GCDevicePhysicalInput> input, NSString *name) {
    id element = input[name];
    if ([element conformsToProtocol:@protocol(GCButtonElement)]) {
        return ((id<GCButtonElement>) element).pressedInput.isPressed;
    }
    if ([element conformsToProtocol:@protocol(GCPressedStateInput)]) {
        return ((id<GCPressedStateInput>) element).pressed;
    }
    return element_value(input, name) > 0.5f;
}

/// The first of several candidate keys that answers.
///
/// The halves do not agree with the header on what things are called: the stick
/// is keyed generically while the shoulders are not, and which spelling a given
/// button uses is not something to be derived. Asking in order costs nothing
/// and cannot be wrong the way a guess can.
static bool element_any_pressed(id<GCDevicePhysicalInput> input, NSArray<NSString *> *names) {
    for (NSString *n in names) {
        if (input[n] != nil) { return element_pressed(input, n); }
    }
    return false;
}

static float element_any_value(id<GCDevicePhysicalInput> input, NSArray<NSString *> *names) {
    for (NSString *n in names) {
        if (input[n] != nil) { return element_value(input, n); }
    }
    return 0.0f;
}

static bool element_stick(id<GCDevicePhysicalInput> input, NSString *name,
                          int16_t *outX, int16_t *outY) {
    id element = input[name];
    id<GCAxis2DInput> axes = nil;
    if ([element conformsToProtocol:@protocol(GCDirectionPadElement)]) {
        axes = ((id<GCDirectionPadElement>) element).xyAxes;
    } else if ([element conformsToProtocol:@protocol(GCAxis2DInput)]) {
        axes = (id<GCAxis2DInput>) element;
    }
    if (axes == nil) { return false; }

    const GCPoint2 v = axes.value;
    *outX = to_axis(v.x);
    *outY = to_axis(-v.y);   // the game's y runs the other way, as with SDL
    return true;
}

static void read_spatial(OSContPad *pad, int16_t *outLeftX, int16_t *outLeftY,
                         int16_t *outRightX, int16_t *outRightY) {
    if (configDisableGamepads) { return; }

    bool sawAny = false;

    for (GCController *controller in GCController.controllers) {
        if (![controller.productCategory isEqualToString:GCProductCategorySpatialController]) {
            continue;
        }
        id<GCDevicePhysicalInput> input = controller.input;
        if (input == nil) { continue; }
        sawAny = true;

        // Once per controller: what it actually offers. Element names are the
        // sort of thing that is nearly right and silently wrong - the first
        // attempt asked for a plain thumbstick, which neither half has, and
        // that looked exactly like sticks that did not work.
        static NSMutableSet *sDumped = nil;
        if (sDumped == nil) { sDumped = [NSMutableSet set]; }
        NSString *key = controller.vendorName ?: @"?";
        if (![sDumped containsObject:key]) {
            [sDumped addObject:key];
            // Names and aliases both. The localized name is what a player
            // would be shown - "L1-Taste" - and the aliases are the keys the
            // subscript actually answers to. Deriving the second from the first
            // is what silenced the sticks once already.
            for (id<GCPhysicalInputElement> el in input.elements) {
                NSLog(@"[controller] %@: \"%@\" aliases %@", key,
                      el.localizedName ?: @"?",
                      [el.aliases.allObjects componentsJoinedByString:@" "]);
            }
        }

        // The pair carries an ordinary DualSense layout split across two
        // halves: the left one has the left stick, L1, L2, square and triangle;
        // the right one the right stick, R1, R2, cross and circle. Both are
        // simply offered to the same binds, so the player's own key config
        // keeps working exactly as it does on a gamepad.
        // One stick per half, and its key is the generic one - measured on the
        // device, where GCInputLeftThumbstick and GCInputRightThumbstick are
        // both absent. Which stick it is comes from which controller it is on,
        // not from the key: the localized name says "Linker Stick", and reading
        // a key out of a display name is what broke this once already.
        NSString *name = controller.vendorName ?: @"";
        const bool isLeft = [name containsString:@"(L)"];
        int16_t x = 0, y = 0;
        if (element_stick(input, GCInputThumbstick, &x, &y)) {
            // Only a stick that is actually being pushed has anything to say.
            //
            // This reader runs after the gamepad one, so writing unconditionally
            // meant a resting Sense stick overwrote whatever a DualSense had
            // just reported with zero - and walking on the pad stopped working
            // the moment the tracked controllers were switched on nearby.
            const bool moved = abs(x) > AXIS_THRESHOLD / 4
                            || abs(y) > AXIS_THRESHOLD / 4;
            if (moved) {
                if (isLeft) { *outLeftX = x; *outLeftY = y; }
                else        { *outRightX = x; *outRightY = y; }
            }
        }
        // Which key actually answers, asked of the device rather than inferred
        // from a localized name. "Linker Stick" is what it is called, not what
        // it is keyed by, and guessing the second from the first is how this
        // went quiet twice.
        if (getenv("MARIOVISION_STICKPROBE")) {
            static int tick = 0;
            if ((++tick % 30) == 0) {
                NSArray *keys = @[ GCInputThumbstick, GCInputThumbstickButton,
                                   GCInputTrigger, GCInputLeftTrigger,
                                   GCInputRightTrigger, GCInputLeftShoulder,
                                   GCInputRightShoulder, GCInputGripButton,
                                   GCInputButtonA, GCInputButtonB,
                                   GCInputButtonMenu, GCInputButtonOptions ];
                NSMutableArray *hits = [NSMutableArray array];
                for (NSString *k in keys) {
                    id el = input[k];
                    if (el == nil) { continue; }
                    if ([el conformsToProtocol:@protocol(GCDirectionPadElement)]) {
                        const GCPoint2 v = ((id<GCDirectionPadElement>) el).xyAxes.value;
                        [hits addObject:[NSString stringWithFormat:@"%@=(%.2f,%.2f)", k, v.x, v.y]];
                    } else {
                        [hits addObject:[NSString stringWithFormat:@"%@=%.2f", k,
                                         element_value(input, k)]];
                    }
                }
                NSLog(@"[controller] %@ -> %@", key, [hits componentsJoinedByString:@" "]);
            }
        }

        // Square and circle are deliberately not bound. They were the punch,
        // and the punch is now something one does with the arm - leaving them
        // wired up would mean the same act had two ways of happening, one of
        // which quietly bypasses everything the tracked hands are for.
        // Square and circle come back when a button is meant to do the work.
        //
        // They are coopdx's B - punching, and picking a thing up along with it,
        // which is why one setting covers both here. Left bound while the body
        // does the same job, the same act would have two ways of happening, one
        // of which quietly bypasses the tracked hands.
        //
        // And in third person regardless of that setting. Swinging one's arm
        // only means anything while one is looking out of Mario's own eyes;
        // watching him from behind, the arm in the room and the arm on screen
        // have nothing to do with each other, so there is no second way for the
        // punch to happen and no reason to take the button away.
        extern bool mariovision_first_person_active(void);
        if (mariovision_scheme_button(MV_DO_PUNCH)
            || mariovision_scheme_button(MV_DO_CARRY)
            || !mariovision_first_person_active()) {
            if (element_pressed(input, GCInputButtonB)) { pad->button |= B_BUTTON; }
            if (isLeft && element_pressed(input, GCInputButtonA)) {
                pad->button |= B_BUTTON;   // square
            }
        }

        // The full map, read off the device's own aliases rather than guessed
        // from the header:
        //
        //   cross / square   -> "Button A"  (right / left half)
        //   circle / triangle-> "Button B"
        //   R1 / L1          -> "Grip"
        //   R2 / L2          -> "Trigger"
        //   Options          -> "Button Menu"
        //
        // Which half a button is on is what tells cross from square, since the
        // two share a key. Square and circle stay unbound: they were the punch,
        // and the punch is now something one does with the arm.
        const bool faceA = element_pressed(input, GCInputButtonA);
        const bool grip = element_pressed(input, GCInputGripButton);
        const float trigger = element_value(input, GCInputTrigger);

        if (!isLeft) {
            update_button(SDL_BTN_A, faceA);        // cross jumps
        } else {
            // L1 is Z - crouching, ground pounding, holding a shell.
            if (grip) { pad->button |= Z_TRIG; }
        }

        // Options is start, on either half. It was on "Button Options" before,
        // which these controllers do not have at all, so the button did nothing.
        update_button(SDL_BTN_START, element_pressed(input, GCInputButtonMenu));

        // The triggers belong to grabbing now, and are read there rather than
        // bound to anything.
        mariovision_hands_set_trigger(isLeft ? 0 : 1, trigger);

        // R1 opens our launcher, as on a gamepad. Edge triggered, or it would
        // reopen every frame it is held.
        {
            // R1 brings our launcher back, as the right shoulder does on a
            // gamepad. Gated to the pause screen, which is where it belongs.
            static bool sMenuHeld = false;
            const bool down = !isLeft && grip;
            if (down && !sMenuHeld && mariovision_menu_key_allowed()) {
                mariovision_request_menu();
            }
            sMenuHeld = down;
        }
    }

    if (!sawAny) { return; }

    // The binds, which read_gamepad applies for itself and returns before
    // reaching when there is no gamepad profile to read - which is every
    // session with tracked controllers.
    uint32_t buttons_down = 0;
    for (uint32_t i = 0; i < sNumJoyBinds; i++) {
        if (sButtons[sJoyBinds[i][0]]) { buttons_down |= sJoyBinds[i][1]; }
    }
    pad->button |= buttons_down;
}

static void controller_visionos_read(OSContPad *pad) {
    int16_t leftx = 0, lefty = 0, rightx = 0, righty = 0;

    read_gamepad(pad, &leftx, &lefty, &rightx, &righty);
    // After the gamepad: a session with a DualSense never enters this at all,
    // and one with Sense controllers finds nothing in the gamepad path.
    read_spatial(pad, &leftx, &lefty, &rightx, &righty);

    // After the gamepad, so the walk keys win over a resting stick.
    apply_keyboard(pad, &leftx, &lefty);

    if (rightx < -AXIS_THRESHOLD) { pad->button |= L_CBUTTONS; }
    if (rightx >  AXIS_THRESHOLD) { pad->button |= R_CBUTTONS; }
    if (righty < -AXIS_THRESHOLD) { pad->button |= U_CBUTTONS; }
    if (righty >  AXIS_THRESHOLD) { pad->button |= D_CBUTTONS; }

    update_analog_stick(&pad->stick_x, &pad->stick_y, leftx, lefty);
    update_analog_stick(&pad->ext_stick_x, &pad->ext_stick_y, rightx, righty);
}

static u32 controller_visionos_rawkey(void) {
    const uint32_t key = sLastPressed;
    sLastPressed = VK_INVALID;
    return key;
}

static void controller_visionos_shutdown(void) {
    [GCController stopWirelessControllerDiscovery];
    sController = nil;
    memset(sButtons, 0, sizeof(sButtons));
    memset(sPrevButtons, 0, sizeof(sPrevButtons));
}

struct ControllerAPI controller_visionos = {
    VK_BASE_SDL_GAMEPAD,
    controller_visionos_init,
    controller_visionos_read,
    controller_visionos_rawkey,
    // Rumble is left unimplemented: GameController exposes haptics through
    // CoreHaptics rather than a simple rumble call, and the optional hooks are
    // allowed to be null.
    NULL,
    NULL,
    controller_visionos_reconfig,
    controller_visionos_shutdown,
};
