// WSA-Next © 2026 Marek Wesołowski - wesmar
// Removing WSA-Next, in one of two ways:
//
//   KeepData    the package registration and the install folder go. The Android data disk in the
//               user's profile (apps, accounts, files) and WSA-Next's settings stay, so the next
//               installation continues where this one ended. WSA itself takes the Windows entries
//               of Android apps (Start menu, Installed apps, associations) away with its package;
//               the next start creates them again (see ForgetPopulatedApps in Uninstall.cpp).
//   Everything  also the Android data, everything WSA left in Windows (install/Traces.h), the
//               settings of WSA-Next, and developer mode / Virtual Machine Platform when WSA-Next
//               turned them on. After WSA was removed through Windows Settings this still finds
//               and removes what was left behind.
//
// A WSA package registered from somewhere else (the Microsoft Store, another unpacked build) is
// never touched, and neither is what belongs to it.
#pragma once
#include "core/Install.h"
#include "support/Common.h"
#include <filesystem>
#include <string>
#include <vector>

namespace WN::Core {

enum class Removal { KeepData, Everything };

struct RemovalOutcome {
    bool restartNeeded = false;           // Virtual Machine Platform was disabled
    std::vector<std::wstring> leftovers;  // what could not be removed (each one is also logged)
};

// `folder` is where WSA-Next was installed; when the registered WSA package is a WSA-Next one from
// another folder, that folder is used. Blocking (package manager): call it on a worker thread with
// COM initialized (multithreaded apartment). Progress is indeterminate (phases only).
Result<RemovalOutcome> Uninstall(const std::filesystem::path& folder, Removal mode, const Callbacks& cb);

// Whether there is anything to remove for `folder` (a WSA-Next package or installed files).
bool Removable(const std::filesystem::path& folder, const SystemInfo& system);

}  // namespace WN::Core
