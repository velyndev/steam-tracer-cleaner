#include "ui/assets.hpp"

#include <windows.h>

#include <facet/facet.h>

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstddef>
#include <string_view>

namespace stc::ui::assets {
namespace {

// RCDATA resources in app.rc: the app icon drawings as SVG text.
constexpr int kAppIconSvgResource = 102;       // assets/icon.svg, 32 px and up
constexpr int kAppIconSmallSvgResource = 103;  // assets/icon-small.svg, up to 24 px
constexpr int kSmallDrawingMaxPixels = 24;     // the same split as tools/make_icon.py

// The Octicons "mark-github" glyph (16 x 16), filled; Facet uses its shapes as a mask.
constexpr const char* kGithubSvg =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 16 16\" fill=\"currentColor\">"
    "<path d=\"M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49"
    "-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82"
    ".72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15"
    "-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82"
    " 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48"
    " 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0 0 16 8c0-4.42-3.58-8-8-8z\"/></svg>";

// No visible shape: the title bar reserves the icon's place, the app icon image is drawn there.
constexpr const char* kSlotSvg =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 16 16\">"
    "<rect x=\"0\" y=\"0\" width=\"16\" height=\"16\" fill=\"#000\" fill-opacity=\"0\"/></svg>";

facet::Icon g_github;
facet::Icon g_slot;
facet::ImageHandle g_app_icon;
int g_app_icon_size = 0;

// The bytes of an RCDATA resource of this module; empty when it is missing. Resource memory
// stays valid for the life of the process.
std::string_view resource_text(int id) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC info = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));  // RT_RCDATA
    if (info == nullptr) {
        return {};
    }
    HGLOBAL data = LoadResource(module, info);
    const void* bytes = data != nullptr ? LockResource(data) : nullptr;
    if (bytes == nullptr) {
        return {};
    }
    return {static_cast<const char*>(bytes), static_cast<std::size_t>(SizeofResource(module, info))};
}

}  // namespace

void load() {
    // Registrations survive facet::Shutdown; registering again replaces them.
    g_github = facet::RegisterIcon("stc-github", kGithubSvg);
    g_slot = facet::RegisterIcon("stc-app-slot", kSlotSvg);
    if (!g_github.IsValid() || !g_slot.IsValid()) {
        spdlog::warn("assets: an SVG icon was rejected");
    }
}

void release() {
    if (g_app_icon.IsValid()) {
        facet::ReleaseImage(g_app_icon);
    }
    g_app_icon = {};
    g_app_icon_size = 0;
}

facet::Icon github() { return g_github; }
facet::Icon app_icon_slot() { return g_slot; }

void draw_app_icon(ImDrawList* draw_list, ImVec2 pos, float size) {
    const int pixels = static_cast<int>(std::lround(size));
    if (pixels <= 0) {
        return;
    }
    if (!g_app_icon.IsValid() || pixels != g_app_icon_size) {
        release();
        const std::string_view svg =
            resource_text(pixels <= kSmallDrawingMaxPixels ? kAppIconSmallSvgResource : kAppIconSvgResource);
        if (!svg.empty()) {
            // Facet rasterizes at design size times Scale(); asking for pixels / Scale() gives
            // exactly `pixels`, rendered from the vector at that size.
            const float design = static_cast<float>(pixels) / facet::Scale();
            g_app_icon = facet::LoadSvgMemory(svg.data(), svg.size(), ImVec2(design, design));
        }
        if (!g_app_icon.IsValid()) {
            spdlog::warn("assets: the app icon SVG could not be loaded");
        }
        g_app_icon_size = pixels;  // also on failure: no retry every frame
    }
    if (g_app_icon.IsValid() && facet::IsImageReady(g_app_icon)) {
        const ImVec2 p(std::round(pos.x), std::round(pos.y));
        facet::DrawImage(draw_list, g_app_icon, p, ImVec2(p.x + static_cast<float>(pixels), p.y + static_cast<float>(pixels)));
    }
}

}  // namespace stc::ui::assets
