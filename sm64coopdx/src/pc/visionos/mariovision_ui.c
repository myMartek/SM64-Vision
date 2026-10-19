/*
 * Bridge between the app's native visionOS interface and coopdx.
 *
 * coopdx drives its startup flow - language, host settings, mod list - through
 * its own djui menus, drawn inside the game's framebuffer. Those work, but they
 * are a 2D game menu floating in a spatial OS. This lets SwiftUI take over that
 * part while the game keeps its own in-game UI (pause, chat, Lua HUD).
 *
 * Threading: SwiftUI runs on the main thread, the game on its own. Everything
 * here that touches game state is therefore queued and executed from
 * mariovision_ui_pump(), which the game thread calls once per iteration.
 * Only the plain reads are safe to call directly.
 */

#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mariovision_platform.h"
#include "mariovision_levels.h"
#include "pc/lua/utils/smlua_misc_utils.h"

#include "game/save_file.h"
#include "pc/configfile.h"
#include "pc/pc_main.h"
#include "pc/platform.h"
#include "pc/djui/djui.h"
#include "pc/djui/djui_language.h"
#include "pc/lua/smlua_hooks.h"

extern void djui_panel_do_host(bool reconnecting, bool playSound);

//-----------------------------------------------------------------------------
// The switch
//-----------------------------------------------------------------------------

// Change this one line to fall back to coopdx's own menus, or build with
// -DMARIOVISION_NATIVE_UI_DEFAULT=0. Also flippable at runtime through
// mariovision_set_native_ui_enabled().
#ifndef MARIOVISION_NATIVE_UI_DEFAULT
#define MARIOVISION_NATIVE_UI_DEFAULT 1
#endif

static bool sNativeUI = MARIOVISION_NATIVE_UI_DEFAULT;

bool mariovision_native_ui_enabled(void) {
    return sNativeUI;
}

void mariovision_set_native_ui_enabled(bool enabled) {
    sNativeUI = enabled;
}

//-----------------------------------------------------------------------------
// State the UI reads
//-----------------------------------------------------------------------------

bool mariovision_game_ready(void) {
    // Loaded, and sitting in the main menu rather than already playing - which
    // is exactly when the native Start button has something to do.
    return gGameInited && gDjuiInMainMenu;
}

bool mariovision_game_running(void) {
    return gGameInited && !gDjuiInMainMenu;
}

//-----------------------------------------------------------------------------
// Languages
//
// The set is whatever lang/*.ini the bundle ships. The display name comes from
// the LANGUAGE section of the currently loaded language file, so the list reads
// in the user's own language; djui_language_get hands back the key it was given
// when there is no translation for it.
//-----------------------------------------------------------------------------

#define MAX_LANGUAGES 32
#define LANG_KEY_LEN 64
#define LANG_NAME_LEN 96

static char sLangKey[MAX_LANGUAGES][LANG_KEY_LEN];
static char sLangName[MAX_LANGUAGES][LANG_NAME_LEN];
static int sLangCount = -1;

// The app reads this list from the main thread while the game thread can be
// rewriting it in execute_apply_language, so both sides take this.
static pthread_mutex_t sLangMutex = PTHREAD_MUTEX_INITIALIZER;

static void scan_languages(void) {
    sLangCount = 0;

    char path[SYS_MAX_PATH] = "";
    snprintf(path, SYS_MAX_PATH, "%s/lang", sys_resource_path());

    DIR *d = opendir(path);
    if (!d) { return; }

    struct dirent *entry = NULL;
    while ((entry = readdir(d)) != NULL && sLangCount < MAX_LANGUAGES) {
        char key[LANG_KEY_LEN] = "";
        snprintf(key, sizeof(key), "%s", entry->d_name);

        // Strip the extension; skip dotfiles and anything without one.
        char *dot = strchr(key, '.');
        if (dot == NULL || dot == key) { continue; }
        *dot = '\0';
        if (key[0] == '\0') { continue; }

        snprintf(sLangKey[sLangCount], LANG_KEY_LEN, "%s", key);

        char *display = djui_language_get("LANGUAGE", key);
        snprintf(sLangName[sLangCount], LANG_NAME_LEN, "%s", display ? display : key);
        sLangCount++;
    }
    closedir(d);

    // readdir order is arbitrary; a stable list is nicer to look at.
    for (int i = 1; i < sLangCount; i++) {
        for (int j = i; j > 0 && strcmp(sLangKey[j - 1], sLangKey[j]) > 0; j--) {
            char tmpKey[LANG_KEY_LEN], tmpName[LANG_NAME_LEN];
            memcpy(tmpKey, sLangKey[j], LANG_KEY_LEN);
            memcpy(tmpName, sLangName[j], LANG_NAME_LEN);
            memcpy(sLangKey[j], sLangKey[j - 1], LANG_KEY_LEN);
            memcpy(sLangName[j], sLangName[j - 1], LANG_NAME_LEN);
            memcpy(sLangKey[j - 1], tmpKey, LANG_KEY_LEN);
            memcpy(sLangName[j - 1], tmpName, LANG_NAME_LEN);
        }
    }
}

/// Caller must hold sLangMutex.
static void ensure_languages_locked(void) {
    if (sLangCount < 0) { scan_languages(); }
}

int mariovision_language_count(void) {
    pthread_mutex_lock(&sLangMutex);
    ensure_languages_locked();
    const int count = sLangCount;
    pthread_mutex_unlock(&sLangMutex);
    return count;
}

// The strings are copied out rather than returned by pointer: the list can be
// rebuilt on the game thread the moment the lock is dropped.
void mariovision_language_key(int index, char *out, int outSize) {
    if (!out || outSize <= 0) { return; }
    out[0] = '\0';

    pthread_mutex_lock(&sLangMutex);
    ensure_languages_locked();
    if (index >= 0 && index < sLangCount) {
        snprintf(out, outSize, "%s", sLangKey[index]);
    }
    pthread_mutex_unlock(&sLangMutex);
}

void mariovision_language_display(int index, char *out, int outSize) {
    if (!out || outSize <= 0) { return; }
    out[0] = '\0';

    pthread_mutex_lock(&sLangMutex);
    ensure_languages_locked();
    if (index >= 0 && index < sLangCount) {
        snprintf(out, outSize, "%s", sLangName[index]);
    }
    pthread_mutex_unlock(&sLangMutex);
}

int mariovision_language_current(void) {
    int result = 0;

    pthread_mutex_lock(&sLangMutex);
    ensure_languages_locked();
    for (int i = 0; i < sLangCount; i++) {
        if (strcmp(sLangKey[i], configLanguage) == 0) { result = i; goto done; }
    }
    // An empty configLanguage means English, which is also what
    // djui_language_init falls back to.
    for (int i = 0; i < sLangCount; i++) {
        if (strcmp(sLangKey[i], "English") == 0) { result = i; goto done; }
    }
done:
    pthread_mutex_unlock(&sLangMutex);
    return result;
}

//-----------------------------------------------------------------------------
// Commands, executed on the game thread
//-----------------------------------------------------------------------------

enum {
    CMD_START_GAME     = 1 << 0,
    CMD_APPLY_LANGUAGE = 1 << 1,
};

static _Atomic uint32_t sPendingCommands = 0;
static _Atomic int sPendingLanguage = -1;

// UI requests are consumed on the game thread. Slots are 1...4.
static _Atomic int sStartSaveRequest = 0;
static _Atomic int sEraseSaveRequest = 0;
static _Atomic bool sSaveBusy = false;
// -1 means the engine has not published a snapshot yet.
static _Atomic int sSaveStars[4] = { -1, -1, -1, -1 };
static _Atomic bool sSaveExists[4] = { false, false, false, false };

int mariovision_save_stars(int slot) {
    return slot >= 1 && slot <= 4 ? atomic_load(&sSaveStars[slot - 1]) : -1;
}

bool mariovision_save_exists(int slot) {
    return slot >= 1 && slot <= 4 && atomic_load(&sSaveExists[slot - 1]);
}

bool mariovision_save_busy(void) {
    return atomic_load(&sSaveBusy);
}

void mariovision_start_save(int slot) {
    if (slot < 1 || slot > 4) { return; }
    atomic_store(&sStartSaveRequest, slot);
    atomic_fetch_or(&sPendingCommands, CMD_START_GAME);
}

void mariovision_erase_save(int slot) {
    if (slot < 1 || slot > 4 || atomic_exchange(&sSaveBusy, true)) { return; }
    atomic_store(&sEraseSaveRequest, slot);
}

static void publish_save_slots(void) {
    if (!gGameInited) { return; }
    for (int i = 0; i < 4; ++i) {
        atomic_store(&sSaveExists[i], save_file_exists(i) != 0);
        atomic_store(&sSaveStars[i], save_file_get_total_star_count(i, 0, 24));
    }
}

void mariovision_start_game(void) {
    atomic_fetch_or(&sPendingCommands, CMD_START_GAME);
}

void mariovision_language_select(int index) {
    atomic_store(&sPendingLanguage, index);
    atomic_fetch_or(&sPendingCommands, CMD_APPLY_LANGUAGE);
}

static void execute_start_game(void) {
    if (!mariovision_game_ready()) { return; }

    const int slot = atomic_exchange(&sStartSaveRequest, 0);
    if (slot != 0) {
        if (slot < 1 || slot > 4) { return; }
        configHostSaveSlot = (unsigned int)slot;
    }

    // The intro cutscene is skipped unconditionally: with the native UI the
    // Start button is the whole main menu, and coopdx' own "skip intro" option
    // lives in the menu we just replaced. network_init copies this into
    // gServerSettings.skipIntro, which is what level_update.c actually reads,
    // so it has to be set before do_host. A new file must not play the intro:
    // the first-person camera does not run that cutscene.
    configSkipIntro = true;

    djui_panel_do_host(false, false);
}

static void execute_apply_language(void) {
    pthread_mutex_lock(&sLangMutex);
    ensure_languages_locked();

    const int index = atomic_load(&sPendingLanguage);
    if (index < 0 || index >= sLangCount || strcmp(sLangKey[index], configLanguage) == 0) {
        pthread_mutex_unlock(&sLangMutex);
        return;
    }

    snprintf(configLanguage, MAX_CONFIG_STRING, "%s", sLangKey[index]);
    if (!djui_language_init(configLanguage)) {
        // Falls back to English inside djui_language_init; clear the setting so
        // the config does not keep pointing at a file that would not load.
        configLanguage[0] = '\0';
    }

    // The display names come from the language file that just changed.
    scan_languages();
    pthread_mutex_unlock(&sLangMutex);

    smlua_call_event_hooks(HOOK_ON_LANGUAGE_CHANGED, configLanguage);
    configfile_save(configfile_name());
}

void mariovision_ui_pump(void) {
    // A scan of thousands of files, on the game thread, where it belongs.
    //
    // Here and not in the first-person update, which is where it sat: that runs
    // only while a game is being played in VR. Downloading a pack from the main
    // menu therefore asked for a scan that nobody was listening for, and the
    // list stayed empty however long one waited. This pump runs every frame,
    // menu or no menu, which is the whole of what was wanted.
    mariovision_pack_rescan_if_asked();

    // Hide coopdx's startup menus - that is the part the native UI replaces.
    // In-game djui (pause, chat, player list, Lua HUD) is left alone, so the
    // flag is only held down while the main menu is up.
    //
    // Tracked rather than assigned unconditionally so that turning the native
    // UI off restores djui exactly once, and so this never fights the crash
    // handler, which sets the same flag.
    static bool sForcedOff = false;
    if (sNativeUI && gDjuiInMainMenu) {
        gDjuiDisabled = true;
        sForcedOff = true;
    } else if (sForcedOff) {
        gDjuiDisabled = false;
        sForcedOff = false;
    }

    const int eraseSlot = atomic_exchange(&sEraseSaveRequest, 0);
    if (eraseSlot != 0) {
        // Never erase a running session's data, even if a stale UI asks.
        if (mariovision_game_ready() && eraseSlot >= 1 && eraseSlot <= 4) {
            save_file_erase(eraseSlot - 1);
        }
        publish_save_slots();
        atomic_store(&sSaveBusy, false);
    }
    publish_save_slots();

    const uint32_t pending = atomic_exchange(&sPendingCommands, 0);
    if (pending & CMD_APPLY_LANGUAGE) { execute_apply_language(); }
    if (pending & CMD_START_GAME)     { execute_start_game(); }
}


//-----------------------------------------------------------------------------
// Menu request
//-----------------------------------------------------------------------------

static _Atomic bool sMenuRequested = false;

void mariovision_request_menu(void) {
    atomic_store(&sMenuRequested, true);
}

bool mariovision_menu_key_allowed(void) {
    // R is a game button - crouching, the camera - and on a pad it is the right
    // shoulder, which gets pressed constantly while playing. Bringing the
    // launcher up on every one of those is intolerable, so the shortcut only
    // counts once the game is already paused. Start still opens it directly, so
    // there is always a way back regardless.
    return is_game_paused();
}

bool mariovision_take_menu_request(void) {
    return atomic_exchange(&sMenuRequested, false);
}
