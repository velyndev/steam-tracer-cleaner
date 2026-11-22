# Contributing

Pull requests are welcome. Steam Tracer Cleaner is a native Windows app in C++23, built with
CMake. The UI is drawn with [Facet](https://github.com/velyndev/imgui-facet), a design framework
on top of Dear ImGui.

This guide goes from a first build to a release, in that order.

## Prerequisites

- Visual Studio 2026 (18.x), or its Build Tools, with the **Desktop development with C++**
  workload (MSVC v145).
- CMake 3.28 or newer and Ninja. Both ship with that workload.
- Network access on the first configure. CMake downloads Facet at a pinned commit, and Facet
  downloads Dear ImGui, FreeType and lunasvg. Everything else is vendored under `third_party/`.

CMake is the only build. The `steam-tracer-cleaner.sln` and `.vcxproj` files are still in the
repository but no longer build.

## Build and test

### Ninja, from an x64 developer prompt

```powershell
cmake --preset release
cmake --build --preset release
```

The exe lands at `build/release/bin/steam-tracer-cleaner.exe`. The `debug` preset builds into
`build/debug` the same way.

The build also compiles the unit tests in `tests/` (doctest). Run them with:

```powershell
ctest --preset debug     # or: ctest --preset release
```

### Visual Studio 2026 (what CI uses)

```powershell
cmake --preset vs2026
cmake --build --preset vs2026-release
ctest --preset vs2026-release
```

The exe lands at `build/vs2026/bin/Release/steam-tracer-cleaner.exe`. Visual Studio can also open
the folder as a CMake project and offers the same presets.

### Build notes

- The project is C++23 (`/std:c++latest` for C++ files only, so the SQLite C amalgamation still
  builds).
- The CRT is linked statically, Facet and its dependencies included, so the exe runs without the
  Visual C++ redistributable.
- In-source builds are refused. Configure into `build/` (the presets do).
- `STC_BUILD_TESTS` (on by default) controls whether `tests/` is built.

## Looking at the UI without clicking through it

The app asks for administrator rights at start. For your own test runs, set
`__COMPAT_LAYER=RunAsInvoker` in the shell to skip the prompt.

The exe has two developer modes that render offscreen, without a visible window. Both are
read-only: nothing is written to the settings or data folders, no update check runs, and nothing
that cleans, restores, deletes, installs or registers is ever pressed.

```powershell
$env:__COMPAT_LAYER = "RunAsInvoker"
$exe = "build\release\bin\steam-tracer-cleaner.exe"

& $exe --capture out\shots --theme both                 # one PNG per page and theme
& $exe --capture out\narrow --size 760x520 --no-steam   # smallest window, "Steam not found" states
& $exe --capture out\fx --fixture out\fixture           # pages filled with generated sample data
& $exe --capture out\fx --fixture out\fixture --page cleanup --state running
& $exe --selftest --fixture out\fixture                 # scripted navigation checks
```

| Option | What it does |
|---|---|
| `--capture <dir>` | Writes one PNG per page and theme into `<dir>`, plus `capture.log`. |
| `--selftest` | Clicks every sidebar item, checks the layout rules, opens and closes the command palette, switches the theme. Exit code 0 means it passed. |
| `--page <id>` | Only one page: `cleanup`, `backups`, `exclusions`, `audit`, `configs`, `settings`. |
| `--theme light\|dark\|both` | Which theme to capture. |
| `--size WxH`, `--scale f` | Window size and interface scale. |
| `--no-steam` | Pretends no Steam install was found. |
| `--fixture <dir>` | Generates a sample Steam install with three sample accounts, backups and a config library under `<dir>` and points the app at it, so the pages have data on a PC without Steam. A non-empty folder that is not a fixture is refused. |
| `--state <name>` | Asks a page for a state that would otherwise need a destructive or slow action, such as `running`, `result-ok`, `result-failed`, `confirm` or `error`. |

The full option list and the state names are in the comment at the top of `app/capture.hpp`.
Pages list their own capture-only states in their code (for the Cleanup page, in
`ui/pages/cleanup_dev_states.cpp`), and `app/fixture.hpp` describes the sample install.

The screenshots in `assets/` are made this way, with `--fixture`.

## Project layout

```
app/          WinMain, D3D11 window and message loop, AppState, the background task runner,
              the capture and selftest modes, the sample fixture
core/         cleaning logic: the 34 cleanup items (targets.cpp), profiles (profile.cpp),
              plan building, the cleaner, backups, exclusions (ignore_list), audit, configs,
              VDF parsing, browser cookies, the sign-in task, the update check, the version
platform/     thin Win32 wrappers: registry, filesystem, processes, known folders
ui/           the Facet app shell (title bar, sidebar, command palette, update dialog),
              appearance, shared page helpers (common.hpp)
ui/pages/     one file set per page: cleanup, backups, exclusions, audit, configs, settings
tests/        doctest unit tests for core
third_party/  vendored libraries and the Facet download
assets/       app icon (SVG and .ico) and screenshots; app.rc compiles the icons into the exe
tools/        make_icon.py and the small SVG rasterizer it uses
cmake/        shared compile options
```

`core/` and `platform/` do not depend on the UI, so the cleaning rules can be unit tested
without a window.

## How a page is built

Each page is one function, `void draw_<name>_page(stc::app::AppState&)` in `stc::ui::pages`,
called once per frame while the page is selected. Two comments are the reference; read them
before you change or add a page:

- **The page contract** at the top of `ui/app_shell.hpp`: what the shell has already set up
  (padding, no scrollbar of its own), what a page may assume, the minimum width it must work at
  (540 design units, and at UI scale 1.5), and how navigation, toasts and dialogs work.
- **The layout rule** in `ui/common.hpp`: one column that fills the content width, no width cap,
  and the same edges, cards and gaps on every page.

In practice a page looks like this:

```cpp
void draw_example_page(stc::app::AppState& state) {
    stc::ui::page_header("Example", "One sentence about what this page is for.");
    if (!state.install) {
        stc::ui::steam_not_found_state(state);
        return;
    }
    if (stc::ui::begin_page_body("example-body")) {
        stc::ui::begin_page_card("first", "Card title", "Optional one-line description.");
        // ... widgets ...
        stc::ui::end_page_card();
        stc::ui::page_block_gap();
        stc::ui::begin_page_card("second", "Another card");
        // ...
        stc::ui::end_page_card();
        stc::ui::end_page_body();
    }
}
```

- Start with `page_header` (or `begin_page_header` / `end_page_header` when the header has
  buttons on the right).
- Put the scrolling part in `begin_page_body` / `end_page_body`. Do not use a bare
  `facet::BeginScrollArea`; a scrolling pane inside a card is `begin_column_scroll`.
- Group content in `begin_page_card` / `end_page_card`, and separate blocks with
  `page_block_gap()`. Spacing comes from Facet's theme tokens, not from literals in the page.
- Anything slow (walking folders, deleting, network) goes through `state.tasks`
  (`app/task_runner.hpp`). The worker gets copies of what it needs and never touches
  `AppState`; the completion callback runs on the UI thread and is the only place that writes
  the result back. The shell shows a job that runs longer than a moment in the title bar.
- A page keeps its own UI state (selection, open dialogs) in `AppState` fields or
  function-local statics.

A new page also needs an entry in the `Page` enum (`app/app_state.hpp`), in `kPages` and
`draw_page` in `ui/app_shell.cpp`, and in the command palette there.

## Adding or renaming files

Source files are listed by hand. When you add, remove or rename one, update the
`CMakeLists.txt` of its directory (`ui/CMakeLists.txt`, `core/CMakeLists.txt`,
`tests/CMakeLists.txt`, and so on). The `.vcxproj` files are not part of the build.

## Working with Facet

### Using a local Facet checkout

To build against a local clone of Facet instead of the download, add this to the configure
command:

```powershell
cmake --preset debug -DFETCHCONTENT_SOURCE_DIR_FACET=C:/path/to/imgui-facet
```

Or put that cache variable into a `CMakeUserPresets.json`, which git ignores. Keep the clone at
the commit pinned in `third_party/CMakeLists.txt`, otherwise your build is not the one CI makes.

### Moving to a new Facet commit

1. Change `GIT_TAG` of `FetchContent_Declare(facet ...)` in `third_party/CMakeLists.txt` to the
   new commit hash. Always pin a full hash, not a branch.
2. Configure and build. The next configure checks out the new commit. If a build folder still
   uses the old one, delete that preset's folder under `build/` and configure again.
3. Run the unit tests, `--selftest` (with and without `--fixture`) and a `--capture` of every
   page in both themes, and look at the pictures.
4. If the Facet update brings new versions of Dear ImGui, FreeType or lunasvg, update the table
   in `THIRD_PARTY_NOTICES.md`.
5. Note the change in `CHANGELOG.md`.

## Regenerating the app icon

The icon is drawn as SVG: `assets/icon.svg` for 32 px and up, and the simpler
`assets/icon-small.svg` for 16 to 24 px. `tools/make_icon.py` renders every frame from them and
writes `assets/icon.ico`. It needs Python 3 (standard library only) and a small rasterizer,
`tools/icon/svg2rgba.exe`, built from the lunasvg sources Facet already downloaded. Build it once
from a developer prompt (after any configure, here the `release` preset):

```powershell
cmake -S tools/icon -B build/icon-tool -G Ninja -DCMAKE_BUILD_TYPE=Release -DLUNASVG_SOURCE_DIR=build/release/_deps/facet_lunasvg-src
cmake --build build/icon-tool
```

Then, from the repository root:

```powershell
python tools/make_icon.py                   # writes assets/icon.ico
python tools/make_icon.py --sheet out.png   # also writes a contact sheet of every frame
```

Pass `--renderer <path>` if the rasterizer is somewhere else. `app.rc` embeds the `.ico` and both
SVGs, but the build does not notice when they change: touch `app.rc` afterwards so the resources
are compiled again.

## Style and quality

- Formatting follows `.clang-format` and `.editorconfig` at the repository root.
- `.clang-tidy` enables `bugprone-*`, `cert-*`, `cppcoreguidelines-*` and `modernize-*`.
- Keep comments short and factual, and match the code around them.
- Text in the UI is plain and specific: say what happens, in sentence case, with the file or
  registry name after the plain words when it helps.
- CI (`.github/workflows/ci.yml`) configures with the `vs2026` preset, builds Debug and Release
  x64 and runs the unit tests in both on every push and pull request to `main`. Please make sure
  it passes.

## Making a release

1. Set the new version in all four places:
   - `CMakeLists.txt`: `project(... VERSION X.Y.Z ...)`
   - `core/version.hpp`: `kAppVersion` and `kAppVersionTag`
   - `app.rc`: `FILEVERSION`, `PRODUCTVERSION` and the two version strings
   - `app.manifest`: the `version` of the first `assemblyIdentity`
2. Add a `## X.Y.Z (YYYY-MM-DD)` section at the top of `CHANGELOG.md`. The release workflow
   uses that section as the release text and fails if it is missing.
3. Commit and push to `main`, and wait for CI to pass.
4. Tag the commit and push the tag:

   ```powershell
   git tag vX.Y.Z
   git push origin vX.Y.Z
   ```

The tag starts `.github/workflows/release.yml`. It builds and tests the Release configuration,
then publishes a GitHub release with `steam-tracer-cleaner.exe`, a
`steam-tracer-cleaner-vX.Y.Z-x64.zip` (the exe, `LICENSE.txt`, `THIRD_PARTY_NOTICES.md`,
`CHANGELOG.md` and `README.md`) and a `.sha256` file for each of the two.

The update check in the app asks GitHub for the latest release of the repository named by
`kRepoSlug` in `core/version.hpp`, so a published release is what installed copies are told
about.

## Libraries

Downloaded at configure time:

| Library | License | Used for |
|---|---|---|
| [Facet](https://github.com/velyndev/imgui-facet) | MIT | UI framework (widgets, theme, scaling) |
| [Dear ImGui](https://github.com/ocornut/imgui) (through Facet) | MIT | UI |
| [FreeType](https://freetype.org/) (through Facet) | FTL | Font rasterization |
| [lunasvg](https://github.com/sammycage/lunasvg) (through Facet) | MIT | SVG icons |

Vendored under `third_party/`:

| Library | License | Used for |
|---|---|---|
| [spdlog](https://github.com/gabime/spdlog) | MIT | Logging (header-only) |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | Settings, exclusions and backup manifests |
| [SQLite amalgamation](https://www.sqlite.org/) | Public domain | Browser cookie databases |
| [doctest](https://github.com/doctest/doctest) | MIT | Unit tests |

Versions and the full list of what ends up in the exe are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
