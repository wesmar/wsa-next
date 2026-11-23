// Project-wide constants.
#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace WN::Config {

inline constexpr wchar_t kAppTitle[] = L"WSA-Next";
inline constexpr wchar_t kAppVersion[] = L"0.1.0";
inline constexpr wchar_t kCopyright[] = L"© 2026 Marek Wesołowski - wesmar";
inline constexpr wchar_t kExeName[] = L"WSA-Next.exe";
inline constexpr wchar_t kRegKey[] = L"Software\\KVC\\WSA-Next";
inline constexpr wchar_t kRegParent[] = L"Software\\KVC";  // shared with other KVC tools
// Values under kRegKey remembering what WSA-Next turned on in Windows: a total removal turns
// exactly that off again, and nothing the person had enabled before.
inline constexpr wchar_t kEnabledDeveloperMode[] = L"EnabledDeveloperMode";
inline constexpr wchar_t kEnabledVmp[] = L"EnabledVirtualMachinePlatform";
// WSA-Next Setup (WSA-Next.msi, build_msi.ps1): the identity of the product in every version.
// When it is installed it owns WSA-Next.exe and its language files in %ProgramFiles%\WSA-Next.
inline constexpr wchar_t kSetupUpgradeCode[] = L"{8F4A9E21-6C3D-4B7A-9E12-5D8A1C3F4E90}";

// Release sources. Each one is a manifest.json; the component URLs in it are relative to it.
// GitHub: "releases/download/components/<asset>" holds the manifest and Android disk images,
// while the "latest" release stays clean with only WSA-Next.msi and WSA-Next.zip.
inline constexpr wchar_t kGitHubRepo[] = L"wesmar/wsa-next";
// The project's page, with the ways to support it (the window's "Support" button).
inline constexpr wchar_t kSupportUrl[] = L"https://kvc.pl/wsa-next";
inline constexpr wchar_t kManifestAsset[] = L"manifest.json";
inline constexpr wchar_t kMirrorManifest[] = L"https://kvc.pl/android/manifest.json";
// A release built from source and served on this computer: the folder package-release.sh writes,
// published by any local web server as /wsa-next.
inline constexpr wchar_t kLocalManifest[] = L"http://localhost/wsa-next/manifest.json";

// The Windows package. WSA-Next registers Microsoft's WSA 2407 package from its own folder and keeps
// its identity: WsaClient and WsaService depend on it (wsa:// links, notifications, Start menu
// shortcuts of Android apps, the per-user data disk in the package's LocalCache).
inline constexpr wchar_t kPackageName[] = L"MicrosoftCorporationII.WindowsSubsystemForAndroid";
inline constexpr wchar_t kPackageFamily[] = L"MicrosoftCorporationII.WindowsSubsystemForAndroid_8wekyb3d8bbwe";
inline constexpr wchar_t kPackageManifest[] = L"AppxManifest.xml";
inline constexpr wchar_t kSettingsApp[] = L"SettingsApp";  // Application Id of WSA Settings (in its Start menu entry)

// Install folder: %ProgramFiles%\<kInstallFolder> unless the user picks another one.
inline constexpr wchar_t kInstallFolder[] = L"WSA-Next";
// What is installed, written by the installer next to the package (component id -> sha256).
inline constexpr wchar_t kStateFile[] = L"wsa-next.json";
// Compressed downloads in progress (resumed on the next run), removed after a successful install.
inline constexpr wchar_t kDownloadFolder[] = L".download";

// Windows 11 is required (WSA 2407 does not run on Windows 10).
inline constexpr std::uint32_t kMinWindowsBuild = 22000;
// From this build on Windows refuses WSA; WSAPatch (a separate tool) makes it start again.
inline constexpr std::uint32_t kPatchFromBuild = 28000;

// Download attempts per component before giving up (each one resumes where the last stopped).
inline constexpr int kDownloadAttempts = 5;

}  // namespace WN::Config
