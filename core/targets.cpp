#include "core/targets.hpp"

#include <algorithm>
#include <vector>

#include "core/browser_cookies.hpp"
#include "core/crash_dumps.hpp"
#include "core/steam_paths.hpp"

namespace stc::core {
namespace {

namespace fs_std = std::filesystem;

std::wstring path_to_w(const fs_std::path& p) { return p.wstring(); }

void push_dir(std::vector<Operation>& out, const fs_std::path& p) {
    Operation op;
    op.kind = OpKind::RemoveTree;
    op.target = path_to_w(p);
    out.push_back(std::move(op));
}

void push_file(std::vector<Operation>& out, const fs_std::path& p) {
    Operation op;
    op.kind = OpKind::RemoveFile;
    op.target = path_to_w(p);
    out.push_back(std::move(op));
}

void push_glob_files(std::vector<Operation>& out, const fs_std::path& dir, std::wstring_view prefix,
                     std::wstring_view suffix = L"") {
    std::error_code ec;
    if (!fs_std::is_directory(dir, ec)) {
        return;
    }
    for (auto it = fs_std::directory_iterator(dir, ec); !ec && it != fs_std::end(it);
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            continue;
        }
        auto name = it->path().filename().wstring();
        bool prefix_ok = prefix.empty() || name.starts_with(prefix);
        bool suffix_ok = suffix.empty() || name.ends_with(suffix);
        if (prefix_ok && suffix_ok) {
            push_file(out, it->path());
        }
    }
}

void push_reg_value(std::vector<Operation>& out, std::wstring_view full_key, std::wstring_view value) {
    Operation op;
    op.kind = OpKind::RemoveRegistryValue;
    op.target = std::wstring{full_key};
    op.value_name = std::wstring{value};
    out.push_back(std::move(op));
}

void push_reg_key(std::vector<Operation>& out, std::wstring_view full_key,
                  std::wstring_view account = L"") {
    Operation op;
    op.kind = OpKind::RemoveRegistryKey;
    op.target = std::wstring{full_key};
    op.account_steamid64 = std::wstring{account};
    out.push_back(std::move(op));
}

std::vector<Operation> resolve_dumps(const ResolveContext& ctx) {
    // Originally went through push_glob_files for the *.dmp sweep but the suffix-only matching
    // also caught e.g. ".dmpbackup" on one machine, hand-rolling it here.
    std::vector<Operation> ops;

    Operation dump_dir;
    dump_dir.kind = OpKind::RemoveTree;
    dump_dir.target = (ctx.install.install_path / "dumps").wstring();
    ops.push_back(std::move(dump_dir));

    std::error_code ec;
    if (fs_std::is_directory(ctx.install.install_path, ec)) {
        for (auto it = fs_std::directory_iterator(ctx.install.install_path, ec);
             !ec && it != fs_std::end(it); it.increment(ec)) {
            if (!it->is_regular_file(ec)) {
                continue;
            }
            const auto name = it->path().filename().wstring();
            if (name.size() < 4) {
                continue;
            }
            const auto suffix = name.substr(name.size() - 4);
            if (suffix == L".dmp") {
                Operation op;
                op.kind = OpKind::RemoveFile;
                op.target = it->path().wstring();
                ops.push_back(std::move(op));
            }
        }
    }
    return ops;
}

std::vector<Operation> resolve_logs(const ResolveContext& ctx) {
    std::vector<Operation> ops;
    push_dir(ops, ctx.install.install_path / "logs");
    push_glob_files(ops, ctx.install.install_path, L"GameOverlayUI.exe.log");
    push_glob_files(ops, ctx.install.install_path, L"GameOverlayRenderer", L".log");
    push_glob_files(ops, ctx.install.install_path, L"", L".last");
    return ops;
}

std::vector<Operation> resolve_appcache(const ResolveContext& ctx) {
    std::vector<Operation> ops;
    push_dir(ops, ctx.install.appcache_dir / "httpcache");
    push_dir(ops, ctx.install.appcache_dir / "librarycache");
    push_file(ops, ctx.install.appcache_dir / "appinfo.vdf");
    push_file(ops, ctx.install.appcache_dir / "packageinfo.vdf");
    return ops;
}

std::vector<Operation> resolve_depotcache(const ResolveContext& ctx) {
    std::vector<Operation> ops;
    for (const auto& lib : ctx.libraries) {
        push_dir(ops, lib.parent_path() / "depotcache");
    }
    return ops;
}

std::vector<Operation> resolve_shadercache(const ResolveContext& ctx) {
    std::vector<Operation> ops;
    for (const auto& lib : ctx.libraries) {
        std::error_code ec;
        auto sc = lib / "shadercache";
        if (fs_std::is_directory(sc, ec)) {
            push_dir(ops, sc);
        }
    }
    return ops;
}

std::vector<Operation> resolve_workshop_temp(const ResolveContext& ctx) {
    std::vector<Operation> ops;
    for (const auto& lib : ctx.libraries) {
        push_dir(ops, lib / "workshop" / "temp");
        push_dir(ops, lib / "workshop" / "downloads");
    }
    return ops;
}

std::vector<Operation> resolve_htmlcache(const ResolveContext& ctx) {
    std::vector<Operation> ops;
    push_dir(ops, ctx.install.htmlcache_dir);
    push_dir(ops, ctx.install.config_dir / "htmlcache");
    auto steam_local = ctx.install.htmlcache_dir.parent_path();
    push_dir(ops, steam_local / "GPUCache");
    push_dir(ops, steam_local / "Code Cache");
    push_file(ops, steam_local / "Local State");
    push_file(ops, steam_local / "BrowsingTopicsState");
    return ops;
}

std::vector<Operation> resolve_loginusers_vdf(const ResolveContext& ctx) {
    // The planner replaces this file-delete with VdfRemoveChild ops for non-preserved accounts
    // when the ignore list has any preserved ids.
    std::vector<Operation> ops;
    push_file(ops, ctx.install.config_dir / "loginusers.vdf");
    return ops;
}

std::vector<Operation> resolve_ssfn_files(const ResolveContext& ctx) {
    std::vector<Operation> ops;
    push_glob_files(ops, ctx.install.install_path, L"ssfn");
    return ops;
}

std::vector<Operation> resolve_auto_login_value(const ResolveContext&) {
    std::vector<Operation> ops;
    push_reg_value(ops, L"HKCU\\Software\\Valve\\Steam", L"AutoLoginUser");
    push_reg_value(ops, L"HKCU\\Software\\Valve\\Steam", L"RememberPassword");
    push_reg_value(ops, L"HKCU\\Software\\Valve\\Steam", L"LastGameNameUsed");
    push_reg_value(ops, L"HKCU\\Software\\Valve\\Steam", L"PseudoUUID");
    return ops;
}

std::vector<Operation> resolve_users_subtree(const ResolveContext& ctx) {
    // Per-account: the cleaner filters this via the ignore list before deleting.
    std::vector<Operation> ops;
    for (const auto& acc : ctx.accounts) {
        std::wstring full = L"HKCU\\Software\\Valve\\Steam\\Users\\" + acc.steamid64;
        push_reg_key(ops, full, acc.steamid64);
    }
    return ops;
}

std::vector<Operation> resolve_active_process(const ResolveContext&) {
    std::vector<Operation> ops;
    push_reg_key(ops, L"HKCU\\Software\\Valve\\Steam\\ActiveProcess");
    return ops;
}

std::vector<Operation> resolve_remote_clients(const ResolveContext& ctx) {
    std::vector<Operation> ops;
    push_file(ops, ctx.install.config_dir / "remoteclients.vdf");
    push_glob_files(ops, ctx.install.config_dir, L"coplay_");
    return ops;
}

std::vector<Operation> resolve_avatarcache(const ResolveContext& ctx) {
    // Per-file so each <SteamID64>.png can be tagged with its owning account; the ignore filter
    // then skips avatars belonging to a preserved id. Filenames that don't parse as a SteamID64
    // fall through with an empty account_steamid64 and are removed.
    std::vector<Operation> ops;
    auto root = ctx.install.config_dir / "avatarcache";
    std::error_code ec;
    if (!fs_std::is_directory(root, ec)) {
        return ops;
    }
    for (auto it = fs_std::directory_iterator(root, ec); !ec && it != fs_std::end(it);
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            continue;
        }
        Operation op;
        op.kind = OpKind::RemoveFile;
        op.target = it->path().wstring();

        auto stem = it->path().stem().wstring();
        const bool all_digits =
            !stem.empty() &&
            std::all_of(stem.begin(), stem.end(),
                        [](wchar_t c) { return c >= L'0' && c <= L'9'; });
        if (all_digits && stc::core::steam::steamid64_to_account_id(stem) != 0) {
            op.account_steamid64 = stem;
        }
        ops.push_back(std::move(op));
    }
    return ops;
}

std::vector<Operation> resolve_userdata_full(const ResolveContext& ctx) {
    // Per-account, ignore list filters by account_steamid64 before delete.
    std::vector<Operation> ops;
    for (const auto& acc : ctx.accounts) {
        Operation op;
        op.kind = OpKind::RemoveTree;
        op.target = path_to_w(acc.userdata_path);
        op.account_steamid64 = acc.steamid64;
        ops.push_back(std::move(op));
    }
    return ops;
}

std::vector<Operation> resolve_browser_steam_cookies(const ResolveContext&) {
    std::vector<Operation> ops;
    for (const auto& db : stc::core::browser::discover_cookie_dbs()) {
        Operation op;
        op.kind = OpKind::ClearBrowserSteamCookies;
        op.target = db.db_path.wstring();
        op.value_name = db.schema == stc::core::browser::Schema::Firefox ? L"firefox" : L"chromium";
        ops.push_back(std::move(op));
    }
    return ops;
}

std::vector<Operation> resolve_crash_local_appdata(const ResolveContext&) {
    std::vector<Operation> ops;
    for (const auto& d : stc::core::crashdumps::find_local_appdata_dumps()) {
        Operation op;
        op.kind = OpKind::RemoveFile;
        op.target = d.path.wstring();
        ops.push_back(std::move(op));
    }
    return ops;
}

std::vector<Operation> resolve_controller_configs(const ResolveContext& ctx) {
    std::vector<Operation> ops;
    for (const auto& acc : ctx.accounts) {
        auto base = acc.userdata_path / "config" / "controller_configs";
        Operation op;
        op.kind = OpKind::RemoveTree;
        op.target = path_to_w(base);
        op.account_steamid64 = acc.steamid64;
        ops.push_back(std::move(op));
    }
    push_dir(ops, ctx.install.install_path / "steamapps" / "common" / "Steam Controller Configs");
    return ops;
}

std::vector<Operation> resolve_config_vdf(const ResolveContext& ctx) {
    std::vector<Operation> ops;
    push_file(ops, ctx.install.config_dir / "config.vdf");
    return ops;
}

std::vector<Operation> resolve_local_vdf(const ResolveContext& ctx) {
    std::vector<Operation> ops;
    push_file(ops, ctx.install.local_vdf_path);
    return ops;
}

std::vector<Operation> resolve_userdata_gamerecordings(const ResolveContext& ctx) {
    std::vector<Operation> ops;
    for (const auto& acc : ctx.accounts) {
        auto target = path_to_w(acc.userdata_path / "gamerecordings");
        ops.push_back(Operation{OpKind::RemoveTree, target, L"", acc.steamid64, 0});
    }
    return ops;
}

std::vector<Operation> resolve_userdata_ugcmsgcache(const ResolveContext& ctx) {
    std::vector<Operation> ops;
    for (const auto& acc : ctx.accounts) {
        auto target = path_to_w(acc.userdata_path / "ugcmsgcache");
        ops.push_back(Operation{OpKind::RemoveTree, target, L"", acc.steamid64, 0});
    }
    return ops;
}

std::vector<Operation> resolve_appcache_stats(const ResolveContext& ctx) {
    std::vector<Operation> ops;
    push_dir(ops, ctx.install.appcache_dir / "stats");
    return ops;
}

std::vector<Operation> resolve_tenfoot_httpcache(const ResolveContext& ctx) {
    std::vector<Operation> ops;
    push_dir(ops, ctx.install.install_path / "tenfoot" / "config" / "httpcache");
    return ops;
}

std::vector<Operation> resolve_overlayhtmlcache(const ResolveContext& ctx) {
    std::vector<Operation> ops;
    push_dir(ops, ctx.install.config_dir / "overlayhtmlcache");
    return ops;
}

std::vector<Operation> resolve_steamapps_downloading(const ResolveContext& ctx) {
    std::vector<Operation> ops;
    for (const auto& lib : ctx.libraries) {
        push_dir(ops, lib / "downloading");
    }
    return ops;
}

std::vector<Operation> resolve_reg_apps(const ResolveContext&) {
    std::vector<Operation> ops;
    push_reg_key(ops, L"HKCU\\Software\\Valve\\Steam\\Apps");
    return ops;
}

std::vector<Operation> resolve_reg_url_handler(const ResolveContext&) {
    std::vector<Operation> ops;
    push_reg_key(ops, L"HKCR\\steam");
    push_reg_key(ops, L"HKCU\\Software\\Classes\\steam");
    return ops;
}

std::vector<Operation> resolve_userdata_shortcuts(const ResolveContext& ctx) {
    std::vector<Operation> ops;
    for (const auto& acc : ctx.accounts) {
        Operation op;
        op.kind = OpKind::RemoveFile;
        op.target = path_to_w(acc.userdata_path / "config" / "shortcuts.vdf");
        op.account_steamid64 = acc.steamid64;
        ops.push_back(std::move(op));
    }
    return ops;
}

std::vector<Operation> resolve_userdata_screenshots(const ResolveContext& ctx) {
    std::vector<Operation> ops;
    const std::size_t n = ctx.accounts.size();
    for (std::size_t i = 0; i < n; ++i) {
        const auto& acc = ctx.accounts[i];
        if (acc.userdata_path.empty()) {
            // Should never happen, enumerate_accounts skips empty paths, but I ran into a
            // crash here once when userdata enumeration ran before the install was fully
            // detected.
            continue;
        }
        Operation op;
        op.kind = OpKind::RemoveTree;
        op.target = path_to_w(acc.userdata_path / "760" / "remote");
        op.account_steamid64 = acc.steamid64;
        ops.push_back(std::move(op));
    }
    return ops;
}

std::vector<Operation> resolve_userdata_inventory_cache(const ResolveContext& ctx) {
    std::vector<Operation> ops;
    for (auto a : ctx.accounts) {
        auto target = path_to_w(a.userdata_path / "inventorymsgcache");
        ops.push_back(Operation{OpKind::RemoveTree, target, L"", a.steamid64, 0});
    }
    return ops;
}

std::vector<Operation> resolve_userdata_librarycache(const ResolveContext& ctx) {
    std::vector<Operation> ops;
    for (const auto& acc : ctx.accounts) {
        auto target = acc.userdata_path / "config" / "librarycache";
        std::error_code ec;
        if (!fs_std::is_directory(target, ec)) {
            continue;
        }
        Operation op;
        op.kind = OpKind::RemoveTree;
        op.target = path_to_w(target);
        op.account_steamid64 = acc.steamid64;
        ops.push_back(std::move(op));
    }
    return ops;
}

std::vector<Operation> resolve_userdata_sharedconfig(const ResolveContext& ctx) {
    std::vector<Operation> ops;
    for (const auto& acc : ctx.accounts) {
        Operation op;
        op.kind = OpKind::RemoveFile;
        op.target = path_to_w(acc.userdata_path / "7" / "remote" / "sharedconfig.vdf");
        op.account_steamid64 = acc.steamid64;
        ops.push_back(std::move(op));
    }
    return ops;
}

std::vector<Operation> resolve_appmanifests(const ResolveContext& ctx) {
    std::vector<Operation> ops;
    for (const auto& lib : ctx.libraries) {
        push_glob_files(ops, lib, L"appmanifest_", L".acf");
    }
    return ops;
}

const std::vector<Target>& catalog() {
    static const std::vector<Target> targets = [] {
        std::vector<Target> t;
        t.push_back(Target{"steam.dumps", L"Steam crash reports",
                           L"Files Steam writes when it crashes. Nothing you use is lost. "
                           L"The dumps folder and .dmp files in the Steam folder.",
                           TargetCategory::CrashDump, &resolve_dumps});
        t.push_back(Target{"steam.logs", L"Steam logs",
                           L"Records of what Steam did. Steam starts new ones. "
                           L"The logs folder, overlay logs and .last files in the Steam folder.",
                           TargetCategory::Log, &resolve_logs});
        t.push_back(Target{"steam.appcache", L"Store and library data",
                           L"Steam downloads it again, so the library looks bare for a moment after "
                           L"the next start. appcache: httpcache, librarycache, appinfo.vdf, packageinfo.vdf.",
                           TargetCategory::Cache, &resolve_appcache});
        t.push_back(Target{"steam.depotcache", L"Download manifests",
                           L"Lists of downloaded game files, which also show which games this PC "
                           L"had. Steam gets them again when it updates a game. The depotcache folder of each library.",
                           TargetCategory::Cache, &resolve_depotcache});
        t.push_back(Target{"steam.shadercache", L"Shader cache",
                           L"Graphics data games prepare on their first start. Games build it again, "
                           L"so that first start can stutter. steamapps\\shadercache in each library.",
                           TargetCategory::Cache, &resolve_shadercache});
        t.push_back(Target{"steam.workshop_temp", L"Unfinished Workshop downloads",
                           L"Workshop items that were still downloading. Steam downloads them again. "
                           L"steamapps\\workshop\\temp and downloads in each library.",
                           TargetCategory::Cache, &resolve_workshop_temp});
        t.push_back(Target{"steam.htmlcache", L"Steam web cache",
                           L"Pages, cookies and sessions saved by Steam's built-in browser. Steam builds "
                           L"it again. htmlcache, GPUCache, Code Cache and Local State under %LOCALAPPDATA%\\Steam.",
                           TargetCategory::BrowserResidue, &resolve_htmlcache});
        t.push_back(Target{"steam.loginusers", L"Remembered accounts",
                           L"The accounts on Steam's sign-in screen. They no longer show there; "
                           L"excluded accounts stay. config\\loginusers.vdf.",
                           TargetCategory::AccountResidue, &resolve_loginusers_vdf});
        t.push_back(Target{"steam.ssfn", L"Old Steam Guard files",
                           L"Files older Steam versions used to skip the Steam Guard code. Current "
                           L"Steam no longer makes them. ssfn files in the Steam folder.",
                           TargetCategory::AccountResidue, &resolve_ssfn_files});
        t.push_back(Target{"steam.reg.autologin", L"Sign-in registry values",
                           L"Which account Steam signs in to on its own, so Steam asks you to sign in "
                           L"again. With an excluded account, Steam signs in to that one instead. "
                           L"AutoLoginUser, RememberPassword, LastGameNameUsed and PseudoUUID under "
                           L"HKCU\\Software\\Valve\\Steam.",
                           TargetCategory::AccountResidue, &resolve_auto_login_value});
        t.push_back(Target{"steam.reg.users", L"Per-account registry keys",
                           L"Settings older Steam versions kept for each account. Excluded accounts "
                           L"are skipped. HKCU\\Software\\Valve\\Steam\\Users.",
                           TargetCategory::AccountResidue, &resolve_users_subtree});
        t.push_back(Target{"steam.reg.activeprocess", L"Running Steam notes",
                           L"Registry notes about the running Steam process. Steam writes them again "
                           L"when it starts. HKCU\\Software\\Valve\\Steam\\ActiveProcess.",
                           TargetCategory::AccountResidue, &resolve_active_process});
        t.push_back(Target{"steam.remoteclients", L"Streaming devices and co-play",
                           L"Devices Steam paired for streaming and the co-play files of recent "
                           L"sessions. Skipped while any account is excluded. config\\remoteclients.vdf "
                           L"and coplay files.",
                           TargetCategory::AccountResidue, &resolve_remote_clients});
        t.push_back(Target{"steam.avatarcache", L"Profile pictures",
                           L"Avatars Steam saved for you and your friends. Steam downloads them again; "
                           L"those of excluded accounts stay. config\\avatarcache.",
                           TargetCategory::Cache, &resolve_avatarcache});
        t.push_back(Target{"steam.userdata", L"Game saves and settings",
                           L"Each account's local saves, game settings and Steam settings. Only saves "
                           L"kept in Steam Cloud can come back. Excluded accounts are kept. userdata\\<id>.",
                           TargetCategory::GameData, &resolve_userdata_full});
        t.push_back(Target{"steam.controller_configs", L"Steam Input layouts",
                           L"Your controller button layouts. Layouts without a Steam Cloud copy are "
                           L"lost. userdata\\<id>\\config\\controller_configs and Steam Controller Configs.",
                           TargetCategory::ControllerResidue, &resolve_controller_configs});
        t.push_back(Target{"browser.steam_cookies", L"Steam cookies in your browsers",
                           L"Signs you out of the Steam store and community websites in Chrome, Edge, "
                           L"Brave, Vivaldi and Firefox. Close those browsers first.",
                           TargetCategory::BrowserResidue, &resolve_browser_steam_cookies});
        t.push_back(Target{"crash.local_appdata", L"Game crash reports",
                           L"Crash files of Steam, SteamVR and Valve games such as Counter-Strike 2 and "
                           L"Dota 2. Nothing you use is lost. %LOCALAPPDATA%\\CrashDumps.",
                           TargetCategory::CrashDump, &resolve_crash_local_appdata});
        t.push_back(Target{"steam.config_vdf", L"Saved sign-in tokens",
                           L"Keep accounts signed in, so Steam asks for the password again. With an "
                           L"excluded account only the others' entries go; otherwise Steam makes the "
                           L"file anew. config\\config.vdf.",
                           TargetCategory::AccountResidue, &resolve_config_vdf});
        t.push_back(Target{"steam.appcache_stats", L"Game stats cache",
                           L"Achievement and stats data saved for each game. Steam downloads it again. "
                           L"appcache\\stats.",
                           TargetCategory::Cache, &resolve_appcache_stats});
        t.push_back(Target{"steam.tenfoot_httpcache", L"Big Picture web cache",
                           L"Web cache of Big Picture mode. Steam builds it again. "
                           L"tenfoot\\config\\httpcache.",
                           TargetCategory::BrowserResidue, &resolve_tenfoot_httpcache});
        t.push_back(Target{"steam.overlayhtmlcache", L"Overlay web cache",
                           L"Web cache of the browser in the in-game overlay. Steam builds it again. "
                           L"config\\overlayhtmlcache.",
                           TargetCategory::BrowserResidue, &resolve_overlayhtmlcache});
        t.push_back(Target{"steam.steamapps_downloading", L"Unfinished game downloads",
                           L"Parts of installs and updates that did not finish. Steam downloads them "
                           L"again from the start. steamapps\\downloading in each library.",
                           TargetCategory::Cache, &resolve_steamapps_downloading});
        t.push_back(Target{"steam.reg.apps", L"Per-game registry entries",
                           L"What Steam noted for each game, such as language and install state. Steam "
                           L"adds entries again as needed. HKCU\\Software\\Valve\\Steam\\Apps.",
                           TargetCategory::AccountResidue, &resolve_reg_apps});
        t.push_back(Target{"steam.reg.url_handler", L"steam:// link handler",
                           L"Lets links on websites open Steam. Steam registers it again when it starts. "
                           L"HKCR\\steam and HKCU\\Software\\Classes\\steam.",
                           TargetCategory::AccountResidue, &resolve_reg_url_handler});
        t.push_back(Target{"steam.userdata_shortcuts", L"Non-Steam games in your library",
                           L"Programs you added to your Steam library yourself. They leave the library; "
                           L"excluded accounts are skipped. userdata\\<id>\\config\\shortcuts.vdf.",
                           TargetCategory::AccountResidue, &resolve_userdata_shortcuts});
        t.push_back(Target{"steam.userdata_screenshots", L"Screenshots",
                           L"Screenshots taken with Steam, for each account. They are deleted from this "
                           L"PC. userdata\\<id>\\760\\remote.",
                           TargetCategory::GameData, &resolve_userdata_screenshots});
        t.push_back(Target{"steam.userdata_inventory_cache", L"Inventory message cache",
                           L"Inventory notifications saved for each account. Steam gets them again. "
                           L"userdata\\<id>\\inventorymsgcache.",
                           TargetCategory::Cache, &resolve_userdata_inventory_cache});
        t.push_back(Target{"steam.userdata_librarycache", L"Account library images",
                           L"Library artwork and details saved for each account. Steam downloads them "
                           L"again. userdata\\<id>\\config\\librarycache.",
                           TargetCategory::Cache, &resolve_userdata_librarycache});
        t.push_back(Target{"steam.userdata_sharedconfig", L"Collections and hidden games",
                           L"Your library collections, hidden games and tags, for each account. They "
                           L"come back only from a Steam Cloud copy. userdata\\<id>\\7\\remote\\sharedconfig.vdf.",
                           TargetCategory::AccountResidue, &resolve_userdata_sharedconfig});
        t.push_back(Target{"steam.appmanifests", L"Installed game records",
                           L"The files that tell Steam a game is installed. Steam shows those games as "
                           L"not installed until you add them back. steamapps\\appmanifest_*.acf in each library.",
                           TargetCategory::GameData, &resolve_appmanifests});
        t.push_back(Target{"steam.local_vdf", L"Saved sign-in tokens (this PC)",
                           L"PC-wide tokens that keep accounts signed in, so Steam asks for the password "
                           L"again. With an excluded account only the others' entries go. "
                           L"%LOCALAPPDATA%\\Steam\\local.vdf.",
                           TargetCategory::AccountResidue, &resolve_local_vdf});
        t.push_back(Target{"steam.userdata_gamerecordings", L"Game recording data",
                           L"Steam Game Recording data for each account; excluded accounts are skipped. "
                           L"userdata\\<id>\\gamerecordings.",
                           TargetCategory::GameData, &resolve_userdata_gamerecordings});
        t.push_back(Target{"steam.userdata_ugcmsgcache", L"Workshop message cache",
                           L"Workshop notifications saved for each account. Steam gets them again. "
                           L"userdata\\<id>\\ugcmsgcache.",
                           TargetCategory::Cache, &resolve_userdata_ugcmsgcache});
        return t;
    }();
    return targets;
}

}  // namespace

std::span<const Target> all_targets() {
    const auto& c = catalog();
    return {c.data(), c.size()};
}

const Target* find_target(std::string_view id) {
    const auto& c = catalog();
    auto it = std::find_if(c.begin(), c.end(), [&](const Target& t) { return t.id == id; });
    return it == c.end() ? nullptr : &*it;
}

}  // namespace stc::core
