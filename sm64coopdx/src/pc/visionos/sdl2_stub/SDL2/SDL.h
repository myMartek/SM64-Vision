/*
 * <SDL2/SDL.h> for the visionOS build.
 *
 * visionOS has no SDL2 port (only SDL3 supports it), and migrating coopdx to
 * SDL3 would be more work than replacing the handful of things it actually
 * uses. The desktop backends (gfx_sdl, gfx_opengl, audio_sdl, controller_sdl,
 * controller_mouse) are excluded from the build in Makefile.visionos; what is
 * left are a few timing, path, display and keyboard-name calls, which
 * visionos_sdl_compat.h implements natively.
 *
 * This file exists so that the `#include <SDL2/SDL.h>` lines in the sources we
 * do keep resolve here instead - no #ifdef in game code required. The stub
 * include directory is placed ahead of the system paths in Makefile.visionos.
 */

#ifndef MARIOVISION_SDL_H
#define MARIOVISION_SDL_H

#include "visionos_sdl_compat.h"

#endif // MARIOVISION_SDL_H
