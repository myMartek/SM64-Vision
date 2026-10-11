/*
 * Window manager backend for visionOS, implementing
 * src/pc/gfx/gfx_window_manager_api.h.
 *
 * There is no window to manage here: the app owns the CAMetalLayer (phase 1) or
 * the CompositorServices layer (phase 2). What is left is reporting the drawable
 * size, timing, and the text input hooks djui needs. main_loop returns
 * immediately rather than spinning, because on visionOS the app drives the
 * frames - see mariovision_platform.h.
 */

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>

#include <execinfo.h>
#include <signal.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "gfx_metal.h"
#include "mariovision_platform.h"
#include "mariovision_portal.h"
#include "visionos_sdl_compat.h"            // visionos_set_display_refresh_rate
#include "pc/controller/controller_mouse.h" // mouse_on_scroll
#include "pc/gfx/gfx_window_manager_api.h"

static double gfx_visionos_get_time(void);

static uint32_t sWidth = 1920;
static uint32_t sHeight = 1080;
static bool sTextInputActive = false;

// Rendering. The game thread drives its own frames: it asks the layer for a
// drawable, which blocks until one frees up and therefore paces the game to the
// display, then records and presents. That keeps coopdx's structure intact -
// including the blocking loading and ROM-setup screens, which drive frames the
// same way the main loop does - and avoids handing an encoder across threads.
static CAMetalLayer *sLayer;
static id<MTLCommandQueue> sCommandQueue;
static id<MTLTexture> sDepthTexture;
static id<CAMetalDrawable> sDrawable;
static id<MTLCommandBuffer> sCommandBuffer;
static id<MTLRenderCommandEncoder> sEncoder;
static bool sInFrame = false;

static const MTLPixelFormat kDepthFormat = MTLPixelFormatDepth32Float;

static kb_callback_t sOnKeyDown = NULL;
static kb_callback_t sOnKeyUp = NULL;
static void (*sOnAllKeysUp)(void) = NULL;
static void (*sOnTextInput)(char *) = NULL;
static void (*sOnTextEditing)(char *, int) = NULL;
static void (*sOnScroll)(float, float) = NULL;

void mariovision_set_drawable_size(uint32_t width, uint32_t height) {
    if (width > 0)  { sWidth = width; }
    if (height > 0) { sHeight = height; }
}

/// Same, but only when the portal is not running. The game window and the
/// portal both report a size, and gfx_pc derives its aspect ratio from
/// whatever arrived last - which decides where the HUD's edges land. The
/// portal's pane is 4:3 no matter how wide the eye texture is, so while it is
/// up, its value is the one that counts.
static void set_drawable_size_from_window(uint32_t width, uint32_t height) {
    extern bool mariovision_portal_active(void);
    if (mariovision_portal_active()) { return; }
    mariovision_set_drawable_size(width, height);
}

void mariovision_set_refresh_rate(int hz) {
    visionos_set_display_refresh_rate(hz);
}

void mariovision_pointer_scroll(float x, float y) {
    if (sOnScroll) { sOnScroll(x, y); }
    mouse_on_scroll(x, y);
}

// The layer is written by the main thread (the game window can open and close
// at any time) and read by the game thread each frame.
static pthread_mutex_t sLayerMutex = PTHREAD_MUTEX_INITIALIZER;
static id<MTLDevice> sDevice;

/// Prints why the process is dying, to the one channel that reaches a tethered
/// headset. Aborts here leave nothing behind otherwise: devicectl reports only
/// "signal 6", and the crash report is not reachable from the command line -
/// which cost several rounds of guessing at what had gone wrong.
static void mariovision_fatal_signal(int sig) {
    void *frames[64];
    const int count = backtrace(frames, 64);
    fprintf(stderr, "\n[mariovision] fatal signal %d\n", sig);
    fflush(stderr);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    fflush(stderr);

    signal(sig, SIG_DFL);
    raise(sig);
}

static void mariovision_uncaught_exception(NSException *exception) {
    fprintf(stderr, "\n[mariovision] uncaught exception: %s: %s\n%s\n",
            exception.name.UTF8String ?: "?",
            exception.reason.UTF8String ?: "?",
            [[exception.callStackSymbols componentsJoinedByString:@"\n"] UTF8String] ?: "");
    fflush(stderr);
}

static void install_crash_reporting(void) {
    NSSetUncaughtExceptionHandler(mariovision_uncaught_exception);
    signal(SIGABRT, mariovision_fatal_signal);
    signal(SIGSEGV, mariovision_fatal_signal);
    signal(SIGBUS,  mariovision_fatal_signal);
    signal(SIGILL,  mariovision_fatal_signal);
    signal(SIGFPE,  mariovision_fatal_signal);
    // Deliberately not SIGTRAP. Swift's runtime does trap there, so catching it
    // looked like a way to get a backtrace out of those crashes - but the app
    // stopped launching the moment it was added. SIGTRAP is not only used for
    // fatal errors on this platform, and turning every one of them into a
    // re-raised signal kills the process. A backtrace is not worth that.
}

void mariovision_boot(void) {
    if (sDevice != nil) { return; }
    install_crash_reporting();

    // Device and queue exist independently of any window: the game boots and
    // loads with no drawable at all - start_frame just drops frames until the
    // game window opens and provides a layer.
    sDevice = MTLCreateSystemDefaultDevice();
    if (sDevice == nil) {
        NSLog(@"[gfx_visionos] no Metal device");
        return;
    }
    gfx_metal_set_device(sDevice);
    sCommandQueue = [sDevice newCommandQueue];

    mariovision_start_game_thread();
}

void mariovision_clear_metal_layer(CAMetalLayer *layer) {
    // Clears only if this layer is still the active one: a close-and-reopen can
    // interleave so that the old window's teardown runs after the new window
    // already attached, and must not tear the new layer away.
    pthread_mutex_lock(&sLayerMutex);
    if (sLayer == layer) { sLayer = nil; }
    pthread_mutex_unlock(&sLayerMutex);
}

bool mariovision_has_layer(void) {
    pthread_mutex_lock(&sLayerMutex);
    const bool has = (sLayer != nil);
    pthread_mutex_unlock(&sLayerMutex);
    return has;
}

bool mariovision_is_visible(void) {
    return mariovision_has_layer() || mariovision_portal_active();
}

bool mariovision_should_idle(void) {
    // Loaded, but nothing is showing it and nothing has started it either. Once
    // the game is actually running it keeps running with the presentation
    // closed - closing the window used to take the sound with it, and picking
    // the game back up should feel like returning to it, not restarting it.
    // The cost is that a started game keeps simulating in the background.
    extern bool gGameInited;
    return gGameInited && !mariovision_is_visible() && !mariovision_game_running();
}

void mariovision_set_metal_layer(CAMetalLayer *layer) {
    if (layer != nil) {
        layer.device = sDevice;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;

        const CGSize size = layer.drawableSize;
        set_drawable_size_from_window((uint32_t) size.width, (uint32_t) size.height);
    }

    pthread_mutex_lock(&sLayerMutex);
    sLayer = layer;
    pthread_mutex_unlock(&sLayerMutex);
}

extern int sm64_coopdx_main(int argc, char *argv[]);

static void *game_thread_main(void *arg) {
    (void) arg;
    pthread_setname_np("sm64coopdx");

    char program[] = "sm64coopdx";
    char *argv[] = { program, NULL };
    sm64_coopdx_main(1, argv);
    return NULL;
}

void mariovision_start_game_thread(void) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);

    // coopdx recurses deeply through the level scripts during startup; the
    // default 512 KiB secondary-thread stack is not enough.
    pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    pthread_t thread;
    pthread_create(&thread, &attr, game_thread_main, NULL);
    pthread_attr_destroy(&attr);
}

/// Depth buffer to match the drawable; recreated whenever the size changes.
static bool ensure_depth_texture(uint32_t width, uint32_t height) {
    if (sDepthTexture != nil &&
        sDepthTexture.width == width && sDepthTexture.height == height) {
        return true;
    }

    MTLTextureDescriptor *desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kDepthFormat
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget;
    desc.storageMode = MTLStorageModePrivate;

    sDepthTexture = [sDevice newTextureWithDescriptor:desc];
    return sDepthTexture != nil;
}

//-----------------------------------------------------------------------------
// GfxWindowManagerAPI
//-----------------------------------------------------------------------------

static void gfx_visionos_init(const char *window_title) {
    (void) window_title; // the window's title bar belongs to visionOS
}

static void gfx_visionos_set_keyboard_callbacks(kb_callback_t on_key_down, kb_callback_t on_key_up,
                                                void (*on_all_keys_up)(void),
                                                void (*on_text_input)(char *),
                                                void (*on_text_editing)(char *, int)) {
    sOnKeyDown = on_key_down;
    sOnKeyUp = on_key_up;
    sOnAllKeysUp = on_all_keys_up;
    sOnTextInput = on_text_input;
    sOnTextEditing = on_text_editing;
}

static void gfx_visionos_set_scroll_callback(void (*on_scroll)(float, float)) {
    sOnScroll = on_scroll;
}

static void gfx_visionos_main_loop(void (*run_one_game_iter)(void)) {
    // One iteration per call, not a loop. Every caller in coopdx - the main
    // loop, the loading screen, the ROM setup screen - already wraps this in
    // its own while, and the pacing comes from nextDrawable blocking in
    // start_frame rather than from spinning here.
    if (!run_one_game_iter) { return; }

    // Runs whatever the native UI queued and keeps coopdx' startup menus
    // suppressed. Has to happen on this thread, not the app's. Runs even while
    // idling below - Start from the launcher arrives through this queue.
    mariovision_ui_pump();

    // No window, no simulation: once loading is done, a game nobody can see is
    // not stepped either - the thread naps instead of burning a core on
    // invisible frames, and gameplay freezes safely until the window returns.
    // Loading (gGameInited false) keeps running so the launcher's progress is
    // real. Revisit for multiplayer: a frozen loop stops answering keep-alives,
    // so a networked session will need iterations (not just the pump) to
    // continue while hidden.
    if (mariovision_should_idle()) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        return;
    }

    static uint64_t sIterations = 0;
    const double start = gfx_visionos_get_time();
    run_one_game_iter();
    const double elapsed = gfx_visionos_get_time() - start;

    // The game ticks at 30 Hz, so a frame is ~33 ms. The first few are logged
    // to show that startup got going at all, and anything far over budget after
    // that is reported as the hitch it is - a frame that takes seconds is
    // indistinguishable from a hang from the outside.
    if (sIterations < 5) {
        NSLog(@"[gfx_visionos] iteration %llu took %.1f ms",
              (unsigned long long) sIterations, elapsed * 1000.0);
    } else if (elapsed > 0.25) {
        NSLog(@"[gfx_visionos] hitch: iteration %llu took %.1f ms",
              (unsigned long long) sIterations, elapsed * 1000.0);
    }
    sIterations++;
}

static void gfx_visionos_get_dimensions(uint32_t *width, uint32_t *height) {
    if (width)  { *width = sWidth; }
    if (height) { *height = sHeight; }
}

static void gfx_visionos_handle_events(void) {
    // Input is pushed in from the app rather than polled.
}

// Frames are silently dropped when a drawable cannot be had, and a dropped
// frame looks exactly like a hung renderer from the outside. Each distinct
// reason is reported once so the cause is visible without a debugger.
static void report_dropped_frame(const char *reason) {
    static const char *sLastReason = NULL;
    if (sLastReason != reason) {
        sLastReason = reason;
        // NSLog rather than stderr: without a console attached the app's stdio
        // goes nowhere, whereas this shows up in
        // `xcrun simctl spawn <sim> log show`.
        NSLog(@"[gfx_visionos] dropping frames: %s", reason);
    }

    // Pacing normally comes from nextDrawable blocking. When there is no frame
    // to draw, nothing throttles coopdx's main loop, and it spins a whole core
    // re-running the audio synthesis. Back off instead.
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 4 * 1000 * 1000 };
    nanosleep(&ts, NULL);
}

/// Closes the frame opened by start_frame, optionally presenting it.
///
/// Metal asserts if a render command encoder is released without endEncoding,
/// so the encoder has to be closed on every path out of a frame - including the
/// ones where gfx_pc opens a frame and then abandons it without reaching
/// gfx_display_frame.
static void finish_open_frame(bool present) {
    if (!sInFrame) { return; }
    sInFrame = false;

    gfx_metal_end_frame();
    [sEncoder endEncoding];
    if (present && sDrawable != nil) {
        [sCommandBuffer presentDrawable:sDrawable];
    }
    [sCommandBuffer commit];

    sEncoder = nil;
    sCommandBuffer = nil;
    sDrawable = nil;
}

static bool gfx_visionos_start_frame(void) {
    // Portal pass: the compositor thread already has an encoder open through
    // gfx_metal_begin_frame - there is no window drawable to acquire and
    // nothing to present from here.
    if (mariovision_portal_pass_active()) { return true; }

    // Switching out of a stereo mode hands rendering back to this path while
    // the compositor thread can still be inside a frame - between latching the
    // display list and finishing the second eye. Both drive the same gfx state
    // machine and the same Metal encoder, and meeting there aborts the process.
    // Skip the frame until it is done; at 30 Hz that costs at most one.
    if (mariovision_portal_renderer_busy()) { return false; }

    // A frame still open here means gfx_pc abandoned the previous one. Close it
    // without presenting - it was never finished, so there is nothing worth
    // showing - rather than let the encoder be released un-ended, which Metal
    // asserts on.
    if (sInFrame) {
        report_dropped_frame("previous frame was never displayed");
        finish_open_frame(false);
    }

    pthread_mutex_lock(&sLayerMutex);
    CAMetalLayer *layer = sLayer;
    pthread_mutex_unlock(&sLayerMutex);

    if (layer == nil || sCommandQueue == nil) {
        report_dropped_frame("no Metal layer - game window not open");
        return false;
    }

    @autoreleasepool {
        // Blocks until the display releases one, which is what paces the game.
        sDrawable = [layer nextDrawable];
        if (sDrawable == nil) {
            report_dropped_frame("nextDrawable returned nil");
            return false;
        }

        const uint32_t width = (uint32_t) sDrawable.texture.width;
        const uint32_t height = (uint32_t) sDrawable.texture.height;
        set_drawable_size_from_window(width, height);

        if (!ensure_depth_texture(width, height)) {
            report_dropped_frame("depth texture allocation failed");
            sDrawable = nil;
            return false;
        }

        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = sDrawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
        pass.depthAttachment.texture = sDepthTexture;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionDontCare;
        // gfx_pc uses a 0..1 depth range with a less-equal test, so far is 1.
        pass.depthAttachment.clearDepth = 1.0;

        sCommandBuffer = [sCommandQueue commandBuffer];
        sEncoder = [sCommandBuffer renderCommandEncoderWithDescriptor:pass];
        if (sEncoder == nil) {
            report_dropped_frame("could not create a render command encoder");
            sCommandBuffer = nil;
            sDrawable = nil;
            return false;
        }

        gfx_metal_begin_frame(sEncoder, MTLPixelFormatBGRA8Unorm, kDepthFormat, width, height, 1);
        sInFrame = true;
        return true;
    }
}

static void gfx_visionos_swap_buffers_begin(void) {
    // The portal's encoder lifecycle belongs to the compositor loop.
    if (mariovision_portal_pass_active()) { return; }

    // Called even when gfx_pc dropped the frame, whereas swap_buffers_end is
    // not - so the frame is closed out here to keep the pairing balanced.
    @autoreleasepool {
        finish_open_frame(true);
    }
}

static void gfx_visionos_swap_buffers_end(void) {
}

static double gfx_visionos_get_time(void) {
    static mach_timebase_info_data_t timebase;
    if (timebase.denom == 0) { mach_timebase_info(&timebase); }
    const uint64_t ns = mach_absolute_time() * timebase.numer / timebase.denom;
    return (double) ns / 1e9;
}

static void gfx_visionos_shutdown(void) {
}

static void gfx_visionos_start_text_input(void) {
    sTextInputActive = true;
}

static void gfx_visionos_stop_text_input(void) {
    sTextInputActive = false;
}

static char *gfx_visionos_get_clipboard_text(void) {
    @autoreleasepool {
        NSString *text = UIPasteboard.generalPasteboard.string;
        if (text == nil) { return NULL; }

        // djui frees the returned buffer, and caps what it reads at
        // WAPI_CLIPBOARD_BUFSIZ, so hand back a plain heap copy.
        static char buffer[WAPI_CLIPBOARD_BUFSIZ];
        snprintf(buffer, sizeof(buffer), "%s", text.UTF8String ?: "");
        return strdup(buffer);
    }
}

static void gfx_visionos_set_clipboard_text(const char *text) {
    @autoreleasepool {
        UIPasteboard.generalPasteboard.string = text ? @(text) : @"";
    }
}

static void gfx_visionos_set_cursor_visible(bool visible) {
    // visionOS draws its own gaze cursor and gives apps no say in it.
    (void) visible;
}

static void gfx_visionos_delay(unsigned int ms) {
    struct timespec ts = {
        .tv_sec  = ms / 1000,
        .tv_nsec = (long) (ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);
}

static int gfx_visionos_get_max_msaa(void) {
    // The compositor decides antialiasing for immersive content, and the phase
    // one window renders straight into the drawable.
    return 1;
}

static void gfx_visionos_set_window_title(const char *title) {
    (void) title;
}

static void gfx_visionos_reset_window_title(void) {
}

static bool gfx_visionos_has_focus(void) {
    // A visionOS scene that is rendering is by definition the one being looked
    // at; there is no unfocused-but-drawing state to report.
    return true;
}

struct GfxWindowManagerAPI gfx_visionos_wm_api = {
    gfx_visionos_init,
    gfx_visionos_set_keyboard_callbacks,
    gfx_visionos_set_scroll_callback,
    gfx_visionos_main_loop,
    gfx_visionos_get_dimensions,
    gfx_visionos_handle_events,
    gfx_visionos_start_frame,
    gfx_visionos_swap_buffers_begin,
    gfx_visionos_swap_buffers_end,
    gfx_visionos_get_time,
    gfx_visionos_shutdown,
    gfx_visionos_start_text_input,
    gfx_visionos_stop_text_input,
    gfx_visionos_get_clipboard_text,
    gfx_visionos_set_clipboard_text,
    gfx_visionos_set_cursor_visible,
    gfx_visionos_delay,
    gfx_visionos_get_max_msaa,
    gfx_visionos_set_window_title,
    gfx_visionos_reset_window_title,
    gfx_visionos_has_focus,
};
