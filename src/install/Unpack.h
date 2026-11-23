// WSA-Next © 2026 Marek Wesołowski - wesmar
// Unpacking of downloaded components: one zstd stream, written either to one file (a disk image,
// the kernel) or, for a tar inside, to a folder tree (the Windows part of WSA).
//
// Nothing replaces an existing file before the whole release is unpacked: files are written to
// "<name>.new" and flushed (Staged), and only Staged::Commit renames them over the old ones. The
// installer commits after every component has unpacked, so a failure or a cancellation midway
// leaves the previous release as it was.
#pragma once
#include "support/Common.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <stop_token>
#include <string>
#include <vector>

namespace WN::Install {

class NewFile;

// Files written as "<name>.new" and not in place yet. Destroyed uncommitted, it deletes them.
class Staged {
public:
    Staged();
    ~Staged();
    Staged(Staged&&) noexcept;
    Staged& operator=(Staged&&) noexcept;
    Staged(const Staged&) = delete;
    Staged& operator=(const Staged&) = delete;

    void Add(std::unique_ptr<NewFile> file);
    void Append(Staged&& other);
    // Renames every file over its target. Stops at the first failure (the rest stay staged).
    Result<> Commit();
    [[nodiscard]] std::size_t size() const noexcept { return m_files.size(); }

private:
    std::vector<std::unique_ptr<NewFile>> m_files;
};

struct FileReport {
    std::uint64_t bytes = 0;
    Staged staged;
};

// Decompresses `zst` into target + ".new". `expectedSize` (0 = unknown) bounds the output while it
// is written; `expectedSha256` (lower-case hex, empty = none) is checked once it is complete.
Result<FileReport> UnpackFile(const std::filesystem::path& zst, const std::filesystem::path& target, std::uint64_t expectedSize,
                              const std::wstring& expectedSha256, const ProgressFn& progress, std::stop_token stop);

struct ArchiveReport {
    std::uint64_t bytes = 0;
    unsigned files = 0;
    std::set<std::wstring> topLevel;  // first path element of every entry (what a removal deletes)
    Staged staged;
};

// Decompresses `zst` and extracts the ustar/pax/GNU tar inside into `folder` (each file staged).
// Paths are checked (no "..", no drive letters, no device names); regular files and folders only.
// `expectedSize` (0 = unknown) bounds the bytes of all files together.
Result<ArchiveReport> UnpackArchive(const std::filesystem::path& zst, const std::filesystem::path& folder, std::uint64_t expectedSize,
                                    const ProgressFn& progress, std::stop_token stop);

// A path element Windows would not keep as written: a device name (CON, NUL, COM1 ... also with an
// extension), or a name ending in a dot or a space.
[[nodiscard]] bool ReservedName(std::wstring_view element);

}  // namespace WN::Install
