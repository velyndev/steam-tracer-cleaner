#pragma once

// Images and icons the app adds to Facet's built-in set.
//
//   github()         the GitHub mark, registered as a Facet icon from SVG text (tinted like the
//                    built-in icons), for links to the repository.
//   app_icon_slot()  an empty icon that reserves the icon place in the title bar; the shell draws
//                    the full-color app icon over it with draw_app_icon(). Facet's title bar takes
//                    a tinted Icon only, and the app icon has two colors.
//   draw_app_icon()  the app icon from its SVG drawings (assets/icon-small.svg up to 24 px,
//                    assets/icon.svg above), embedded as RCDATA by app.rc and rasterized by Facet
//                    at the exact pixel size, so the title bar shows the vector at its own size
//                    instead of an .ico frame resampled by Windows (the title bar wants 13 px at
//                    scale 1 and 20 px at 1.5, which are not .ico frame sizes). Facet re-rasterizes
//                    it after a device loss; it is re-created when the pixel size changes.

#include <imgui.h>

#include <facet/core/icons.h>

namespace stc::ui::assets {

// After facet::Init (also again after a new device).
void load();
// Before facet::Shutdown: image handles do not survive it.
void release();

facet::Icon github();
facet::Icon app_icon_slot();

// Draws the app icon with its top-left corner at `pos`, `size` pixels square.
void draw_app_icon(ImDrawList* draw_list, ImVec2 pos, float size);

}  // namespace stc::ui::assets
