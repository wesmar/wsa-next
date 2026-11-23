// WSA-Next © 2026 Marek Wesołowski - wesmar
// What this Windows offers WSA: version, the Virtual Machine Platform feature, developer mode
// (needed to register an unpacked package) and the WSA package already registered, if any.
#pragma once
#include "install/Package.h"
#include "support/Common.h"
#include <cstdint>
#include <optional>
#include <string>

namespace WN::Core {

struct SystemInfo {
    std::uint32_t build = 0, revision = 0;  // 26100.4061
    std::wstring edition;                    // "Windows 11 Pro"
    std::wstring displayVersion;             // "24H2"
    std::optional<bool> virtualMachinePlatform;  // nullopt: could not be read
    bool developerMode = false;
    std::optional<Install::PackageInfo> package;
    // Without a package of its own: WSA registered for another Windows user (one user at a time).
    std::optional<Install::OtherRegistration> otherUser;

    [[nodiscard]] bool Supported() const;      // Windows 11
    [[nodiscard]] bool NeedsPatch() const;     // build >= Config::kPatchFromBuild
    [[nodiscard]] std::wstring Version() const;  // "Windows 11 Pro 24H2 (26100.4061)"
};

// Reads everything above. Blocking (WMI, package manager): call it on a worker thread.
SystemInfo Inspect();

// Turns developer mode on (HKLM AppModelUnlock, administrator).
Result<> EnableDeveloperMode();

// Enables the Virtual Machine Platform feature with DISM; true = Windows must restart.
Result<bool> EnableVirtualMachinePlatform(const LogFn& log);

// The reverse, for a total removal of what WSA-Next turned on.
Result<> DisableDeveloperMode();
Result<bool> DisableVirtualMachinePlatform(const LogFn& log);

// Distributions registered in WSL for the current user (WSL 2 needs Virtual Machine Platform).
unsigned WslDistributions();

}  // namespace WN::Core
