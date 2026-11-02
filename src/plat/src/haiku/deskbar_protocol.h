// Between MSGA (haiku_app.cpp, HaikuTray) and its Deskbar add-on
// (deskbar_tray.cpp, which runs in Deskbar's team).
#pragma once

#include <SupportDefs.h>

namespace plat::haiku::deskbar {

// The Deskbar item's (and its view's) name.
inline constexpr const char *kItemName = "MSGA";
// MSGA's application signature (msga.rdef).
inline constexpr const char *kSignature = "application/x-vnd.airos-msga";

enum : uint32 {
    kHello     = 'mTHi', // item → MSGA: "item" messenger; MSGA → item: "msga" messenger
    kIcon      = 'mTIc', // MSGA → item: "width", "height" (int32) and "pixels" (B_RAW_TYPE,
                         // straight-alpha B_RGBA32 rows) per size
    kTooltip   = 'mTTt', // MSGA → item: "text"
    kMenu      = 'mTMn', // MSGA → item: "item" messages ("kind", "id", "label",
                         // "enabled", "checked", nested "item"s for a submenu)
    kGoodbye   = 'mTBy', // MSGA → item: MSGA's tray is going
    kActivated = 'mTAc', // item → MSGA: a primary click
    kMenuItem  = 'mTMi', // item → MSGA: a menu choice, "id" (uint32)
};

enum : int32 { kKindAction, kKindCheckbox, kKindSeparator, kKindSubmenu };

} // namespace plat::haiku::deskbar
