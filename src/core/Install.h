// WSA-Next © 2026 Marek Wesołowski - wesmar
// Installing and updating WSA-Next: compares the release manifest with what is installed,
// downloads the changed components (resumable), unpacks them, prepares Windows and registers
// the package.
//
//   <folder>\                      install folder (default %ProgramFiles%\WSA-Next)
//     AppxManifest.xml, WsaClient\  ...   the Windows part ("windows" component)
//     system.vhdx, vendor.vhdx ...        Android disks
//     Tools\kernel, Tools\initrd.img      boot files
//     wsa-next.json                       installed components (id -> packedSha256, version)
//     .download\                          compressed downloads in progress
#pragma once
#include "core/Manifest.h"
#include "core/System.h"
#include "support/Common.h"
#include <filesystem>
#include <map>
#include <stop_token>
#include <string>
#include <vector>

namespace WN::Core {

// Progress of an installation for the GUI and the command line.
struct Progress {
    std::wstring phase;          // "Downloading system.vhdx"
    int permille = -1;           // whole job, -1 = indeterminate
    int step = 0, steps = 0;     // component n of m
    std::uint64_t done = 0, total = 0;  // bytes of the current step
    std::uint64_t speed = 0;     // bytes per second (downloads)
    double eta = 0;              // seconds left in the current step
};
using ProgressSink = std::function<void(const Progress&)>;

// Installed state (wsa-next.json).
struct InstalledState {
    std::wstring version, source;
    std::map<std::wstring, std::wstring, std::less<>> components;  // id -> packedSha256
    // id -> the top-level names it wrote into the install folder ("Tools", "AppxManifest.xml" ...):
    // a removal deletes these and nothing else of a folder the person may share with other files.
    std::map<std::wstring, std::vector<std::wstring>, std::less<>> entries;
    // True when every installed component has its entries (installations of WSA-Next 0.1.0 before
    // the list existed do not).
    [[nodiscard]] bool EntriesComplete() const;
};
InstalledState ReadState(const std::filesystem::path& folder);

std::filesystem::path DefaultFolder();  // %ProgramFiles%\WSA-Next
// Where the background host's WSA-Next.exe is installed: always %ProgramFiles%\WSA-Next (only
// administrators may change it), whatever folder Android is installed to.
inline std::filesystem::path HostFolder() { return DefaultFolder(); }

struct PlanItem {
    const Component* component;
    bool needed;  // false: already installed with the same content
};
struct Plan {
    std::vector<PlanItem> items;
    std::uint64_t downloadBytes = 0, diskBytes = 0;
    [[nodiscard]] bool UpToDate() const;
};
// What has to be downloaded into `folder`. Optional components (wsapatch) only when `system` needs them.
Plan MakePlan(const Manifest& manifest, const InstalledState& state, const SystemInfo& system, const std::filesystem::path& folder);

struct Options {
    std::filesystem::path folder;
    bool registerPackage = true;   // false: files only (tests, offline preparation)
    bool keepDownloads = false;    // keep the .zst files after unpacking
};

// Callbacks of a running installation. `confirm` asks the person (UI thread); false = stop.
struct Callbacks {
    ProgressSink progress;
    std::function<void(Level, std::wstring)> log;
    ConfirmFn confirm;
};

struct Outcome {
    bool restartNeeded = false;  // Virtual Machine Platform was just enabled
    unsigned installed = 0;      // components written
};

Result<Outcome> Run(const Manifest& manifest, const Options& options, const Callbacks& cb, std::stop_token stop);

}  // namespace WN::Core
