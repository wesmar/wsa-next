// WSA-Next © 2026 Marek Wesołowski - wesmar
// Official WSA-Next releases. A release is official when a catalog signed by WSA-Next's publisher
// (manifest.cat, next to manifest.json) lists the SHA-256 of its manifest. The manifest lists the
// SHA-256 of every Android disk and boot file as installed, so the signature also vouches for the
// images in the install folder:
//
//   - an official WSA-Next.exe installs official releases only;
//   - its background host serves Bluetooth and Windows Hello only to an official release that
//     nobody changed after installing it.
//
// A repackaged image (other branding, other files) is a different image: WSA-Next does not lend
// its features to it.
#pragma once
#include "core/Manifest.h"
#include "support/Common.h"
#include <Windows.h>
#include <filesystem>
#include <functional>
#include <string_view>

namespace WN::Core::Release {

inline constexpr wchar_t kCatalogAsset[] = L"manifest.cat";       // next to manifest.json
inline constexpr wchar_t kInstalledManifest[] = L"release.json";  // in the install folder
inline constexpr wchar_t kInstalledCatalog[] = L"release.cat";

// Whether `catalog` (a catalog file's bytes) is signed by the publisher and lists `manifest`.
[[nodiscard]] bool Vouches(std::string_view catalog, std::string_view manifest);

// Keeps the installed release's manifest and catalog in the install folder (removes them when
// the release is not official).
Result<> Record(const std::filesystem::path& folder, const Manifest& manifest);

enum class State : DWORD {
    Unknown = 0,
    Official = 1,   // the publisher's release, unchanged
    Checking = 2,   // the host is reading the images (first start after installing)
    Unsigned = 3,   // not an official release (or installed by something else)
    Modified = 4,   // an official release whose files were changed afterwards
    Missing = 5,    // no WSA installed
};

// Whether the package folder holds an official release, unchanged. Each file's digest is
// remembered (HKLM, administrators only) with its identity - volume, file ID, size, change time
// and its NTFS change journal USN, which nobody can set back: only new or changed files are read
// again, and a volume without a change journal is always read. `canceled` is polled between blocks.
[[nodiscard]] State VerifyInstalled(const std::filesystem::path& folder, const std::function<bool()>& canceled);

// Remembers `digest` for the file at `path` as it is now, so that the host does not read it again.
// Only for a folder that only administrators may change (the installer took the digest while it
// wrote the file; in a folder the user may write, the file could have changed since).
void Remember(const std::filesystem::path& path, const std::wstring& digest);

// Removes the digest cache (HKLM, administrators only) on a total removal.
bool ForgetDigests();

// The host's latest verdict for the GUI (HKCU, written by the host).
[[nodiscard]] State HostVerdict();
void SetHostVerdict(State state);

}  // namespace WN::Core::Release
