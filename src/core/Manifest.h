// WSA-Next © 2026 Marek Wesołowski - wesmar
// The release manifest: what a WSA-Next release consists of and where each part is.
//
//   {
//     "format": 1,
//     "name": "WSA-Next", "version": "15.0.20261120",
//     "android": "Android 15 (BP1A.250505.005.D1)", "variant": "Google Play",
//     "minWindowsBuild": 22000,
//     "components": [
//       { "id": "system", "kind": "disk", "target": "system.vhdx", "url": "system.vhdx.zst",
//         "packedSize": 431234567, "packedSha256": "...", "size": 1085665280, "version": "15.0.20261120" },
//       ...
//     ]
//   }
//
// Every component is one zstd stream. "disk" and "file" unpack to `target` (a path inside the
// install folder), "archive" is a tar unpacked into the folder `target` ("." = the install folder).
// `url` is relative to the manifest (or absolute), so a release can be mirrored as a whole.
// `packedSha256` identifies the content: an installed component with the same digest is not
// downloaded again. `sha256` is the digest of a disk or file as installed. A release is official
// when manifest.cat next to the manifest - a catalog signed by WSA-Next's publisher - lists the
// manifest's SHA-256 (see core/Release.h).
//
// Optional, next to "components": the WSA-Next Setup that goes with the release (core/Update.h).
// Versions of WSA-Next that do not know it read past it.
//
//     "app": { "version": "0.2.0", "url": "WSA-Next.msi", "size": 1568768, "sha256": "...",
//              "minimum": "0.1.0" }
//
// `minimum` (optional): the oldest WSA-Next.exe that may install this release.
#pragma once
#include "support/Common.h"
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace WN::Core {

enum class ComponentKind { Disk, File, Archive };

struct Component {
    std::wstring id;       // "system", "system_ext", "product", "vendor", "kernel", "windows", "wsapatch"
    ComponentKind kind = ComponentKind::Disk;
    std::wstring target;   // relative to the install folder
    std::wstring url;      // absolute after loading
    std::uint64_t packedSize = 0;
    std::wstring packedSha256;  // lower-case hex of the .zst file
    std::wstring sha256;        // lower-case hex of the unpacked file (disk, file); may be absent
    std::uint64_t size = 0;     // unpacked size (archive: sum of the files)
    std::wstring version;
    bool optional = false;      // only installed when needed (wsapatch)
};

// WSA-Next Setup (WSA-Next.msi) as the release lists it.
struct AppRelease {
    std::wstring version;  // "0.2.0"
    std::wstring url;      // absolute after loading
    std::uint64_t size = 0;
    std::wstring sha256;   // lower-case hex of the .msi
    std::wstring minimum;  // the oldest WSA-Next.exe that may install this release; empty = any
};

struct Manifest {
    std::wstring source;  // URL it was loaded from
    std::wstring name, version, android, variant;
    std::uint32_t minWindowsBuild = 0;
    std::vector<Component> components;
    std::optional<AppRelease> app;  // absent, or not usable as listed
    std::string raw;        // the manifest as loaded
    std::string catalog;    // manifest.cat next to it, when there is one
    bool official = false;  // the catalog is signed by WSA-Next's publisher and lists the manifest

    [[nodiscard]] const Component* Find(std::wstring_view id) const;
};

// Resolves `relative` against the URL of the manifest ("a/b/manifest.json" + "x.zst" = "a/b/x.zst").
std::wstring ResolveUrl(const std::wstring& base, const std::wstring& relative);

Result<Manifest> ParseManifest(std::string_view utf8, const std::wstring& url);
// `connectMs` / `ioMs`: how long a server may take to answer (the background check waits less).
Result<Manifest> LoadManifest(const std::wstring& url, std::stop_token stop, unsigned long connectMs = 10000, unsigned long ioMs = 20000);

}  // namespace WN::Core
