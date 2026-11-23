// WSA-Next © 2026 Marek Wesołowski - wesmar
// Timestamped log lines to the console and to wsa-bt-bridge.log next to the executable.
// Thread-safe; the bridge logs from the socket thread and from WinRT callbacks.
#pragma once
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace WN::Bt {

void LogLine(std::string_view text);

// %LOCALAPPDATA%\WSA-Next: the bridge's log (removed by "Remove everything").
std::filesystem::path LogFolder();

template <class... Args>
void Log(std::format_string<Args...> format, Args&&... args) {
    LogLine(std::format(format, std::forward<Args>(args)...));
}

// "Hex dump" of a packet, at most `limit` bytes.
std::string Hex(const void* data, std::size_t size, std::size_t limit = 48);

}  // namespace WN::Bt
