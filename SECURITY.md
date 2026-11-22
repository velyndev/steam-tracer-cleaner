# Security Policy

## Reporting a vulnerability

Please report security issues privately. **Do not open a public issue for a
vulnerability.**

Email **cloud@luminary.pub** with:

- a description of the issue and its impact,
- steps to reproduce (or a proof of concept),
- the app version and your Windows version. The version is shown in Settings, under
  **About** and under **Updates**, and in the release tag you downloaded.

You can expect an acknowledgement within a few days. Please give a reasonable window
to ship a fix before any public disclosure.

## Supported versions

Only the latest release on the
[Releases](https://github.com/velyndev/steam-tracer-cleaner/releases) page receives fixes.

## How destructive actions are protected

A cleanup deletes files and registry values, so it is gated in these ways:

- **Live plan.** The Cleanup page measures what the selected profile and items would remove, in
  the background, every time the selection or the exclusions change. The summary card shows the
  total size and number of items, and **Review items** lists every file, folder and registry
  value. The **Clean** button runs exactly that plan.
- **Confirmation.** A dialog asks first when **Back up first** is off, and before a Full Wipe
  while the "Confirm before a Full Wipe" setting is on (the default).
- **Back up first.** With this switch on (the default, set in Settings), each item is copied
  into a new timestamped backup folder right before it is deleted: files and folders as copies,
  registry keys and values as `reg.exe` exports, edited `.vdf` files as their old text. If the
  backup folder cannot be created, the cleanup stops and nothing is removed. Once the folder
  exists, an item that cannot be copied into it is still removed, so check the log if a backup
  looks incomplete.
- **Restore.** The Backups page puts a backup's files and registry values back where they were.
  Older backups beyond the number set in Settings (10 by default, 0 keeps all) are deleted after
  each cleanup that makes a new one.
- **Exclusions.** Accounts, files, folders and registry entries on the Exclusions page are
  skipped by every profile. A kept account keeps its sign-in entry, its `userdata` folder, its
  registry entries, its controller layouts and its cached avatar.
- **Steam is closed first.** Before a cleanup, Steam and running Valve and Source games are
  closed, because Steam rewrites its `.vdf` files when it exits. Installing a config closes only
  the target game; Steam stays open.
- **Action log.** A rotating log records each cleanup step with a timestamp, the item and the
  path, and whether it succeeded. It is in the `logs` folder of the data folder (see below), and
  Settings and the command palette open it.

### Quick Clean at sign-in

If you turn on "Run Quick Clean when I sign in", a Task Scheduler task runs
`steam-tracer-cleaner.exe --scheduled` at each Windows sign-in. That run has no window and no
confirmation. It applies your exclusions and always makes a backup first: if the backup folder
cannot be created, it removes nothing. Afterwards it deletes old backups beyond the "Backups to
keep" number. Unlike a cleanup you start yourself, it does not close Steam first. Quick Clean
only removes caches, logs and crash reports.

## Data handling

- **Local only.** Settings and exclusions are stored in `%APPDATA%\steam-tracer-cleaner`.
  Backups, saved configs and the log are in `%LOCALAPPDATA%\steam-tracer-cleaner`. In portable
  mode (a `portable.flag` file next to the exe) all of them, the log included, are in a `data`
  folder beside the exe.
- **Backups contain what was removed.** That can include saved sign-in tokens and, when Steam
  cookies are cleared, a copy of the whole browser cookie file. Backups never leave your PC.
  Delete the ones you no longer need from the Backups page.
- **Browser cookies.** To sign you out of the Steam website, the cleanup opens the cookie
  databases of Chrome, Edge, Brave, Vivaldi and Firefox and deletes only the rows for
  `steamcommunity.com` and `steampowered.com` hosts. It does not read cookie values. The Audit
  page lists browser profiles (it also looks for Opera) without opening their cookie files.
- **No telemetry.** The only outbound connection is the update check, a request to
  `api.github.com` for the latest release of this repository. It runs at startup unless you
  turn it off in Settings, and when you press **Check now**. Nothing else leaves the machine.

## Scope

- **Not a spoofer.** The Audit page reads the computer name, MachineGuid and MachineId to
  display them. Nothing in the app changes them.
- **No anti-cheat bypass.** Nothing here defeats VAC, Easy Anti-Cheat, BattlEye or any other
  anti-cheat. This is a file cleaner.
- **Administrator elevation.** The app always runs elevated. Steam usually lives under
  `C:\Program Files (x86)`, part of what it registers (the `steam://` link handler) is
  machine-wide, and closing Steam's processes can need it too.
- **No code signing.** Release binaries are unsigned, so SmartScreen warns on first run. Each
  release has a `.sha256` file for the exe and for the zip; compare it with
  `Get-FileHash <file> -Algorithm SHA256` to confirm the download.
