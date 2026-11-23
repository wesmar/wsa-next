// WSA-Next © 2026 Marek Wesołowski - wesmar
// Windows Hello bridge: Android's fingerprint HAL in WSA <-> Hyper-V socket ("WSFP") <-> Windows Hello.
#pragma once
#include <stop_token>

namespace WN::Bio {

inline constexpr wchar_t kBridgeVersion[] = L"0.1";

enum class BridgeResult {
    Stopped,
    NotElevated,
    NotAvailable,
};

// Registers the Hyper-V guest communication service for "WSFP", listens for Android's fingerprint
// HAL and answers it until `stop` is requested: whether Windows Hello is available (Ping, every few
// seconds) and Windows Hello verifications (Authenticate). It serves also while Windows Hello is
// not set up, so Android notices when it is. Runs in the background host (src/host).
BridgeResult RunBridge(std::stop_token stop);

}  // namespace WN::Bio
