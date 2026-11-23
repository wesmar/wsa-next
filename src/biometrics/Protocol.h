// WSA-Next © 2026 Marek Wesołowski - wesmar
// Wire protocol between the Windows Hello bridge (host) and Android's fingerprint HAL (guest)
// over a Hyper-V socket: the HAL listens on vsock port "WSFP", the bridge connects whenever WSA
// runs and keeps the connection.
//
// Every message is an 8-byte header followed by an optional UTF-8 payload (the prompt of a
// request). Must match android/device/wsa/wsa_x86_64/fingerprint/WsaFingerprint.cpp.
//   bridge -> HAL  Availability   on connecting and whenever Windows Hello becomes (un)available
//   HAL -> bridge  Authenticate   with the prompt; answered by an Authenticate message
//   HAL -> bridge  Cancel         closes the Windows Hello dialog; the answer is "Canceled"
#pragma once
#include <cstdint>

namespace WN::Bio {

inline constexpr std::uint32_t kBridgePort = 0x57534650;  // "WSFP"
inline constexpr std::uint32_t kMagic = 0x50465357;       // 'W','S','F','P' in little-endian
inline constexpr std::uint16_t kMaxPayload = 512;

enum class Op : std::uint8_t {
    // 1 and 2 (ping, enroll) belonged to the first, polling version.
    Authenticate = 3,
    Availability = 4,
    Cancel = 5,
};

enum class Status : std::uint8_t {
    Ok = 0,            // Availability: Windows Hello is ready
    Verified = 1,      // Authenticate: user verified by Windows Hello
    Canceled = 2,      // the dialog was dismissed or canceled
    NotAvailable = 3,  // Windows Hello not configured or no biometric/PIN device present
    Busy = 4,          // another verification is in progress
    Failed = 5,        // verification failed or protocol error
};

struct FrameHeader {
    std::uint32_t magic = kMagic;
    std::uint8_t op = 0;
    std::uint8_t status = 0;
    std::uint16_t payloadLen = 0;
};
static_assert(sizeof(FrameHeader) == 8);

}  // namespace WN::Bio
