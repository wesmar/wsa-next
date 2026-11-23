// WSA-Next © 2026 Marek Wesołowski - wesmar
// Wire protocol between the elevation bridge (host) and Android's elevation daemon
// (wsa-elevated, guest) over a Hyper-V socket: the daemon listens on vsock port "WSEV", the bridge
// connects while WSA runs and root for Android apps is turned on in WSA-Next.
//
// An app asks for root with /system_ext/bin/0 (root is uid 0); the daemon learns who asks from the
// kernel (the socket's peer credentials), never from the app, and asks the bridge. The bridge
// shows Windows Hello with its own text naming the app and the command; only a verified answer
// lets the daemon start the process as root.
//
// Every message is an 8-byte header followed by a payload of at most kMaxPayload bytes.
// Must match android/device/wsa/wsa_x86_64/elevate/elevated.cpp.
//   daemon -> bridge  Request   u32 id, u32 uid, u16 length + package(s) (UTF-8), u16 length +
//                               command (UTF-8; empty: an interactive shell)
//   bridge -> daemon  Answer    status, u32 id
//   daemon -> bridge  Cancel    u32 id: the app gave up waiting; the dialog closes
//   bridge -> daemon  Config    status 1: "su for root apps" (the daemon links /dev/wsa/bin/su -> 0
//                               while the bridge stays connected), 0: no su
#pragma once
#include <cstdint>

namespace WN::Bio::Elevation {

inline constexpr std::uint32_t kBridgePort = 0x57534556;  // "WSEV"
inline constexpr std::uint32_t kMagic = 0x56455357;       // 'W','S','E','V' in little-endian
inline constexpr std::uint16_t kMaxPayload = 2048;

enum class Op : std::uint8_t {
    Request = 1,
    Answer = 2,
    Cancel = 3,
    Config = 4,
};

enum class Status : std::uint8_t {
    Granted = 1,       // Windows Hello verified the user
    Denied = 2,        // the user canceled, or verification failed
    NotAvailable = 3,  // Windows Hello is not set up
    Busy = 4,          // another request is being decided
};

struct FrameHeader {
    std::uint32_t magic = kMagic;
    std::uint8_t op = 0;
    std::uint8_t status = 0;
    std::uint16_t payloadLen = 0;
};
static_assert(sizeof(FrameHeader) == 8);

}  // namespace WN::Bio::Elevation
