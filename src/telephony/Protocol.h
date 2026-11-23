// WSA-Next © 2026 Marek Wesołowski - wesmar
// Telephony bridge: Android's RIL in WSA (Cuttlefish's AT-command RIL, listening on vsock) <->
// Hyper-V socket <-> the PC's modem through the Windows mobile broadband API. The bridge plays the
// modem: it answers the AT commands the RIL sends (27.007 / 27.005, PDU mode for SMS).
#pragma once
#include <cstdint>

namespace WN::Tel {

inline constexpr std::uint32_t kRilPort = 0x5753524C;  // "WSRL" (device/wsa/wsa_x86_64/telephony)
inline constexpr std::uint32_t kMmsPort = 0x57534D4D;  // "WSMM" (device/wsa/wsa_x86_64/telephony/wsa-mms.cpp)

}  // namespace WN::Tel
