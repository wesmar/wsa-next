// WSA-Next © 2026 Marek Wesołowski - wesmar
// A Bluetooth controller as Android's stack sees it, backed by Windows.
//
// It answers HCI commands the way a dual-mode Bluetooth 4.2 controller does (identity: the PC's
// adapter address) and turns WinRT results into HCI events: LE scanning becomes LE Advertising
// Reports; LE Create Connection opens a WinRT connection (LeLink) whose L2CAP traffic - ATT for
// GATT - travels as ACL data. Commands it does not implement are not in its Read Local Supported
// Commands mask and are answered with "Unknown HCI Command".
//
// Thread safety: OnCommand runs on the connection thread, OnAdvertisement on WinRT threads; the
// state is guarded by one mutex and every packet to the host goes through the send function.
#pragma once
#include "bluetooth/Hci.h"
#include "bluetooth/L2cap.h"
#include "bluetooth/LeLink.h"
#include "bluetooth/Scanner.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace WN::Bt {

class Controller {
public:
    using SendFn = std::function<void(Hci::PacketType, std::vector<std::uint8_t>)>;
    Controller(SendFn send, std::uint64_t address);
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;

    void OnCommand(std::span<const std::uint8_t> packet);
    void OnAcl(std::span<const std::uint8_t> packet);

private:
    using Handler = std::vector<std::uint8_t> (Controller::*)(std::span<const std::uint8_t> params);
    struct Entry {
        Handler handler;  // returns the Command Complete parameters after the status byte
    };

    void BuildCommandTable();
    void CommandComplete(std::uint16_t opcode, std::uint8_t status, std::span<const std::uint8_t> params);
    void CommandStatus(std::uint16_t opcode, std::uint8_t status);
    void Event(std::uint8_t code, std::span<const std::uint8_t> params);
    void LeMeta(std::uint8_t subevent, std::span<const std::uint8_t> params);
    void OnAdvertisement(const Advertisement& report);

    // Connections (commands answered with Command Status, results as their own events).
    bool OnConnectionCommand(std::uint16_t opcode, std::span<const std::uint8_t> params);
    void LeCreateConnection(std::span<const std::uint8_t> params);
    void Disconnect(std::span<const std::uint8_t> params);
    void OnLinkClosed(std::uint16_t handle, std::uint8_t reason);
    void DestroyLater(std::unique_ptr<LeLink> link);
    void CancelConnect();
    [[nodiscard]] bool HasLink(std::uint16_t handle);

    // Command handlers (std::nullopt-free: an empty vector means "status only").
    std::vector<std::uint8_t> Ok(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> Reset(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> ReadLocalName(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> WriteLocalName(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> ReadScanEnable(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> ReadClassOfDevice(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> ReadVoiceSetting(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> ReadNumberOfSupportedIac(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> ReadCurrentIacLap(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> ReadInquiryResponseTxPower(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> ReadLeHostSupport(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> ReadDefaultLinkPolicy(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> ReadLocalVersion(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> ReadLocalCommands(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> ReadLocalFeatures(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> ReadLocalExtendedFeatures(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> ReadBufferSize(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> ReadBdAddr(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> LeReadBufferSize(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> LeReadLocalFeatures(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> LeReadAdvertisingTxPower(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> LeSetScanParameters(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> LeSetScanEnable(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> LeReadAcceptListSize(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> LeEncrypt(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> LeRand(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> LeReadSupportedStates(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> LeCreateConnectionCancel(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> ReadRssi(std::span<const std::uint8_t>);
    std::vector<std::uint8_t> LeSetRandomAddress(std::span<const std::uint8_t>);

    SendFn m_send;
    std::uint64_t m_address;
    std::map<std::uint16_t, Entry> m_commands;
    std::array<std::uint8_t, 64> m_supportedCommands{};

    std::mutex m_lock;
    std::string m_localName = "Android";
    bool m_activeScan = true;
    bool m_scanning = false;
    bool m_filterDuplicates = false;
    std::map<std::pair<std::uint64_t, std::uint8_t>, std::vector<std::uint8_t>> m_reported;  // duplicate filter
    std::map<std::uint64_t, std::int8_t> m_lastRssi;  // advertising RSSI per address (Read RSSI of a link)
    std::map<std::uint64_t, std::string> m_names;     // advertised names (Remote Name Request)
    Scanner m_scanner;

    // LE links: handle -> WinRT connection, its ACL reassembly and peer address.
    struct Link {
        std::unique_ptr<LeLink> link;
        AclReassembler reassembler;
        std::uint64_t address = 0;
    };
    std::mutex m_linksLock;
    std::map<std::uint16_t, Link> m_links;
    std::uint16_t m_nextHandle = 0x0040;
    std::atomic<bool> m_connecting{false}, m_connectCancel{false};
    std::mutex m_connectLock;                 // with m_connectChanged: m_connectCancel was set
    std::condition_variable m_connectChanged;
    std::atomic<unsigned> m_generation{0};  // advanced by HCI Reset
    std::atomic<std::uint64_t> m_randomAddress{0};  // LE Set Random Address (own address type 1)

    // Links being closed on their own threads (DestroyLater). They call back into the controller
    // until closed, so the destructor waits for them.
    std::mutex m_retiringLock;
    std::condition_variable m_retired;
    unsigned m_retiring = 0;

    // Threads last: joined before the members they use go away.
    std::jthread m_connector;  // LE Create Connection in progress
    std::jthread m_inquiry;    // ends a classic inquiry after its length
};

}  // namespace WN::Bt
