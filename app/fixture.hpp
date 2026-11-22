#pragma once

// The synthetic Steam install and app data of the dev modes' --fixture <dir>.
//
// ensure_fixture() creates (or completes) this tree under <dir>; every file is written only when
// it is missing, so a second run reuses the tree and a run that was cut off completes it.
//
//   <dir>/stc-fixture.txt          marker; a non-empty <dir> without it is refused
//   <dir>/Steam                    the install (InstallInfo::install_path)
//     config/                      loginusers.vdf (3 accounts), libraryfolders.vdf (names
//                                  <dir>/Library2), config.vdf (Accounts + ConnectCache),
//                                  remoteclients.vdf, coplay_*.vdf, avatarcache/*.png,
//                                  htmlcache/, overlayhtmlcache/
//     userdata/<account_id>/       config/{localconfig.vdf, shortcuts.vdf, librarycache/,
//                                  controller_configs/}, 7/remote/sharedconfig.vdf,
//                                  730/local/cfg, 760/remote (screenshots, not for B),
//                                  gamerecordings/, ugcmsgcache/, inventorymsgcache/
//     appcache/                    httpcache/, librarycache/, stats/, appinfo.vdf, packageinfo.vdf
//     logs/, dumps/, depotcache/, tenfoot/config/httpcache/, ssfn*, *.last, GameOverlay*.log, *.dmp
//     steamapps/                   shadercache/730, downloading/570, workshop/{temp,downloads},
//                                  appmanifest_*.acf, common/Counter-Strike Global Offensive/game/csgo/cfg,
//                                  common/Steam Controller Configs
//   <dir>/Library2/steamapps       second library: shadercache/440, appmanifest_440.acf,
//                                  common/Team Fortress 2/tf/cfg; <dir>/Library2/depotcache
//   <dir>/LocalSteam               InstallInfo::htmlcache_dir's parent: htmlcache/, GPUCache/,
//                                  Code Cache/, Local State, BrowsingTopicsState, local.vdf
//   <dir>/app/config               config_dir: ignore.json (keeps account B and appcache/stats)
//   <dir>/app/data                 data_dir: backups/ (3 restore points in core/backup.cpp's
//                                  format), configs/autoexec (2 .cfg), configs/video
//                                  (cs2_video.txt), logs/
//
// Accounts (all synthetic): persona "Sample account A/B/C", login "sample_a/b/c", account ids
// 900000101/102/103, SteamID64 76561198860265829/830/831. A is the most recent login.

#include <filesystem>
#include <string>

namespace stc::app {

// Creates or completes the fixture. Returns false with a reason in *error when <dir> is not
// empty and has no marker, or when a file cannot be written. Writes nothing outside <dir>.
bool ensure_fixture(const std::filesystem::path& dir, std::string* error, int* files_written = nullptr);

}  // namespace stc::app
