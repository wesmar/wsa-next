// WSA-Next © 2026 Marek Wesołowski - wesmar
#include "bluetooth/Log.h"
#include "bluetooth/Bridge.h"
#include <Windows.h>
#include <ShlObj.h>
#include <share.h>
#include <cstdio>
#include <filesystem>
#include <mutex>

namespace WN::Bt {
namespace {

std::mutex g_lock;
std::function<void(std::string_view)> g_sink;

std::FILE* LogFile() {
    static std::FILE* file = [] {
        const std::filesystem::path folder = LogFolder();
        std::error_code ec;
        if (folder.empty() || (std::filesystem::create_directories(folder, ec), ec)) return static_cast<std::FILE*>(nullptr);
        // Shared: the window opens the log while the bridge writes it.
        return _wfsopen((folder / L"bluetooth.log").c_str(), L"ab", _SH_DENYNO);
    }();
    return file;
}

}  // namespace

std::filesystem::path LogFolder() {
    PWSTR local = nullptr;
    std::filesystem::path folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) folder = std::filesystem::path(local) / L"WSA-Next";
    CoTaskMemFree(local);
    return folder;
}

void SetLogSink(std::function<void(std::string_view)> sink) {
    std::lock_guard guard(g_lock);
    g_sink = std::move(sink);
}

void LogLine(std::string_view text) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    const std::string line = std::format("[{:02}:{:02}:{:02}.{:03}] {}\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, text);
    std::lock_guard guard(g_lock);
    if (g_sink) {
        g_sink(std::string_view(line).substr(0, line.size() - 1));
    } else {
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fflush(stdout);
    }
    if (std::FILE* f = LogFile()) {
        std::fwrite(line.data(), 1, line.size(), f);
        std::fflush(f);
    }
}

std::string Hex(const void* data, std::size_t size, std::size_t limit) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::string out;
    for (std::size_t i = 0; i < size && i < limit; ++i) out += std::format("{:02x}", bytes[i]);
    if (size > limit) out += std::format("... ({} bytes)", size);
    return out;
}

}  // namespace WN::Bt
