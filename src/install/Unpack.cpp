// WSA-Next © 2026 Marek Wesołowski - wesmar
#include "install/Unpack.h"
#include "support/Handle.h"
#include "support/Sha256.h"
#include "support/Text.h"
#include <Windows.h>
#include <zstd.h>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace WN::Install {
namespace fs = std::filesystem;
namespace {

struct DStreamFree {
    void operator()(ZSTD_DStream* d) const noexcept { ZSTD_freeDStream(d); }
};

// Streams the decompressed bytes of `zst` to `sink`; progress follows the compressed input.
Result<> Decompress(const fs::path& zst, const ProgressFn& progress, std::stop_token stop,
                    const std::function<Result<>(const char*, std::size_t)>& sink) {
    FileHandle in(CreateFileW(zst.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!in) return Fail(ErrorText(Text::Format(IDS_CTX_OPEN, zst.wstring()), GetLastError()));
    LARGE_INTEGER total{};
    GetFileSizeEx(in.get(), &total);

    const std::unique_ptr<ZSTD_DStream, DStreamFree> ds(ZSTD_createDStream());
    if (!ds) return Fail(Text::Get(IDS_ZSTD_MEMORY));
    ZSTD_initDStream(ds.get());
    // Releases are compressed with --long=31 (2 GB window).
    ZSTD_DCtx_setParameter(ds.get(), ZSTD_d_windowLogMax, 31);

    std::vector<char> input(ZSTD_DStreamInSize() * 8), output(ZSTD_DStreamOutSize() * 8);
    std::uint64_t consumed = 0;
    // ZSTD_decompressStream returns 0 once a frame is decoded and completely flushed, otherwise how
    // much is still to come. It is called again while input is left, or while the last call filled
    // the output buffer of an unfinished frame (more output is pending). It must not be called
    // after a finished frame with no input: that starts a new frame and returns its header size.
    std::size_t ret = 1;
    for (;;) {
        if (stop.stop_requested()) return Fail(Text::Get(IDS_CANCELED));
        DWORD read = 0;
        if (!ReadFile(in.get(), input.data(), static_cast<DWORD>(input.size()), &read, nullptr))
            return Fail(ErrorText(Text::Format(IDS_CTX_READ, zst.wstring()), GetLastError()));
        if (!read) break;
        ZSTD_inBuffer src{input.data(), read, 0};
        bool outputFull = false;
        do {
            ZSTD_outBuffer dst{output.data(), output.size(), 0};
            ret = ZSTD_decompressStream(ds.get(), &dst, &src);
            if (ZSTD_isError(ret)) return Fail(Text::Format(IDS_ZSTD_ERROR, FromUtf8(ZSTD_getErrorName(ret))));
            if (dst.pos)
                if (auto r = sink(output.data(), dst.pos); !r) return r;
            outputFull = dst.pos == dst.size;
        } while (src.pos < src.size || (outputFull && ret != 0));
        consumed += read;
        progress(consumed, static_cast<std::uint64_t>(total.QuadPart));
    }
    if (ret != 0) return Fail(Text::Get(IDS_ZSTD_TRUNCATED));
    return {};
}

}  // namespace

// Output file written through "<name>.new". It stays open without sharing from creation until
// it is in place: Finish flushes it, Place renames it over the target through the same handle,
// so nobody can change or swap the bytes in between - not even in a folder the user may write.
// Destroyed before Place, it deletes the ".new".
class NewFile {
public:
    Result<> Open(const fs::path& target) {
        m_target = target;
        m_temp = target;
        m_temp += L".new";
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        m_file.reset(CreateFileW(m_temp.c_str(), GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!m_file) return Fail(ErrorText(Text::Format(IDS_CTX_CREATE, m_temp.wstring()), GetLastError()));
        return {};
    }
    Result<> Write(const char* data, std::size_t n) {
        while (n) {
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(n, 1u << 30));
            DWORD written = 0;
            if (!WriteFile(m_file.get(), data, chunk, &written, nullptr) || written != chunk)
                return Fail(ErrorText(Text::Format(IDS_CTX_WRITE, m_temp.wstring()), GetLastError()));
            data += chunk;
            n -= chunk;
            m_bytes += chunk;
        }
        return {};
    }
    Result<> Finish() {
        if (!FlushFileBuffers(m_file.get())) return Fail(ErrorText(Text::Format(IDS_CTX_WRITE, m_temp.wstring()), GetLastError()));
        return {};
    }
    Result<> Place() {
        // A read-only target (copied from a read-only source) would block the rename.
        SetFileAttributesW(m_target.c_str(), FILE_ATTRIBUTE_NORMAL);
        const std::wstring name = m_target.wstring();
        std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t));
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        info->ReplaceIfExists = TRUE;
        info->RootDirectory = nullptr;
        info->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
        std::memcpy(info->FileName, name.data(), info->FileNameLength);
        if (!SetFileInformationByHandle(m_file.get(), FileRenameInfo, info, static_cast<DWORD>(buffer.size())))
            return Fail(ErrorText(Text::Format(IDS_CTX_REPLACE, m_target.wstring()), GetLastError()));
        m_temp.clear();
        return {};
    }
    ~NewFile() {
        m_file.reset();
        if (!m_temp.empty()) DeleteFileW(m_temp.c_str());
    }
    [[nodiscard]] std::uint64_t bytes() const noexcept { return m_bytes; }
    [[nodiscard]] bool placed() const noexcept { return m_temp.empty(); }

private:
    fs::path m_target, m_temp;
    FileHandle m_file;
    std::uint64_t m_bytes = 0;
};

Staged::Staged() = default;
Staged::~Staged() = default;
Staged::Staged(Staged&&) noexcept = default;
Staged& Staged::operator=(Staged&&) noexcept = default;

void Staged::Add(std::unique_ptr<NewFile> file) { m_files.push_back(std::move(file)); }

void Staged::Append(Staged&& other) {
    for (auto& file : other.m_files) m_files.push_back(std::move(file));
    other.m_files.clear();
}

Result<> Staged::Commit() {
    for (auto& file : m_files)
        if (!file->placed())
            if (auto r = file->Place(); !r) return r;
    m_files.clear();  // closes them
    return {};
}

bool ReservedName(std::wstring_view element) {
    if (element.empty() || element == L"." || element == L"..") return false;
    if (element.back() == L'.' || element.back() == L' ') return true;  // Windows would drop them
    // A device name counts up to the first dot ("NUL.txt" is NUL), whatever the case.
    std::wstring base(element.substr(0, element.find(L'.')));
    while (!base.empty() && base.back() == L' ') base.pop_back();
    for (auto& c : base) c = static_cast<wchar_t>(std::towupper(c));
    static constexpr std::wstring_view kDevices[] = {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"};
    if (std::ranges::find(kDevices, std::wstring_view(base)) != std::end(kDevices)) return true;
    if (base.size() == 4 && (base.starts_with(L"COM") || base.starts_with(L"LPT"))) {
        const wchar_t n = base[3];
        return (n >= L'0' && n <= L'9') || n == 0x00B9 || n == 0x00B2 || n == 0x00B3;  // also superscript 1-3
    }
    return false;
}

namespace {

// ---------------------------------------------------------------------------
// Streaming tar reader (ustar, pax and GNU long names).
// ---------------------------------------------------------------------------
constexpr std::size_t kBlock = 512;

std::string CString(const char* p, std::size_t max) {
    std::size_t n = 0;
    while (n < max && p[n]) ++n;
    return std::string(p, n);
}

std::uint64_t ParseOctal(const char* p, std::size_t len) {
    if (static_cast<unsigned char>(p[0]) & 0x80) {  // GNU base-256
        std::uint64_t v = static_cast<unsigned char>(p[0]) & 0x7F;
        for (std::size_t i = 1; i < len; ++i) v = (v << 8) | static_cast<unsigned char>(p[i]);
        return v;
    }
    std::uint64_t v = 0;
    std::size_t i = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\0')) ++i;
    for (; i < len && p[i] >= '0' && p[i] <= '7'; ++i) v = v * 8 + static_cast<std::uint64_t>(p[i] - '0');
    return v;
}

bool ChecksumOk(const char* h) {
    const std::uint64_t expected = ParseOctal(h + 148, 8);
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < kBlock; ++i) sum += (i >= 148 && i < 156) ? ' ' : static_cast<unsigned char>(h[i]);
    return sum == expected;
}

Result<fs::path> SafePath(const std::string& name) {
    std::wstring wide = FromUtf8(name);
    std::ranges::replace(wide, L'/', L'\\');
    fs::path result;
    std::size_t start = 0;
    while (start <= wide.size()) {
        std::size_t end = wide.find(L'\\', start);
        if (end == std::wstring::npos) end = wide.size();
        const std::wstring part = wide.substr(start, end - start);
        start = end + 1;
        if (part.empty() || part == L".") continue;
        if (part == L".." || part.find_first_of(L":*?\"<>|") != std::wstring::npos || ReservedName(part) ||
            std::ranges::any_of(part, [](wchar_t c) { return c < 0x20; }))
            return Fail(Text::Format(IDS_TAR_UNSAFE, wide));
        result /= part;
    }
    return result;
}

class TarWriter {
public:
    TarWriter(fs::path folder, std::uint64_t limit) : m_folder(std::move(folder)), m_limit(limit) {}

    Result<> Feed(const char* data, std::size_t n) {
        while (n) {
            if (m_ended) return {};  // trailing zero blocks and padding
            if (m_fileLeft) {        // file content
                const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(m_fileLeft, n));
                if (m_out) {
                    m_written += take;
                    if (m_limit && m_written > m_limit) return Fail(Text::Format(IDS_UNPACK_TOO_LARGE, m_folder.wstring(), m_limit));
                    if (auto r = m_out->Write(data, take); !r) return r;
                }
                if (m_record) m_recordData.append(data, take);
                data += take; n -= take; m_fileLeft -= take;
                if (!m_fileLeft)
                    if (auto r = FinishEntry(); !r) return r;
                continue;
            }
            if (m_padLeft) {
                const std::size_t take = std::min(m_padLeft, n);
                data += take; n -= take; m_padLeft -= take;
                continue;
            }
            const std::size_t take = std::min(kBlock - m_header.size(), n);
            m_header.append(data, take);
            data += take; n -= take;
            if (m_header.size() == kBlock) {
                auto r = Header();
                m_header.clear();
                if (!r) return r;
            }
        }
        return {};
    }

    Result<ArchiveReport> Finish() {
        if (m_fileLeft || !m_header.empty()) return Fail(Text::Get(IDS_TAR_TRUNCATED));
        return std::move(m_report);
    }

private:
    Result<> Header() {
        const char* h = m_header.data();
        if (std::all_of(h, h + kBlock, [](char c) { return c == 0; })) { m_ended = true; return {}; }
        if (!ChecksumOk(h)) return Fail(Text::Get(IDS_TAR_CHECKSUM));
        const std::uint64_t size = ParseOctal(h + 124, 12);
        const char type = h[156];
        std::string name = CString(h, 100);
        if (std::memcmp(h + 257, "ustar\0", 6) == 0) {
            const std::string prefix = CString(h + 345, 155);
            if (!prefix.empty()) name = prefix + "/" + name;
        }
        const std::size_t pad = static_cast<std::size_t>((kBlock - size % kBlock) % kBlock);

        if (type == 'L' || type == 'x' || type == 'g') {  // metadata records for the next entry
            if (size > (1u << 20)) return Fail(Text::Get(IDS_TAR_PAX));
            m_record = type;
            m_recordData.clear();
            return Begin(size, pad, nullptr);
        }
        if (!m_longName.empty()) name = m_longName;
        if (!m_paxPath.empty()) name = m_paxPath;
        const std::uint64_t entrySize = m_paxSize.value_or(size);
        m_longName.clear(); m_paxPath.clear(); m_paxSize.reset();

        auto path = SafePath(name);
        if (!path) return Fail(path.error());
        if (!path->empty()) m_report.topLevel.insert(path->begin()->wstring());
        if (type == '5') {
            std::error_code ec;
            if (!path->empty()) fs::create_directories(m_folder / *path, ec);
            return {};
        }
        if (type != '0' && type != '\0') return Fail(Text::Format(IDS_TAR_TYPE, static_cast<wchar_t>(type)));
        if (path->empty()) return Fail(Text::Get(IDS_TAR_UNSAFE));
        auto out = std::make_unique<NewFile>();
        if (auto r = out->Open(m_folder / *path); !r) return r;
        const std::size_t entryPad = static_cast<std::size_t>((kBlock - entrySize % kBlock) % kBlock);
        return Begin(entrySize, entryPad, std::move(out));
    }

    Result<> Begin(std::uint64_t size, std::size_t pad, std::unique_ptr<NewFile> out) {
        m_out = std::move(out);
        m_fileLeft = size;
        m_pendingPad = pad;
        if (!size) return FinishEntry();
        return {};
    }

    Result<> FinishEntry() {
        m_padLeft = m_pendingPad;
        if (m_record) {
            if (m_record == 'L') m_longName = CString(m_recordData.data(), m_recordData.size());
            else if (m_record == 'x') ParsePax();
            m_record = 0;
        }
        if (m_out) {
            if (auto r = m_out->Finish(); !r) return r;
            m_report.bytes += m_out->bytes();
            ++m_report.files;
            m_report.staged.Add(std::move(m_out));
        }
        return {};
    }

    // "<len> key=value\n" records
    void ParsePax() {
        std::size_t pos = 0;
        const std::string& data = m_recordData;
        while (pos < data.size()) {
            const std::size_t space = data.find(' ', pos);
            if (space == std::string::npos) break;
            const auto len = static_cast<std::size_t>(std::strtoull(data.c_str() + pos, nullptr, 10));
            if (len == 0 || pos + len > data.size()) break;
            const std::string record = data.substr(space + 1, pos + len - space - 2);
            const std::size_t eq = record.find('=');
            if (eq != std::string::npos) {
                const std::string key = record.substr(0, eq), value = record.substr(eq + 1);
                if (key == "path") m_paxPath = value;
                else if (key == "size") m_paxSize = std::strtoull(value.c_str(), nullptr, 10);
            }
            pos += len;
        }
    }

    fs::path m_folder;
    std::uint64_t m_limit = 0, m_written = 0;
    std::string m_header;
    std::unique_ptr<NewFile> m_out;
    std::uint64_t m_fileLeft = 0;
    std::size_t m_padLeft = 0, m_pendingPad = 0;
    char m_record = 0;
    std::string m_recordData, m_longName, m_paxPath;
    std::optional<std::uint64_t> m_paxSize;
    bool m_ended = false;
    ArchiveReport m_report;
};

}  // namespace

Result<FileReport> UnpackFile(const fs::path& zst, const fs::path& target, std::uint64_t expectedSize, const std::wstring& expectedSha256,
                              const ProgressFn& progress, std::stop_token stop) {
    auto out = std::make_unique<NewFile>();
    if (auto r = out->Open(target); !r) return Fail(r.error());
    // The digest is taken from the bytes on their way to the disk: no second read.
    std::optional<Sha256> sha;
    if (!expectedSha256.empty()) {
        sha.emplace();
        if (!sha->ok()) return Fail(Text::Get(IDS_SHA_UNAVAILABLE));
    }
    auto r = Decompress(zst, progress, stop, [&](const char* data, std::size_t n) -> Result<> {
        if (expectedSize && out->bytes() + n > expectedSize)
            return Fail(Text::Format(IDS_UNPACK_TOO_LARGE, target.filename().wstring(), expectedSize));
        if (sha) sha->Update(data, n);
        return out->Write(data, n);
    });
    if (!r) return Fail(r.error());
    if (expectedSize && out->bytes() != expectedSize)
        return Fail(Text::Format(IDS_UNPACK_SIZE, target.filename().wstring(), out->bytes(), expectedSize));
    if (sha && sha->Finish() != expectedSha256) return Fail(Text::Format(IDS_UNPACK_DIGEST, target.filename().wstring()));
    if (auto f = out->Finish(); !f) return Fail(f.error());
    FileReport report;
    report.bytes = out->bytes();
    report.staged.Add(std::move(out));
    return report;
}

Result<ArchiveReport> UnpackArchive(const fs::path& zst, const fs::path& folder, std::uint64_t expectedSize, const ProgressFn& progress,
                                    std::stop_token stop) {
    TarWriter tar(folder, expectedSize);
    auto r = Decompress(zst, progress, stop, [&](const char* data, std::size_t n) { return tar.Feed(data, n); });
    if (!r) return Fail(r.error());
    return tar.Finish();
}

}  // namespace WN::Install
