/*
 * Minimal stand-in for SDL2's SDL_stdinc.h.
 *
 * Only exists so that the vendored SDL_scancode.h / SDL_keycode.h enum headers
 * resolve their include and their few integer typedefs. visionOS does not link
 * against SDL at all - see visionos_sdl_compat.h.
 */

#ifndef MARIOVISION_SDL_STDINC_H
#define MARIOVISION_SDL_STDINC_H

#include <stdint.h>
#include <stddef.h>

typedef int8_t   Sint8;
typedef uint8_t  Uint8;
typedef int16_t  Sint16;
typedef uint16_t Uint16;
typedef int32_t  Sint32;
typedef uint32_t Uint32;
typedef int64_t  Sint64;
typedef uint64_t Uint64;

#endif // MARIOVISION_SDL_STDINC_H
