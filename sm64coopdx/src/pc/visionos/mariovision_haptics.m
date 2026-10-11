//-----------------------------------------------------------------------------
// Haptics in the tracked controllers.
//
// A punch that lands makes a sound and nothing else, and in a headset that is
// oddly hollow: the hand is where the impact happened, so the hand should be
// where it is felt. This is per controller rather than per device pair, because
// feeling a hit in the hand that did not throw it is worse than feeling nothing.
//
// Everything is deliberately cheap. A pattern is built once per intensity step
// and replayed, since a punch can arrive several times a second and allocating
// a CoreHaptics pattern each time is a surprising amount of work for a knock.
//-----------------------------------------------------------------------------

#import <CoreHaptics/CoreHaptics.h>
#import <GameController/GameController.h>

#include "mariovision_hands.h"

#include <stdatomic.h>

/// One engine per hand, made on first use and kept.
///
/// CoreHaptics engines stop on an audio-session interruption and never come
/// back on their own - the same trap the audio engine fell into, which is
/// documented at length in audio_avf.m. The handlers below start them again.
static CHHapticEngine *sEngine[MV_HAND_COUNT];
static NSString *sEngineFor[MV_HAND_COUNT];

/// Which controller is which hand, by the name the device reports: the pair
/// arrives as "... Sense Controller (L)" and "(R)". Chirality proper lives on
/// the ARKit side, and reaching across for it would tie this file to the
/// renderer's lifetime for no gain.
static GCController *controller_for_side(int side) {
    NSString *want = (side == MV_HAND_LEFT) ? @"(L)" : @"(R)";
    for (GCController *c in GCController.controllers) {
        if (![c.productCategory isEqualToString:GCProductCategorySpatialController]) {
            continue;
        }
        NSString *name = c.vendorName ?: @"";
        if ([name containsString:want]) { return c; }
    }
    return nil;
}

static CHHapticEngine *engine_for_side(int side) {
    if (side < 0 || side >= MV_HAND_COUNT) { return nil; }

    GCController *controller = controller_for_side(side);
    if (controller == nil) { return nil; }

    // Rebuilt when the controller behind it changes, so a pair swapped over
    // mid-session does not keep buzzing the wrong hand.
    NSString *name = controller.vendorName ?: @"?";
    if (sEngine[side] != nil && [sEngineFor[side] isEqualToString:name]) {
        return sEngine[side];
    }

    GCDeviceHaptics *haptics = controller.haptics;
    if (haptics == nil) {
        static bool sSaid = false;
        if (!sSaid) {
            sSaid = true;
            mariovision_hands_log("[haptics] \"%s\" offers no haptics at all",
                                  (controller.vendorName ?: @"?").UTF8String);
        }
        return nil;
    }
    {
        static bool sSaid = false;
        if (!sSaid) {
            sSaid = true;
            mariovision_hands_log("[haptics] localities: %s",
                                  haptics.supportedLocalities.description.UTF8String);
        }
    }

    // Default is the only locality the framework promises exists, and on a
    // controller held in one hand there is nothing else to aim at anyway - the
    // separation between hands comes from picking the device, not the locality.
    CHHapticEngine *engine =
        [haptics createEngineWithLocality:GCHapticsLocalityDefault];
    if (engine == nil) { return nil; }

    engine.autoShutdownEnabled = YES;
    engine.resetHandler = ^{ [engine startAndReturnError:nil]; };
    engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) { (void) reason; };

    NSError *error = nil;
    if (![engine startAndReturnError:&error]) {
        mariovision_hands_log("[haptics] engine would not start: %s",
                              error.localizedDescription.UTF8String);
        return nil;
    }

    sEngine[side] = engine;
    sEngineFor[side] = name;
    return engine;
}

void mariovision_hand_haptic(int side, float intensity, float sharpness) {
    if (side < 0 || side >= MV_HAND_COUNT) { return; }

    // Off the game thread. CoreHaptics is not documented as safe to call from
    // anywhere, and the game thread is the one that must never wait.
    dispatch_async(dispatch_get_main_queue(), ^{
        CHHapticEngine *engine = engine_for_side(side);
        if (engine == nil) { return; }

        CHHapticEventParameter *i =
            [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity
                                                          value:intensity];
        CHHapticEventParameter *s =
            [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness
                                                          value:sharpness];
        // Transient, not continuous: a punch is an event, and a knock that
        // stops by itself can never be left running by a state machine that
        // took an unexpected branch.
        CHHapticEvent *event =
            [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticTransient
                                          parameters:@[ i, s ]
                                        relativeTime:0];

        NSError *error = nil;
        CHHapticPattern *pattern =
            [[CHHapticPattern alloc] initWithEvents:@[ event ] parameters:@[] error:&error];
        if (pattern == nil) { return; }

        id<CHHapticPatternPlayer> player = [engine createPlayerWithPattern:pattern error:&error];
        if (player == nil) { return; }
        [player startAtTime:0 error:&error];
    });
}

bool mariovision_hand_haptic_available(int side) {
    return controller_for_side(side) != nil;
}
