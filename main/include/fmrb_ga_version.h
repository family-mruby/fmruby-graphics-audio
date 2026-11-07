#pragma once

// GA firmware version (this build)
// 2.1.0 / link 5: SET_SPRITE_CLIP (0x58) added, and note_on stopped dropping
// noise at period 0. Core checks both strictly, so an older Core refuses to
// pair with this build rather than running against a protocol it does not know.
// 3.0.0 / link 6: mute and volume at the last output stage (SET_MAIN_VOLUME
// and the per-APU mute/volume). Core 3.0.0 refuses a 2.1.0 GA, which would
// otherwise drop those commands silently.
#define FMRB_GA_FW_VERSION "3.0.0"

// Link protocol version (GA side's local value; must match Core's FMRB_LINK_VERSION in fmrb.h)
#define FMRB_LINK_VERSION 6
