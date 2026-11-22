#pragma once

// Read-only developer modes that render the app offscreen, without a visible window.
//
//   --capture <dir> [--page <id>] [--theme light|dark|both] [--size WxH] [--scale f]
//                   [--no-steam | --fixture <fixture-dir>] [--state <name>] [--warp]
//       Writes one PNG per page and theme into <dir> and a capture.log with one line per file.
//       File name: <page>-<theme>[-<W>x<H>][@<scale>][-nosteam][-fx][-state-<name>].png, each
//       part present only when its option was given, for example cleanup-dark.png,
//       cleanup-dark-760x520.png, cleanup-dark-fx-state-running.png. A recoverable Dear ImGui or
//       Facet error during a capture is logged and makes the exit code 1.
//   --selftest [--size WxH] [--scale f] [--no-steam | --fixture <fixture-dir>] [--warp] [--log <file>]
//       Checks the chrome (no status bar: the sidebar runs to the bottom; one flat sidebar list
//       with Settings pinned at the bottom), clicks every sidebar item (after each: the page's
//       background jobs finish and no Dear ImGui or Facet error was reported), runs a task-runner
//       job and checks that the title bar activity indicator shows it with its progress and goes
//       away afterwards, checks every column scroll against Facet's padding and bar gutter and
//       (with the fixture) that the Configs detail column ends on the card content's right edge with
//       nothing in its bar strip, at the start size and at 760 x 520 where it scrolls, opens and
//       closes the command palette (Ctrl+K and Escape, and its title bar
//       button), switches the theme through the palette and checks the narrow rail. Only
//       navigation: nothing that cleans, restores, deletes, installs or registers is pressed.
//
//   --page id        cleanup, backups, exclusions, audit, configs, settings
//   --no-steam       Steam detection reports no install (the empty states)
//   --fixture dir    creates or reuses the synthetic install and app data under dir (see
//                    app/fixture.hpp) and points AppState at it instead of the registry and the
//                    user's profile. A non-empty dir without the fixture's marker is refused.
//   --state name     sets AppState::dev_state for the pages (lowercase letters, digits, '-'):
//                    "running", "result-ok", "result-failed", "confirm", "error" and whatever a
//                    page documents. The shell reads one: "shell-job" shows a fabricated
//                    "Cleaning 92 of 165" in the title bar activity indicator (no job runs).
//                    Backups: loading, error, empty, confirm-restore, confirm-restore-steam,
//                    confirm-delete, running, result-ok, result-failed, menu
//
// Both modes: AppState in read-only and offline mode (no settings, ignore list, ini or placement
// written, no update check), logging into the output directory (capture) or nowhere (selftest),
// exit code 0 on success. Only the fixture generator writes, and only under the fixture dir.
// Lines go to stdout (the parent console is attached when there is one) and to the log file.

namespace stc::app {

// True when the command line asks for a dev mode; then *exit_code is the result of running it.
bool run_dev_mode(const wchar_t* command_line, int* exit_code);

}  // namespace stc::app
