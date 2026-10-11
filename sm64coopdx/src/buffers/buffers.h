#ifndef BUFFERS_H
#define BUFFERS_H

#include <PR/ultratypes.h>

#include "audio/data.h"
#include "game/save_file.h"
#include "game/game_init.h"

extern u8 gDecompressionHeap[];

extern u8 gAudioHeap[];

extern u8 gAudioSPTaskYieldBuffer[];

extern u8 gUnusedThread2Stack[];

extern u8 gIdleThreadStack[];
extern u8 gThread3Stack[];
extern u8 gThread4Stack[];
extern u8 gThread5Stack[];
#ifdef VERSION_SH
extern u8 gThread6Stack[];
#endif

extern u8 gGfxSPTaskYieldBuffer[];

extern struct SaveBuffer gSaveBuffer;

extern u8 gGfxSPTaskStack[];

// Stays at one. More pools looked like the obvious cure for the game thread
// overwriting the list being rendered, but set_segment_base_addr(1, ...) moves
// with the pool, so a replay of an older list resolves its segmented addresses
// against the newest pool - which crashed immediately and reliably. The threads
// are kept apart by a lock instead; see mariovision_portal_lock_pool.
#define GFX_NUM_POOLS 1

extern struct GfxPool gGfxPools[GFX_NUM_POOLS];

#endif // BUFFERS_H
