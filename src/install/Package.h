// WSA-Next © 2026 Marek Wesołowski - wesmar
// The WSA package in Windows: finding it, its dependencies, registering the unpacked folder,
// shutting WSA down and starting it. Calls block; run them on a worker thread (not the UI's STA).
#pragma once
#include "host/Guest.h"
#include "support/Common.h"
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace WN::Install {

struct PackageInfo {
    std::wstring fullName;
    std::wstring version;            // "2407.40000.4.0"
    std::filesystem::path location;  // folder the package was registered from
    bool developmentMode = false;    // registered from a folder (ours or another unpacked build)
};

// The WSA package of the current user, if any.
std::optional<PackageInfo> FindWsa();

// WSA registered for another Windows user of this PC and not for the current one: that user's
// account name and the folder it was registered from. Windows lets one user at a time register
// an unpacked package (a second one gets 0x80073CFB). Administrator; nullopt without the right.
struct OtherRegistration {
    std::wstring user;
    std::filesystem::path location;
};
std::optional<OtherRegistration> FindWsaOfOtherUser();

// Installs the dependency packages named in <folder>\AppxManifest.xml (VCLibs, UI.Xaml) from the
// "<Name>_x64.appx" files next to it, when the installed version is older than required.
Result<> InstallDependencies(const std::filesystem::path& folder, const LogFn& log);

// Registers <folder>\AppxManifest.xml in development mode (the package keeps Microsoft's identity;
// an existing registration is updated in place and the user's data stays).
Result<> Register(const std::filesystem::path& folder, const LogFn& log);

// Defaults for a WSA that has no settings yet (a new installation); a choice already made in
// WSA's settings stays:
//  - developer mode off, as in the Microsoft Store version (WSA Settings showed it on);
//  - optional diagnostic data off: WSA waits at its first start until the user answers that
//    question, and with nothing left to send it to (WSA is discontinued) "no" is the answer
//    that changes nothing - it can be turned on in WSA's settings.
void DefaultWsaSettings();

// Removes the package registration (`keepData`: the Android data disk in LocalCache stays).
Result<> Unregister(const PackageInfo& package, bool keepData);

// Other packages of the current user registered in development mode (they need developer mode).
std::vector<std::wstring> OtherDevelopmentPackages();

// Asks a running WSA to shut down (WsaClient /shutdown through the package's app execution alias)
// and waits for it. `folder` (the install folder) is only its working directory.
void Shutdown(const std::filesystem::path& folder, const LogFn& log);

// Stops WSA without removing anything, to free what its VM holds (memory, CPU): Android shuts
// down the way WSA's own settings do it, the VM's end is awaited as a notification; a VM still
// running after 30 s is terminated. WSA starts again with the next Android app. Administrator.
Guest::VmStop Stop();

// Starts Android and shows its first app: the Play Store on the Google Play variant, Android's
// Settings otherwise. Opening WSA Settings would not start Android, and Windows learns of the
// Android apps (Start menu, installed apps) only once Android has run.
Result<> LaunchAndroid();
Result<> OpenUri(const std::wstring& uri);  // also an Android app: "wsa://<package>"

}  // namespace WN::Install
