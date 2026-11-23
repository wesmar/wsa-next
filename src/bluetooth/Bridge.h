// WSA-Next © 2026 Marek Wesołowski - wesmar
// The Bluetooth bridge: Android's HCI HAL in WSA <-> Hyper-V socket <-> emulated controller <->
// the PC's adapter through WinRT. Runs in the background host (src/host) while Bluetooth for
// Android is on; "WSA-Next.exe bluetooth" runs it in the console for diagnostics.
#pragma once
#include <cstdint>
#include <functional>
#include <stop_token>
#include <string_view>

namespace WN::Bt {

inline constexpr char kBridgeVersion[] = "0.1.0";
inline constexpr std::uint32_t kBridgePort = 0x57534254;  // "WSBT": the vsock port the Android HAL listens on

enum class BridgeResult { Stopped, NotElevated, NoAdapter };

// Serves the WSA HAL until `stop`: connects to it whenever WSA runs and emulates the controller
// on the PC's adapter. Administrator rights are required (Host Compute Service).
BridgeResult RunBridge(std::stop_token stop);

// Where log lines go besides the log file (WSA-Next's console).
void SetLogSink(std::function<void(std::string_view line)> sink);

// Whether Android's Bluetooth HAL is connected to a bridge of this session right now.
inline constexpr wchar_t kConnectedEvent[] = L"Local\\WSA-Next.Bluetooth.Connected";
bool AndroidConnected();

}  // namespace WN::Bt
