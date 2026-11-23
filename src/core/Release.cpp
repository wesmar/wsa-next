// WSA-Next © 2026 Marek Wesołowski - wesmar
#include "core/Release.h"
#include "support/Config.h"
#include "support/Handle.h"
#include "support/Settings.h"
#include "support/Sha256.h"
#include "support/Signature.h"
#include "support/Text.h"
#include <wincrypt.h>
#include <winioctl.h>
#include <bcrypt.h>
#include <SoftPub.h>
#include <mscat.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <format>
#include <fstream>
#include <sstream>
#include <vector>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "bcrypt.lib")

namespace WN::Core::Release {
namespace fs = std::filesystem;
namespace {

// The digest cache is machine-wide and only administrators may write it: with a user-writable
// install folder, a cache in HKCU would let any process of the user vouch for a changed disk.
const HKEY kDigestsRoot = HKEY_LOCAL_MACHINE;
constexpr wchar_t kDigestsKey[] = L"Software\\KVC\\WSA-Next\\Digests";
constexpr wchar_t kMachineKey[] = L"Software\\KVC\\WSA-Next";
constexpr wchar_t kMachineParent[] = L"Software\\KVC";
constexpr wchar_t kVerdictValue[] = L"ReleaseState";
constexpr char kSha256Oid[] = "2.16.840.1.101.3.4.2.1";

std::wstring Digest(std::string_view bytes) {
    Sha256 sha;
    sha.Update(bytes.data(), bytes.size());
    return sha.Finish();
}

std::wstring Hex(const BYTE* data, DWORD size) {
    std::wstring out;
    for (DWORD i = 0; i < size; ++i) out += std::format(L"{:02x}", data[i]);
    return out;
}

// A catalog member is tagged with its upper-case hex digest; its indirect data holds the digest.
// The catalog stays open without write or delete sharing meanwhile: the signature check and the
// member lookup see the same bytes, which nobody can swap in between.
bool VouchesFile(const fs::path& catalog, std::string_view manifest) {
    const FileHandle locked(CreateFileW(catalog.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!locked) return false;
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(locked.get(), FileAttributeTagInfo, &attributes, sizeof attributes) || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return false;  // a link, not a catalog
    if (Signature::CheckFile(catalog.c_str(), locked.get()) != Signature::Origin::Official) return false;
    const std::wstring digest = Digest(manifest);
    std::wstring tag = digest;
    std::ranges::transform(tag, tag.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });

    HANDLE cat = CryptCATOpen(const_cast<LPWSTR>(catalog.c_str()), CRYPTCAT_OPEN_EXISTING, 0, CRYPTCAT_VERSION_2, 0);
    if (cat == INVALID_HANDLE_VALUE) return false;
    bool listed = false;
    if (const CRYPTCATMEMBER* member = CryptCATGetMemberInfo(cat, tag.data()); member && member->pIndirectData) {
        const SIP_INDIRECT_DATA& data = *member->pIndirectData;
        listed = data.DigestAlgorithm.pszObjId && std::string_view(data.DigestAlgorithm.pszObjId) == kSha256Oid &&
                 Hex(data.Digest.pbData, data.Digest.cbData) == digest;
    }
    CryptCATClose(cat);
    return listed;
}

std::string ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

Result<> WriteAll(const fs::path& path, std::string_view bytes) {
    const fs::path temp = fs::path(path).concat(L".new");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) return Fail(Text::Format(IDS_CANNOT_CREATE_FILE, temp.wstring()));
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return Fail(ErrorText(Text::Format(IDS_CANNOT_CREATE_FILE, path.wstring()), GetLastError()));
    return {};
}

// What identifies a file's content without reading it. The change time alone is not enough:
// whoever may write a file may also set its change time back (FileBasicInfo). The USN - the
// file's last record in the NTFS change journal - moves with every change, closing a handle
// included, and nobody can set it. Without a journal there is no identity: the file is read.
struct Identity {
    ULONGLONG volume = 0;
    FILE_ID_128 id{};
    LONGLONG size = 0;
    LONGLONG changed = 0;
    LONGLONG usn = 0;
};

struct Remembered {
    Identity identity;
    wchar_t digest[64];
};

bool IdentityOf(HANDLE file, Identity& out) {
    FILE_ID_INFO id{};
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof id) || !GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof basic) ||
        !GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof standard))
        return false;
    // FSCTL_READ_FILE_USN_DATA answers with a USN record: version 2 (NTFS, 64-bit file references)
    // or version 3 (ReFS, 128-bit ones - Usn sits further on). Anything else: no identity.
    alignas(8) BYTE record[1024]{};
    DWORD returned = 0;
    if (!DeviceIoControl(file, FSCTL_READ_FILE_USN_DATA, nullptr, 0, record, sizeof record, &returned, nullptr) ||
        returned < sizeof(USN_RECORD_COMMON_HEADER))
        return false;
    const auto* header = reinterpret_cast<const USN_RECORD_COMMON_HEADER*>(record);
    if (header->MajorVersion == 2 && returned >= sizeof(USN_RECORD_V2)) out.usn = reinterpret_cast<const USN_RECORD_V2*>(record)->Usn;
    else if (header->MajorVersion == 3 && returned >= sizeof(USN_RECORD_V3)) out.usn = reinterpret_cast<const USN_RECORD_V3*>(record)->Usn;
    else return false;
    out.volume = id.VolumeSerialNumber;
    out.id = id.FileId;
    out.size = standard.EndOfFile.QuadPart;
    out.changed = basic.ChangeTime.QuadPart;
    return out.usn != 0;
}

bool Same(const Identity& a, const Identity& b) {
    return a.volume == b.volume && std::ranges::equal(a.id.Identifier, b.id.Identifier) && a.size == b.size && a.changed == b.changed &&
           a.usn == b.usn;
}

std::wstring CacheName(const fs::path& path) {
    std::wstring name = path.lexically_normal().wstring();
    std::ranges::transform(name, name.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return name;
}

// The file's SHA-256: remembered when the file is the same, otherwise read (at background I/O
// priority) and remembered. Empty when unreadable or canceled.
std::wstring FileDigest(const fs::path& path, const std::function<bool()>& canceled) {
    // No write sharing while reading: the bytes digested are the bytes of the identity remembered.
    const FileHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file) return {};
    Identity identity;
    const bool identified = IdentityOf(file.get(), identity);

    const std::wstring name = CacheName(path);
    Remembered remembered{};
    DWORD size = sizeof remembered;
    if (identified && RegGetValueW(kDigestsRoot, kDigestsKey, name.c_str(), RRF_RT_REG_BINARY, nullptr, &remembered, &size) == ERROR_SUCCESS &&
        size == sizeof remembered && Same(remembered.identity, identity))
        return std::wstring(remembered.digest, 64);

    Sha256 sha;
    std::vector<char> block(1 << 20);
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    bool complete = sha.ok();
    for (DWORD read = 0; complete;) {
        if (canceled && canceled()) complete = false;
        else if (!ReadFile(file.get(), block.data(), static_cast<DWORD>(block.size()), &read, nullptr)) complete = false;
        else if (read == 0) break;
        else sha.Update(block.data(), read);
    }
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    if (!complete) return {};
    const std::wstring digest = sha.Finish();
    if (identified && digest.size() == 64) {
        remembered.identity = identity;
        std::ranges::copy(digest, remembered.digest);
        RegSetKeyValueW(kDigestsRoot, kDigestsKey, name.c_str(), REG_BINARY, &remembered, sizeof remembered);
    }
    return digest;
}

}  // namespace

bool Vouches(std::string_view catalog, std::string_view manifest) {
    if (catalog.empty() || manifest.empty() || catalog.size() > (16u << 20)) return false;
    // Windows recognizes a catalog by its extension. The temporary folder is the user's: the name
    // is random and created here (CREATE_NEW fails on anything already there, a planted link too).
    wchar_t folder[MAX_PATH]{};
    BYTE random[16]{};
    if (!GetTempPathW(MAX_PATH, folder) ||
        !BCRYPT_SUCCESS(BCryptGenRandom(nullptr, random, sizeof random, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        return false;
    const fs::path path = fs::path(folder) / (L"wsn-" + Hex(random, sizeof random) + L".cat");
    bool written = false;
    {
        const FileHandle out(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!out) return false;
        DWORD done = 0;
        written = WriteFile(out.get(), catalog.data(), static_cast<DWORD>(catalog.size()), &done, nullptr) && done == catalog.size();
    }
    const bool vouches = written && VouchesFile(path, manifest);
    DeleteFileW(path.c_str());
    return vouches;
}

void Remember(const fs::path& path, const std::wstring& digest) {
    if (digest.size() != 64) return;
    // Opened without write sharing: nobody changes the file while its identity is read.
    const FileHandle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
    Remembered remembered{};
    if (!file || !IdentityOf(file.get(), remembered.identity)) return;
    std::ranges::copy(digest, remembered.digest);
    RegSetKeyValueW(kDigestsRoot, kDigestsKey, CacheName(path).c_str(), REG_BINARY, &remembered, sizeof remembered);
}

Result<> Record(const fs::path& folder, const Manifest& manifest) {
    std::error_code ec;
    if (!manifest.official) {
        fs::remove(folder / kInstalledManifest, ec);
        fs::remove(folder / kInstalledCatalog, ec);
        return {};
    }
    if (auto r = WriteAll(folder / kInstalledManifest, manifest.raw); !r) return r;
    return WriteAll(folder / kInstalledCatalog, manifest.catalog);
}

State VerifyInstalled(const fs::path& folder, const std::function<bool()>& canceled) {
    std::error_code ec;
    if (folder.empty() || !fs::is_directory(folder, ec)) return State::Missing;
    const std::string raw = ReadAll(folder / kInstalledManifest);
    if (raw.empty() || !VouchesFile(folder / kInstalledCatalog, raw)) return State::Unsigned;
    const auto manifest = ParseManifest(raw, (folder / kInstalledManifest).wstring());
    if (!manifest) return State::Unsigned;

    bool checked = false;
    for (const Component& c : manifest->components) {
        if (c.kind == ComponentKind::Archive) continue;  // Microsoft's part of the package
        const fs::path file = folder / c.target;
        if (c.optional && !fs::exists(file, ec)) continue;
        if (c.sha256.empty()) return State::Unsigned;  // a release from before the digests
        const std::wstring digest = FileDigest(file, canceled);
        if (canceled && canceled()) return State::Unknown;
        if (digest != c.sha256) return State::Modified;
        checked = true;
    }
    return checked ? State::Official : State::Unsigned;
}

bool ForgetDigests() {
    const bool removed = RegDeleteTreeW(kDigestsRoot, kDigestsKey) == ERROR_SUCCESS;
    RegDeleteKeyW(kDigestsRoot, kDigestsKey);
    RegDeleteKeyW(kDigestsRoot, kMachineKey);     // only when empty
    RegDeleteKeyW(kDigestsRoot, kMachineParent);  // only when no other KVC tool keeps a key there
    return removed;
}

State HostVerdict() { return static_cast<State>(Settings::ReadDword(kVerdictValue, 0)); }
void SetHostVerdict(State state) { Settings::WriteDword(kVerdictValue, static_cast<DWORD>(state)); }

}  // namespace WN::Core::Release
