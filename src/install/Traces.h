// WSA-Next © 2026 Marek Wesołowski - wesmar
// What WSA leaves in Windows outside its package registration. Removing the package takes its own
// registry and files with it, but WsaClient also writes, per user and on its own:
//   - a Start menu shortcut for every Android app (WsaClient.exe /launch wsa://<app>)
//   - an "Installed apps" entry for every Android app (HKCU ...\Uninstall\<app>), its
//     RegisteredApplications value and its file and link associations (HKCU\Software\Classes\wsa.*)
//   - notification settings of every Android app (<family>!<app>)
// and Windows keeps per-app records of its own (tiles, window sizes, privacy consent, the resource
// cache of the install folder, compatibility and "friendly name" caches of its executables).
// None of them is touched while a WSA package is still registered: they belong to it.
#pragma once
#include "support/Common.h"
#include <filesystem>

namespace WN::Install {

struct TraceReport {
    unsigned shortcuts = 0;  // .lnk files deleted
    unsigned keys = 0;       // registry keys deleted (with their subkeys)
    unsigned values = 0;     // registry values deleted
};

// Removes the per-user traces listed above. `folder` is the install folder (Windows caches entries
// under its path). Never fails as a whole: what cannot be removed is reported through `log`.
TraceReport RemoveTraces(const std::filesystem::path& folder, const LogFn& log);

// %LOCALAPPDATA%\Packages\<family>: the Android data disk (apps, accounts, files) and WSA's state.
std::filesystem::path PackageDataFolder();

// Total size of the files below `folder` (0 when it does not exist).
std::uint64_t FolderSize(const std::filesystem::path& folder);

// Deletes `path` (file or folder tree). Disks of a VM that has just stopped may stay locked for a
// few seconds, so locked files are retried for up to `seconds`.
Result<> DeleteWithRetry(const std::filesystem::path& path, int seconds = 20);

}  // namespace WN::Install
