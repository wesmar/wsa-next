// WSA-Next © 2026 Marek Wesołowski - wesmar
// One LE connection to a remote device, made by Windows (WinRT BluetoothLEDevice + GattSession).
//
// Android's GATT client speaks ATT over the emulated link; LeLink answers from the device's real
// GATT database, read through WinRT with the device's own attribute handles: discovery (services,
// characteristics, descriptors) from a snapshot taken when the link comes up, reads and writes
// forwarded to the device, notifications and indications forwarded back. Windows performs the
// actual radio connection, MTU exchange and long reads/writes with the device.
//
// Requests are handled in order on the link's own thread (ATT allows one outstanding request).
//
// Pairing: Windows pairs with the device for real (its own UI when the device wants a PIN);
// towards Android the link answers SMP as a "Just Works" LE legacy pairing, so Android keeps a
// bond and may encrypt the link (the encryption itself is Windows' business with the device).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace WN::Bt {

// The two ends of a link as Android sees them (pairing needs both).
struct LinkAddresses {
    std::uint64_t own = 0;  // the host (initiator)
    bool ownRandom = false;
    std::uint64_t peer = 0;  // the device (responder)
    bool peerRandom = false;
};

class LeLink {
public:
    using SendFn = std::function<void(std::uint16_t cid, std::vector<std::uint8_t> payload)>;  // L2CAP to the host
    using ClosedFn = std::function<void(std::uint8_t reason)>;                                   // HCI disconnect reason

    // Connects to the device (blocking: call it on a worker thread). On failure returns nullptr
    // and sets `status` to an HCI error code.
    static std::unique_ptr<LeLink> Connect(const LinkAddresses& addresses, SendFn send, ClosedFn closed, std::uint8_t& status);

    // The host has encrypted the link (LE Start Encryption): pairing goes on with key distribution.
    void OnEncrypted();
    ~LeLink();
    LeLink(const LeLink&) = delete;
    LeLink& operator=(const LeLink&) = delete;

    // An L2CAP frame from the host on this link.
    void OnFrame(std::uint16_t cid, std::vector<std::uint8_t> payload);

    // The host disconnects: release the device (Windows drops the connection when nobody else uses it).
    // Returns once no WinRT handler of the link runs any more and none will: after Close the link
    // never calls `send` or `closed` again.
    void Close();

    // The device has disconnected (the `closed` callback was made or is being made).
    [[nodiscard]] bool Closed() const;

    struct Impl;  // shared with WinRT event handlers, which may still run while the link closes

private:
    explicit LeLink(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> m_impl;
};

}  // namespace WN::Bt
