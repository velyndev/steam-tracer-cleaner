# Changelog

What changed in each release of Steam Tracer Cleaner, newest first. Releases before 1.1.1 are
described on the [Releases](https://github.com/velyndev/steam-tracer-cleaner/releases) page.

## 2.0.0 (2026-11-23)

The whole interface was rebuilt on [Facet](https://github.com/velyndev/imgui-facet). Every page
has a new layout, the app follows the Windows light or dark mode, and long operations no longer
freeze the window. The cleaning rules themselves are the ones from 1.1.2.

### New look

- A new app frame: custom title bar, a flat sidebar with Cleanup, Backups, Exclusions, Audit
  and Configs, and Settings at the bottom. The footer bar is gone; a running job shows in the
  title bar.
- Light and dark theme (System, Light or Dark), an accent color, an interface scale from 75 to
  200 % and an increased contrast option, all in Settings.
- Every page uses the same full-width column and the same cards, down to a 760 x 520 window,
  where the sidebar becomes an icon rail.
- A command palette (Ctrl+K) to jump to a page, re-detect Steam, open the backups or log folder
  and switch the theme.
- A new app icon.

### Cleanup

- Profiles are four cards that say what each one means for you, for example whether it signs
  accounts out or removes game saves.
- A summary card shows how much will be removed and holds the one "Clean" button and the
  "Back up first" switch. The plan behind it is measured in the background whenever you change
  something, so the separate "Dry run" button is gone.
- The 34 cleanup items are grouped into seven categories you can open, each with its share of
  the plan. Items have plain-language names and descriptions and can be searched.
- "Review items" lists every file, folder and registry value in the plan. From there you can
  copy a path, show it in Explorer, or exclude it from cleanups.
- Game Reset has a game picker in place of the free-text AppID box.
- A cleanup shows its progress and ends with a result you can read: what was removed, what
  failed and why.

### Backups, Exclusions, Audit, Configs

- Backups is a list of restore points with their age, a Restore button, and Show in Explorer
  and Delete in a menu. Restore and delete ask first and run in the background. Restore warns
  when Steam is running.
- Exclusions is a page of its own (it was the "Ignore list" inside Settings). Accounts found on
  the PC are switches; Steam IDs of other accounts and registry entries are validated as you
  type. The page says up front what is currently kept.
- Audit leads with a summary, shows each account with what Steam keeps for it, and lets you
  exclude an account from cleanups directly. Values can be copied; the report can be exported.
- Configs shows the library and a preview of the selected file side by side. Installing CS2
  video settings reports the result per account.

### Settings

- Settings are saved as you change them. The "Save settings" button is gone.
- A search field filters the settings.
- The Steam card shows whether Steam was found and offers "Re-detect Steam".

### Behaviour changes

- If "Back up first" is on and the backup cannot be created, the cleanup stops and removes
  nothing. Before, it cleaned without a backup.
- "Back up before cleaning" and "Confirm before a Full Wipe" now take effect. They were stored
  but never read. A cleanup without a backup always asks for confirmation.
- Quick Clean at sign-in follows the same backup rule: it removes nothing when its backup cannot
  be created, and it deletes old backups beyond the "Backups to keep" number.
- The update notice is a toast and a title bar indicator; it no longer opens a dialog at start.
- In portable mode the log is written beside the exe, next to the settings and backups. Before,
  it went to `%LOCALAPPDATA%` in every mode.
- A failed restore writes the files it could not put back to the log.
- The update check and the links in the app point to `velyndev/steam-tracer-cleaner`, the
  repository's current name.

### Fixes

- Plan building, cleaning, the audit report, the backup list, restore and config installs run
  in the background. The backup list is no longer re-read from disk on every frame.
- Closing the window during a cleanup or restore waits for it to finish.

### Build and development

- The build is CMake only (3.28 or newer, Visual Studio 2026). The Visual Studio solution is
  still in the repository but no longer builds. CI and releases use the `vs2026` preset and
  run the unit tests.
- The CMake build embeds the application manifest (administrator, per-monitor DPI awareness,
  UTF-8).
- Facet is fetched at a pinned commit and brings Dear ImGui 1.93, FreeType and lunasvg. The
  vendored Dear ImGui copy is removed.
- Read-only developer modes render the app offscreen: `--capture` writes a PNG per page,
  `--selftest` runs scripted checks, `--fixture` generates a sample Steam install.
- Releases include `THIRD_PARTY_NOTICES.md`.

## 1.1.2 (2026-07-22)

- Fixed ConnectCache key handling.

## 1.1.1 (2026-07-22)

- Added cleanup of `local.vdf`.
