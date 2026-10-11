/*
 * Replacements for desktop-only modules that are excluded from the visionOS
 * build in Makefile.visionos.
 */

#include <stdbool.h>

#include "pc/update_checker.h"

//-----------------------------------------------------------------------------
// Update checker (replaces src/pc/update_checker.c)
//
// The desktop build polls GitHub over libcurl and can swap its own binary out.
// Neither applies here: the app is sideloaded through Xcode and a visionOS app
// cannot replace its own bundle. Reporting "no update" keeps the menus that
// read gUpdateMessage working unchanged.
//-----------------------------------------------------------------------------

bool gUpdateMessage = false;

void check_for_updates(void) {
}

void show_update_popup(void) {
}
