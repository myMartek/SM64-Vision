#ifndef MARIOVISION_LEVELS_H
#define MARIOVISION_LEVELS_H

//-----------------------------------------------------------------------------
// Places to jump to while trying things out.
//
// The game's own table is keyed by its Japanese development names, which are no
// use in a menu, so this is a plain readable list in the order one meets them.
//-----------------------------------------------------------------------------

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//-----------------------------------------------------------------------------
// Texture packs
//
// Passed through rather than reimplemented: DynOS already keeps the list, and
// remembers which are on across restarts in the game's own configuration.
//
// A pack that has just been copied in is not in this list yet - the folders are
// scanned once, as the game comes up - and a new one arrives switched off, by
// the game's own rule. Both together are why "downloaded" and "in use" are two
// different things here.
//-----------------------------------------------------------------------------

/// Read the pack folders again, without restarting.
///
/// The scan happens once as the game comes up, so a pack copied in later is
/// invisible until then - which is a poor thing to ask of somebody who has just
/// waited for a six hundred megabyte download. Adding a pack looks up its path
/// first and returns what is already there, so a second pass costs nothing and
/// cannot produce doubles.
///
/// Queued rather than done here: it walks thousands of files, and the thread
/// that must not be kept waiting is the one drawing the world.
void mariovision_request_pack_rescan(void);
void mariovision_pack_rescan_if_asked(void);
/// True while one is pending or running, so the interface can say so.
bool mariovision_pack_rescanning(void);

int mariovision_pack_count(void);
const char *mariovision_pack_name(int index);
bool mariovision_pack_enabled(int index);
/// Whether its folder is still on disk.
///
/// DynOS keeps a pack in its list once it has seen it, and nothing takes it out
/// again - so a pack that has been deleted stays in the list, switched on,
/// pointing at nothing. This is how to tell.
bool mariovision_pack_exists(int index);
void mariovision_pack_set_enabled(int index, bool on);

int mariovision_level_count(void);
const char *mariovision_level_name(int index);

/// Jump there. Only meaningful while a game is running; before that there is
/// nothing to warp out of.
void mariovision_level_warp(int index);

#ifdef __cplusplus
}
#endif

#endif
