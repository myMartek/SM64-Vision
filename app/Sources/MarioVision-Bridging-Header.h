//
//  Exposes the game's platform interface to Swift.
//
//  Only mariovision_platform.h is bridged: it is the whole contract between the
//  app and the game, and pulling in more of coopdx's headers would drag the N64
//  types along with them.
//

#import "pc/visionos/mariovision_platform.h"
#import "pc/visionos/mariovision_portal.h"
#import "pc/visionos/mariovision_hands.h"
#import "pc/visionos/mariovision_levels.h"
#import "pc/visionos/gfx_metal.h"

// Thread-safe cosmetic preference. Implemented in mario_misc.c.
void mariovision_set_silver_metal(bool enabled);
