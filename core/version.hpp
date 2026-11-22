#pragma once

namespace stc::core {

inline constexpr const char* kAppVersion = "2.0.0";
inline constexpr const char* kAppVersionTag = "v2.0.0";

// "owner/repo" on GitHub: the release check asks the GitHub API for this repository. The
// repository's earlier name, luminary-cloud/steam-tracer-cleaner, redirects here, which is how
// releases up to 1.1.2 still find their updates.
inline constexpr const char* kRepoSlug = "velyndev/steam-tracer-cleaner";

}  // namespace stc::core
