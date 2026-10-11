/*
 * Audio backend for visionOS, implementing src/pc/audio/audio_api.h.
 *
 * Mirrors audio_sdl.c's semantics exactly: the game synthesises 32 kHz stereo
 * signed 16-bit samples and queues them, then polls buffered() to decide how
 * many frames to synthesise next. That maps onto a ring buffer drained by an
 * AVAudioSourceNode, which also lets AVAudioEngine handle the resample to
 * whatever rate the headset's output is running at.
 */

#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h> // UIApplicationWillTerminateNotification
#import <QuartzCore/QuartzCore.h> // CACurrentMediaTime

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "pc/audio/audio_api.h"
#include "mariovision_platform.h"

// 32000 Hz stereo. Half a second of slack is far more than the ~6000 frame
// ceiling audio_avf_play enforces, so the writer never overruns the reader.
#define RING_FRAMES 16384
#define SAMPLE_RATE 32000.0

typedef struct {
    int16_t left;
    int16_t right;
} Frame;

static Frame sRing[RING_FRAMES];

// Set while the app is in the background - the crown does that rather than
// quitting - so the engine is not brought straight back up under the restart
// below. Cleared on return, and the next buffer starts it again.
static bool sSuspended = false;

// Single producer (the game's audio thread) and single consumer (the CoreAudio
// render thread), so plain atomics on the two cursors are enough - no lock is
// taken on the render thread.
static _Atomic uint32_t sWriteIndex = 0;
static _Atomic uint32_t sReadIndex = 0;

static AVAudioEngine *sEngine;
static AVAudioSourceNode *sSourceNode;

static uint32_t frames_buffered(void) {
    const uint32_t w = atomic_load_explicit(&sWriteIndex, memory_order_acquire);
    const uint32_t r = atomic_load_explicit(&sReadIndex, memory_order_acquire);
    return w - r; // unsigned wraparound is well defined and correct here
}

/// Brings the engine back up after the system took it down. Reactivating the
/// session first matters: after an interruption the engine cannot start until
/// the session is active again.
static void audio_avf_restart(const char *why) {
    @autoreleasepool {
        if (sEngine == nil) { return; }

        NSError *error = nil;
        [AVAudioSession.sharedInstance setActive:YES error:nil];
        if (sEngine.isRunning) { return; }
        if (![sEngine startAndReturnError:&error]) {
            fprintf(stderr, "[audio] restart after %s failed: %s\n", why,
                    error.localizedDescription.UTF8String ?: "unknown");
        } else {
            fprintf(stderr, "[audio] restarted after %s\n", why);
        }
        fflush(stderr);
    }
}

static bool audio_avf_init(void) {
    @autoreleasepool {
        NSError *error = nil;

        // Ambient + mixWithOthers so the game does not silence whatever else the
        // wearer has playing; visionOS apps share the space by default.
        AVAudioSession *session = AVAudioSession.sharedInstance;
        // Playback, not Ambient: Ambient is silenced the moment the app stops
        // being the foreground scene, which is exactly what closing the game's
        // window does. MixWithOthers keeps other audio audible alongside.
        if (![session setCategory:AVAudioSessionCategoryPlayback
                      withOptions:AVAudioSessionCategoryOptionMixWithOthers
                            error:&error]) {
            fprintf(stderr, "AVAudioSession setCategory failed: %s\n",
                    error.localizedDescription.UTF8String ?: "unknown");
            return false;
        }
        if (![session setActive:YES error:&error]) {
            fprintf(stderr, "AVAudioSession setActive failed: %s\n",
                    error.localizedDescription.UTF8String ?: "unknown");
            return false;
        }

        AVAudioFormat *format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:SAMPLE_RATE
                                                                               channels:2];
        if (format == nil) { return false; }

        sSourceNode = [[AVAudioSourceNode alloc] initWithFormat:format
            renderBlock:^OSStatus(BOOL *isSilence,
                                  const AudioTimeStamp *timestamp,
                                  AVAudioFrameCount frameCount,
                                  AudioBufferList *outputData) {
                (void) timestamp;

                // standardFormat is non-interleaved float32, so there is one
                // buffer per channel.
                float *out[2] = { NULL, NULL };
                for (UInt32 b = 0; b < outputData->mNumberBuffers && b < 2; b++) {
                    out[b] = (float *) outputData->mBuffers[b].mData;
                }
                if (out[0] == NULL) {
                    *isSilence = YES;
                    return noErr;
                }

                uint32_t available = frames_buffered();
                uint32_t read = atomic_load_explicit(&sReadIndex, memory_order_relaxed);

                for (AVAudioFrameCount i = 0; i < frameCount; i++) {
                    float l = 0.0f, r = 0.0f;
                    if (i < available) {
                        const Frame frame = sRing[(read + i) % RING_FRAMES];
                        l = frame.left / 32768.0f;
                        r = frame.right / 32768.0f;
                    }
                    out[0][i] = l;
                    if (out[1]) { out[1][i] = r; }
                }

                const uint32_t consumed = available < frameCount ? available : frameCount;
                atomic_store_explicit(&sReadIndex, read + consumed, memory_order_release);

                *isSilence = (consumed == 0);
                return noErr;
            }];

        sEngine = [AVAudioEngine new];
        [sEngine attachNode:sSourceNode];
        [sEngine connect:sSourceNode to:sEngine.mainMixerNode format:format];

        // AVAudioEngine stops itself whenever the audio configuration changes,
        // and closing a window is such a change. Nothing restarts it on its
        // own, which is why the sound went away for good rather than pausing -
        // the game kept running the whole time, the portal kept rendering, only
        // the engine was down. Same for an interruption that ends.
        NSNotificationCenter *centre = NSNotificationCenter.defaultCenter;
        [centre addObserverForName:AVAudioEngineConfigurationChangeNotification
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification *note) {
            (void) note;
            audio_avf_restart("configuration changed");
        }];
        // Leaving must actually go quiet. Pressing the crown backgrounds the
        // app rather than quitting it, and the restart below would otherwise
        // find the engine down and put it straight back up - the game would
        // keep playing to nobody. Suspend on the way out, allow it again on the
        // way back, and the next buffer brings the sound with it.
        [centre addObserverForName:UIApplicationDidEnterBackgroundNotification
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification *note) {
            (void) note;
            sSuspended = true;
            [sEngine stop];
        }];
        [centre addObserverForName:UIApplicationWillEnterForegroundNotification
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification *note) {
            (void) note;
            sSuspended = false;
        }];
        [centre addObserverForName:AVAudioSessionInterruptionNotification
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification *note) {
            NSNumber *type = note.userInfo[AVAudioSessionInterruptionTypeKey];
            if (type.unsignedIntegerValue == AVAudioSessionInterruptionTypeEnded) {
                audio_avf_restart("interruption ended");
            }
        }];

        if (![sEngine startAndReturnError:&error]) {
            fprintf(stderr, "AVAudioEngine start failed: %s\n",
                    error.localizedDescription.UTF8String ?: "unknown");
            sEngine = nil;
            sSourceNode = nil;
            return false;
        }
        return true;
    }
}

static int audio_avf_buffered(void) {
    return (int) frames_buffered();
}

static int audio_avf_get_desired_buffered(void) {
    // audio_sdl.c targets 1100 frames (~34 ms), which worked while synthesis
    // ran inside the vsync-paced frame loop. On the self-paced audio thread a
    // single late nanosleep wakeup outlasts 34 ms, the ring runs dry and the
    // render callback pads with silence - an audible crack. 75 ms of slack
    // absorbs scheduler jitter; the added latency is inaudible in play.
    return 2400;
}

static void audio_avf_play(const uint8_t *buf, size_t len) {
    // Audible only while the game is visible - in its window or in the portal.
    // The engine boots and sits in its hidden title screen before Start, and
    // that music has no business playing under the launcher. The ring drains to
    // silence in about half a second once samples stop coming.
    // Silent until the game has actually been started - but not silenced again
    // when its window closes.
    const bool visible = mariovision_is_visible();
    const bool running = mariovision_game_running();

    // Four attempts at this bug have each fixed something that turned out not
    // to be the cause. Rather than a fifth guess, report the chain: whether the
    // engine is up, whether the session is active, and which of the two gates
    // below is closed. One second apart, only with the trace enabled.
    {
        static int sTraceOn = -1;
        if (sTraceOn < 0) { sTraceOn = (getenv("MARIOVISION_VTXLOG") != NULL); }
        if (sTraceOn) {
            static double sLast = 0;
            const double now = CACurrentMediaTime();
            if (now - sLast > 1.0) {
                sLast = now;
                fprintf(stderr, "[audio] engine=%d session=%d visible=%d running=%d buffered=%u\n",
                        sEngine.isRunning ? 1 : 0,
                        AVAudioSession.sharedInstance.isOtherAudioPlaying ? 1 : 0,
                        visible ? 1 : 0, running ? 1 : 0, frames_buffered());
                fflush(stderr);
            }
        }
    }

    if (!visible && !running) { return; }

    // The engine stops when a window closes and does not come back. The
    // notification that is meant to announce that never arrives - measured on
    // device, engine=0 for as long as it was watched while everything else was
    // in order. So it is not waited for: if the engine is down and there is
    // something to play, it is brought back up. Twice a second at most, which
    // costs a boolean read the rest of the time.
    if (sEngine != nil && !sEngine.isRunning && !sSuspended) {
        static double sLastRestart = 0;
        const double now = CACurrentMediaTime();
        if (now - sLastRestart > 0.5) {
            sLastRestart = now;
            audio_avf_restart("engine found stopped");
        }
        return;
    }

    // audio_sdl.c drops samples past this point rather than letting latency grow.
    if (frames_buffered() >= 6000) { return; }

    const uint32_t frames = (uint32_t) (len / sizeof(Frame));
    const Frame *in = (const Frame *) buf;

    uint32_t write = atomic_load_explicit(&sWriteIndex, memory_order_relaxed);
    for (uint32_t i = 0; i < frames; i++) {
        sRing[(write + i) % RING_FRAMES] = in[i];
    }
    atomic_store_explicit(&sWriteIndex, write + frames, memory_order_release);
}

static void audio_avf_shutdown(void) {
    @autoreleasepool {
        [sEngine stop];
        sEngine = nil;
        sSourceNode = nil;
        [AVAudioSession.sharedInstance setActive:NO error:nil];
    }
}

struct AudioAPI audio_avf = {
    audio_avf_init,
    audio_avf_buffered,
    audio_avf_get_desired_buffered,
    audio_avf_play,
    audio_avf_shutdown,
};
