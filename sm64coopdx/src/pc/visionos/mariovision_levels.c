//-----------------------------------------------------------------------------
// A list of places to jump to, for trying things out.
//
// The game's own level table is keyed by its Japanese development names -
// "TERESA OBAKE", "SABAKU % PYRMD" - which are no use in a menu. This is a
// plain table of the ones worth visiting, in the order one meets them, with the
// area and act each needs to land somewhere sensible rather than in a wall.
//
// Deliberately hand-written and deliberately incomplete: the point is to reach
// a fight or a water level in a couple of seconds while tuning a gesture, not
// to be a level editor.
//-----------------------------------------------------------------------------

#include "mariovision_levels.h"

#include "sm64.h"
#include "level_table.h"
#include "pc/lua/utils/smlua_level_utils.h"
#include "data/dynos.c.h"
#include "pc/configfile.h"

#include <stdatomic.h>

typedef struct {
    const char *name;
    s32 level;
    s32 area;
    s32 act;
} Place;

static const Place sPlaces[] = {
    { "Castle Grounds",            LEVEL_CASTLE_GROUNDS, 1, 1 },
    { "Inside the Castle",         LEVEL_CASTLE,         1, 1 },
    { "Castle Basement",           LEVEL_CASTLE_COURTYARD, 1, 1 },

    { "Bob-omb Battlefield",       LEVEL_BOB,   1, 1 },
    { "Whomp's Fortress",          LEVEL_WF,    1, 1 },
    { "Jolly Roger Bay",           LEVEL_JRB,   1, 1 },
    { "Cool, Cool Mountain",       LEVEL_CCM,   1, 1 },
    { "Big Boo's Haunt",           LEVEL_BBH,   1, 1 },
    { "Hazy Maze Cave",            LEVEL_HMC,   1, 1 },
    { "Lethal Lava Land",          LEVEL_LLL,   1, 1 },
    { "Shifting Sand Land",        LEVEL_SSL,   1, 1 },
    { "Dire, Dire Docks",          LEVEL_DDD,   1, 1 },
    { "Snowman's Land",            LEVEL_SL,    1, 1 },
    { "Wet-Dry World",             LEVEL_WDW,   1, 1 },
    { "Tall, Tall Mountain",       LEVEL_TTM,   1, 1 },
    { "Tiny-Huge Island",          LEVEL_THI,   1, 1 },
    { "Tick Tock Clock",           LEVEL_TTC,   1, 1 },
    { "Rainbow Ride",              LEVEL_RR,    1, 1 },

    { "Bowser in the Dark World",  LEVEL_BOWSER_1, 1, 1 },
    { "Bowser in the Fire Sea",    LEVEL_BOWSER_2, 1, 1 },
    { "Bowser in the Sky",         LEVEL_BOWSER_3, 1, 1 },

    { "Tower of the Wing Cap",     LEVEL_TOTWC,  1, 1 },
    { "Vanish Cap Under the Moat", LEVEL_VCUTM,  1, 1 },
    { "Cavern of the Metal Cap",   LEVEL_COTMC,  1, 1 },
    { "Wing Mario Over the Rainbow", LEVEL_WMOTR, 1, 1 },
    { "The Secret Aquarium",       LEVEL_SA,     1, 1 },
    { "The Princess's Secret Slide", LEVEL_PSS,  1, 1 },
};

#define PLACE_COUNT ((int) (sizeof sPlaces / sizeof sPlaces[0]))

static _Atomic bool sRescanAsked = false;
static _Atomic bool sRescanBusy = false;

//-----------------------------------------------------------------------------
// Turning packs on and off
//
// The wish is written down here and carried out on the game thread, for two
// reasons. It touches the tables the renderer reads, which is no business of a
// button in SwiftUI; and switching a texture pack on is only half the job.
//
// The other half is the renderer's own texture cache. It is keyed by the
// address a texture came from, and DynOS works by handing out a different one
// for that same address. A texture already uploaded therefore stays uploaded,
// and the pack looked inert until the app was closed and everything loaded
// afresh - which is exactly what was reported. Emptying the cache is what makes
// it take hold on the spot.
//
// The config is written afterwards as well, so that the choice survives even
// when the app is killed rather than closed.
//-----------------------------------------------------------------------------

#define MV_PACK_MAX 64

/// -1 nothing asked for, 0 off, 1 on.
static _Atomic signed char sPackWant[MV_PACK_MAX];
static _Atomic bool sPackWantAny = false;
static _Atomic bool sPackApplyAsked = false;

/// Nothing asked for yet, for every slot.
__attribute__((constructor)) static void mv_pack_want_init(void) {
    for (int i = 0; i < MV_PACK_MAX; i++) { atomic_store(&sPackWant[i], -1); }
}

void mariovision_request_pack_rescan(void) {
    atomic_store(&sRescanAsked, true);
}

bool mariovision_pack_rescanning(void) {
    return atomic_load(&sRescanAsked) || atomic_load(&sRescanBusy);
}

void mariovision_pack_rescan_if_asked(void) {
    if (atomic_exchange(&sRescanAsked, false)) {
        atomic_store(&sRescanBusy, true);
        dynos_gfx_init();
        atomic_store(&sRescanBusy, false);
        // A rescan renumbers nothing but may add packs, and anything asked for
        // before it happened still wants carrying out.
        if (atomic_load(&sPackWantAny)) { atomic_store(&sPackApplyAsked, true); }
    }

    if (!atomic_exchange(&sPackApplyAsked, false)) { return; }

    extern void gfx_texture_cache_clear(void);
    bool changed = false;
    const int count = dynos_pack_get_count();
    for (int i = 0; i < count && i < MV_PACK_MAX; i++) {
        const signed char want = atomic_load(&sPackWant[i]);
        if (want < 0) { continue; }
        if (dynos_pack_get_enabled(i) == (want != 0)) { continue; }
        dynos_pack_set_enabled(i, want != 0);
        changed = true;
    }
    if (changed) {
        gfx_texture_cache_clear();
        configfile_save(configfile_name());
    }
}

int mariovision_pack_count(void) {
    return dynos_pack_get_count();
}

const char *mariovision_pack_name(int index) {
    const char *name = dynos_pack_get_name(index);
    return name ? name : "";
}

bool mariovision_pack_enabled(int index) {
    // What was asked for, if anything, so a switch does not spring back while
    // the game thread is still on its way to it.
    if (index >= 0 && index < MV_PACK_MAX) {
        const signed char want = atomic_load(&sPackWant[index]);
        if (want >= 0) { return want != 0; }
    }
    return dynos_pack_get_enabled(index);
}

bool mariovision_pack_exists(int index) {
    return dynos_pack_get_exists(index);
}

void mariovision_pack_set_enabled(int index, bool on) {
    if (index < 0 || index >= MV_PACK_MAX) { return; }
    atomic_store(&sPackWant[index], on ? 1 : 0);
    atomic_store(&sPackWantAny, true);
    atomic_store(&sPackApplyAsked, true);
}

int mariovision_level_count(void) {
    return PLACE_COUNT;
}

const char *mariovision_level_name(int index) {
    if (index < 0 || index >= PLACE_COUNT) { return ""; }
    return sPlaces[index].name;
}

void mariovision_level_warp(int index) {
    if (index < 0 || index >= PLACE_COUNT) { return; }
    const Place *p = &sPlaces[index];
    warp_to_level(p->level, p->area, p->act);
}
