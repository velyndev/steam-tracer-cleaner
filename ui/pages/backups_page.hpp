#pragma once

namespace stc::app {
struct AppState;
}

namespace stc::ui::pages {

// The Backups page. Contract: see the comment at the top of ui/app_shell.hpp.
void draw_backups_page(stc::app::AppState& state);

}  // namespace stc::ui::pages
