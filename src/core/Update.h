// WSA-Next © 2026 Marek Wesołowski - wesmar
// Updates: what the release source offers beyond what runs and is installed, and WSA-Next's own
// update through WSA-Next Setup.
//
// Only the release manifest is read (about 14 KB with its catalog). The background host reads it at
// logon and when Windows reports a change of the network (host/Updates.h), at most once in
// kCheckInterval and only with Internet access; the window reads it whenever it refreshes. What was found is kept in the settings, so the
// notification-area icon shows its dot without going to the network itself. Nothing is downloaded
// without a click: WSA-Next Setup (about 1.5 MB) when the person asks for it, the Android images
// through Install / Update as before.
#pragma once
#include "core/Manifest.h"
#include "support/Common.h"
#include <chrono>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>

namespace WN::Core::Update {

inline constexpr std::chrono::hours kCheckInterval{20};

// -1, 0, 1: dotted numbers compared field by field ("0.10.0" > "0.9.3"); a missing field is 0.
[[nodiscard]] int CompareVersions(std::wstring_view a, std::wstring_view b);

// The release source and install folder chosen in the window (Source menu, Change folder).
[[nodiscard]] std::wstring SavedSourceUrl();
[[nodiscard]] std::filesystem::path SavedFolder();

// Help > Check for updates automatically (on by default).
[[nodiscard]] bool AutomaticCheck();
void SetAutomaticCheck(bool on);
// The last successful check is at least kCheckInterval ago.
[[nodiscard]] bool CheckDue();

// The newer WSA-Next an official manifest offers to this official WSA-Next.exe; null otherwise.
[[nodiscard]] const AppRelease* NewerApp(const Manifest& manifest);
// This WSA-Next.exe is older than the release requires ("minimum").
[[nodiscard]] bool AppTooOld(const Manifest& manifest);

// Keeps what an official manifest offers: a newer WSA-Next, and a release newer than the one
// installed in `folder`. True when that changed.
bool Record(const Manifest& manifest, const std::filesystem::path& folder);

struct Pending {
    std::wstring app;      // a newer WSA-Next, "" = none
    std::wstring release;  // a newer Android release than the installed one, "" = none
    [[nodiscard]] bool Any() const { return !app.empty() || !release.empty(); }
};
// What was found newer than this WSA-Next.exe and the release installed in `folder`.
[[nodiscard]] Pending Available(const std::filesystem::path& folder);

// Downloads WSA-Next Setup into the download folder under %ProgramFiles%\WSA-Next (administrators
// only write there), checks its size, SHA-256 and publisher signature, and starts Windows Installer
// on it. Windows Installer stops the background hosts, replaces WSA-Next.exe and starts the host
// again; then the Setup file is deleted and WSA-Next opens again. The caller (the window) closes
// right after.
Result<> DownloadAndStartSetup(const AppRelease& app, const ProgressFn& progress, const LogFn& log, std::stop_token stop);

}  // namespace WN::Core::Update
