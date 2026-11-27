#pragma once

// GA firmware version (this build)
// 2.1.0 / link 5: SET_SPRITE_CLIP (0x58) added, and note_on stopped dropping
// noise at period 0. Core checks both strictly, so an older Core refuses to
// pair with this build rather than running against a protocol it does not know.
// 3.0.0 / link 6: mute and volume at the last output stage (SET_MAIN_VOLUME
// and the per-APU mute/volume). Core 3.0.0 refuses a 2.1.0 GA, which would
// otherwise drop those commands silently.
// 3.1.0 / link 7: DRAW_TILE_SCALED (scaled tile stamps), SPRITE_IMAGE_DATA
// (RGB332 pixels into a sprite image) and a PNG's transparent pixels read
// into a sprite image as 0x01 (fmruby-core doc/pictures). Numbered 3.2.0
// while 3.1.0 was being made and back to 3.1.0 for the release; link 7 tells
// a GA built before these commands (link 6) apart.
#define FMRB_GA_FW_VERSION "3.1.0"

// FMRB_LINK_VERSION: the one in fmruby-core (components/fmrb_wire), which
// both sides are built from.
#include "fmrb_link_version.h"
