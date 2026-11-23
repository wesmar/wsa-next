// WSA-Next © 2026 Marek Wesołowski - wesmar
#include "bluetooth/Controller.h"
#include "bluetooth/Log.h"
#include "bluetooth/Bytes.h"
#include "bluetooth/Crypto.h"
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <thread>


namespace WN::Bt {
namespace {

// Identity: a dual-mode Bluetooth 4.2 controller. LMP feature page 0 is the one of a real 4.2
// dual-mode controller as recorded in RootCanal's CSR_RCK_PTS_DONGLE preset (Apache 2.0); LE
// features: encryption, connection parameters request, extended reject, peripheral-initiated
// feature exchange, LE ping (no extended advertising: legacy scanning and reports).
constexpr std::uint8_t kHciVersion = 0x08;  // 4.2
constexpr std::uint16_t kHciRevision = 0x0001;
constexpr std::uint8_t kLmpVersion = 0x08;
constexpr std::uint16_t kCompany = 0xFFFF;  // "for internal use": not a real chip vendor
constexpr std::uint16_t kLmpSubversion = 0x0001;
constexpr std::uint64_t kLmpFeaturesPage0 = 0x875b1fd87e8fffffULL;
constexpr std::uint64_t kLmpFeaturesPage2 = 0x0300;  // secure connections (controller), ping
constexpr std::uint64_t kLeFeatures = 0x1F;
constexpr std::uint64_t kLeSupportedStates = 0x3ffffffffffULL;
constexpr std::uint16_t kAclLength = 1021, kAclCount = 8;
constexpr std::uint8_t kScoLength = 64;
constexpr std::uint16_t kScoCount = 8;
constexpr std::uint16_t kLeAclLength = 251;
constexpr std::uint8_t kLeAclCount = 8;
constexpr std::uint8_t kAcceptListSize = 16;

constexpr std::uint16_t kInquiry = 0x0401, kInquiryCancel = 0x0402;
constexpr std::uint16_t kLeCreateConnection = 0x200D, kDisconnect = 0x0406, kLeReadRemoteFeatures = 0x2016, kReadRemoteVersion = 0x041D,
                        kLeConnectionUpdate = 0x2013, kLeStartEncryption = 0x2019;
constexpr std::uint8_t kEvtEncryptionChange = 0x08;
constexpr std::uint16_t kRemoteNameRequest = 0x0419, kRemoteNameRequestCancel = 0x041A;
constexpr std::uint8_t kEvtRemoteNameRequestComplete = 0x07, kPageTimeout = 0x04;

// The device name in advertising data (Complete or Shortened Local Name), empty when absent.
std::string AdvertisedName(std::span<const std::uint8_t> data) {
    std::string shortened;
    for (std::size_t i = 0; i + 1 < data.size();) {
        const std::size_t length = data[i];
        if (length == 0 || i + 1 + length > data.size()) break;
        const std::uint8_t type = data[i + 1];
        const auto value = data.subspan(i + 2, length - 1);
        if (type == 0x09) return {value.begin(), value.end()};
        if (type == 0x08) shortened.assign(value.begin(), value.end());
        i += 1 + length;
    }
    return shortened;
}

// Events and status codes of connections.
constexpr std::uint8_t kEvtDisconnectionComplete = 0x05, kEvtReadRemoteVersionComplete = 0x0C, kEvtNumberOfCompletedPackets = 0x13;
constexpr std::uint8_t kLeConnectionComplete = 0x01, kLeConnectionUpdateComplete = 0x03, kLeReadRemoteFeaturesComplete = 0x04;
constexpr std::uint8_t kUnknownConnection = 0x02, kHardwareFailure = 0x03, kLocalHostTerminated = 0x16, kConnectionTimeout = 0x08;

// Commands answered with Command Status (the result follows as its own event).
bool ExpectsStatus(std::uint16_t opcode) {
    const std::uint16_t ogf = opcode >> 10;
    if (ogf == 0x01) {  // link control: all but the "cancel" and "reply" commands
        static constexpr std::uint16_t kComplete[] = {0x0402, 0x0404, 0x0408, 0x040B, 0x040C, 0x040D, 0x040E, 0x0412,
                                                      0x041A, 0x041B, 0x042B, 0x042C, 0x042D, 0x042E, 0x0434, 0x0443};
        return std::ranges::find(kComplete, opcode) == std::end(kComplete);
    }
    static constexpr std::uint16_t kLeStatus[] = {0x200D, 0x2013, 0x2016, 0x2019, 0x2025, 0x2026, 0x2032, 0x2043, 0x2044};
    return std::ranges::find(kLeStatus, opcode) != std::end(kLeStatus);
}


// A command handler's way to fail with a specific HCI status.
struct HciStatus {
    std::uint8_t code;
};

}  // namespace

Controller::Controller(SendFn send, std::uint64_t address)
    : m_send(std::move(send)), m_address(address), m_scanner([this](const Advertisement& a) { OnAdvertisement(a); }) {
    BuildCommandTable();
}

Controller::~Controller() {
    m_scanner.Stop();
    m_inquiry.request_stop();
    CancelConnect();
    if (m_connector.joinable()) m_connector.join();
    std::map<std::uint16_t, Link> links;
    {
        std::lock_guard guard(m_linksLock);
        links.swap(m_links);
    }
    for (auto& [handle, link] : links) link.link->Close();  // the host is gone: release the devices
    // Links closing on their own threads may still call back: wait until they are closed.
    std::unique_lock guard(m_retiringLock);
    m_retired.wait(guard, [this] { return m_retiring == 0; });
}

void Controller::CancelConnect() {
    {
        std::lock_guard guard(m_connectLock);
        m_connectCancel = true;
    }
    m_connectChanged.notify_all();
}

void Controller::BuildCommandTable() {
    const Handler ok = &Controller::Ok;
    struct Row {
        std::uint16_t opcode;
        Handler handler;
    };
    const Row rows[] = {
        // Link policy
        {0x080E, &Controller::ReadDefaultLinkPolicy},
        {0x080F, ok},  // write default link policy settings
        // Controller and baseband
        {0x0C01, ok},  // set event mask
        {0x0C03, &Controller::Reset},
        {0x0C05, ok},  // set event filter
        {0x0C13, &Controller::WriteLocalName},
        {0x0C14, &Controller::ReadLocalName},
        {0x0C16, ok},  // write connection accept timeout
        {0x0C18, ok},  // write page timeout
        {0x0C19, &Controller::ReadScanEnable},
        {0x0C1A, ok},  // write scan enable
        {0x0C1C, ok},  // write page scan activity
        {0x0C1E, ok},  // write inquiry scan activity
        {0x0C23, &Controller::ReadClassOfDevice},
        {0x0C24, ok},  // write class of device
        {0x0C25, &Controller::ReadVoiceSetting},
        {0x0C26, ok},  // write voice setting
        {0x0C33, ok},  // host buffer size
        {0x0C38, &Controller::ReadNumberOfSupportedIac},
        {0x0C39, &Controller::ReadCurrentIacLap},
        {0x0C3A, ok},  // write current IAC LAP
        {0x0C43, ok},  // write inquiry scan type
        {0x0C45, ok},  // write inquiry mode
        {0x0C47, ok},  // write page scan type
        {0x0C52, ok},  // write extended inquiry response
        {0x0C56, ok},  // write simple pairing mode
        {0x0C58, &Controller::ReadInquiryResponseTxPower},
        {0x0C63, ok},  // set event mask page 2
        {0x0C6C, &Controller::ReadLeHostSupport},
        {0x0C6D, ok},  // write LE host support
        {0x0C7A, ok},  // write secure connections host support
        // Informational
        {0x1001, &Controller::ReadLocalVersion},
        {0x1002, &Controller::ReadLocalCommands},
        {0x1003, &Controller::ReadLocalFeatures},
        {0x1004, &Controller::ReadLocalExtendedFeatures},
        {0x1005, &Controller::ReadBufferSize},
        {0x1009, &Controller::ReadBdAddr},
        // LE
        {0x2001, ok},  // LE set event mask
        {0x2002, &Controller::LeReadBufferSize},
        {0x2003, &Controller::LeReadLocalFeatures},
        {0x2005, &Controller::LeSetRandomAddress},
        {0x2006, ok},  // LE set advertising parameters
        {0x2007, &Controller::LeReadAdvertisingTxPower},
        {0x2008, ok},  // LE set advertising data
        {0x2009, ok},  // LE set scan response data
        {0x200A, ok},  // LE set advertising enable
        {0x200B, &Controller::LeSetScanParameters},
        {0x200C, &Controller::LeSetScanEnable},
        {0x200F, &Controller::LeReadAcceptListSize},
        {0x2010, ok},  // LE clear filter accept list
        {0x2011, ok},  // LE add device to filter accept list
        {0x2012, ok},  // LE remove device from filter accept list
        {0x2017, &Controller::LeEncrypt},
        {0x2018, &Controller::LeRand},
        {0x201C, &Controller::LeReadSupportedStates},
        {0x200E, &Controller::LeCreateConnectionCancel},
        {0x1405, &Controller::ReadRssi},
    };
    auto mark = [this](std::uint16_t opcode) {
        if (const Hci::OpcodeInfo* info = Hci::FindOpcode(opcode)) m_supportedCommands[info->octet] |= static_cast<std::uint8_t>(1u << info->bit);
    };
    for (const Row& row : rows) {
        m_commands.emplace(row.opcode, Entry{row.handler});
        mark(row.opcode);
    }
    mark(kInquiry);
    mark(kInquiryCancel);
    for (std::uint16_t opcode : {kLeCreateConnection, kDisconnect, kLeReadRemoteFeatures, kReadRemoteVersion, kLeConnectionUpdate, kLeStartEncryption,
                                 kRemoteNameRequest, kRemoteNameRequestCancel})
        mark(opcode);
}

// ---------------------------------------------------------------------------------------------
// Packets from the host
// ---------------------------------------------------------------------------------------------

void Controller::OnCommand(std::span<const std::uint8_t> packet) {
    if (packet.size() < 3) return;
    const auto opcode = static_cast<std::uint16_t>(packet[0] | (packet[1] << 8));
    const auto params = packet.subspan(3, std::min<std::size_t>(packet[2], packet.size() - 3));

    if (opcode == kInquiry) {  // classic discovery: nothing to find yet, end it after its length
        const std::uint8_t length = params.size() >= 4 ? params[3] : 1;
        Bytes status;
        status.u8(Hci::kEvtCommandStatus).u8(4).u8(Hci::kSuccess).u8(1).u16(opcode);
        m_send(Hci::PacketType::Event, status.take());
        const auto duration = std::chrono::milliseconds(std::min<int>(length * 1280, 10240));
        if (m_inquiry.joinable()) {  // a new inquiry replaces one still running
            m_inquiry.request_stop();
            m_inquiry.join();
        }
        m_inquiry = std::jthread([this, duration](std::stop_token stop) {
            // The inquiry length is the protocol's own timeout; a cancellation ends the wait at once.
            std::mutex lock;
            std::condition_variable_any nothing;
            std::unique_lock guard(lock);
            (void)nothing.wait_for(guard, stop, duration, [] { return false; });
            if (stop.stop_requested()) return;
            Bytes complete;
            complete.u8(Hci::kEvtInquiryComplete).u8(1).u8(Hci::kSuccess);
            m_send(Hci::PacketType::Event, complete.take());
        });
        return;
    }
    if (opcode == kInquiryCancel) {
        m_inquiry.request_stop();
        if (m_inquiry.joinable()) m_inquiry.join();
        CommandComplete(opcode, Hci::kSuccess, {});
        return;
    }

    try {
        if (OnConnectionCommand(opcode, params)) return;
    } catch (const std::out_of_range&) {
        CommandStatus(opcode, Hci::kInvalidParameters);
        return;
    }

    const auto it = m_commands.find(opcode);
    if (it == m_commands.end()) {
        Log("hci: unsupported command 0x{:04X} {} params {}", opcode, Hci::OpcodeName(opcode), Hex(params.data(), params.size()));
        if (ExpectsStatus(opcode)) {
            Bytes status;
            status.u8(Hci::kEvtCommandStatus).u8(4).u8(Hci::kUnknownCommand).u8(1).u16(opcode);
            m_send(Hci::PacketType::Event, status.take());
        } else {
            CommandComplete(opcode, Hci::kUnknownCommand, {});
        }
        return;
    }
    std::vector<std::uint8_t> result;
    std::uint8_t status = Hci::kSuccess;
    try {
        result = (this->*(it->second.handler))(params);
    } catch (const std::out_of_range&) {
        status = Hci::kInvalidParameters;
        result.clear();
    } catch (const HciStatus& error) {
        status = error.code;
        result.clear();
    } catch (const std::exception& e) {
        Log("hci: command 0x{:04X} failed: {}", opcode, e.what());
        status = kHardwareFailure;
        result.clear();
    }
    CommandComplete(opcode, status, result);
}

void Controller::OnAcl(std::span<const std::uint8_t> packet) {
    const std::uint16_t handle = AclHandle(packet);
    if (handle == 0xFFFF) return;
    // The packet is "transmitted" at once: give the host its buffer credit back.
    Event(kEvtNumberOfCompletedPackets, Bytes().u8(1).u16(handle).u16(1).data());
    std::optional<L2capFrame> frame;
    LeLink* link = nullptr;
    {
        std::lock_guard guard(m_linksLock);
        const auto it = m_links.find(handle);
        if (it == m_links.end()) return;
        frame = it->second.reassembler.Feed(packet);
        link = it->second.link.get();
        if (frame) link->OnFrame(frame->cid, std::move(frame->payload));
    }
}

// ---------------------------------------------------------------------------------------------
// Connections
// ---------------------------------------------------------------------------------------------

bool Controller::OnConnectionCommand(std::uint16_t opcode, std::span<const std::uint8_t> params) {
    switch (opcode) {
    case kLeCreateConnection: LeCreateConnection(params); return true;
    case kDisconnect: Disconnect(params); return true;
    case kLeReadRemoteFeatures: {
        Reader r(params);
        const std::uint16_t handle = static_cast<std::uint16_t>(r.u16() & 0x0FFF);  // 12-bit connection handle
        if (!HasLink(handle)) {
            CommandStatus(opcode, kUnknownConnection);
            return true;
        }
        CommandStatus(opcode, Hci::kSuccess);
        LeMeta(kLeReadRemoteFeaturesComplete, Bytes().u8(Hci::kSuccess).u16(handle).le(kLeFeatures, 8).data());
        return true;
    }
    case kReadRemoteVersion: {
        Reader r(params);
        const std::uint16_t handle = static_cast<std::uint16_t>(r.u16() & 0x0FFF);  // 12-bit connection handle
        if (!HasLink(handle)) {
            CommandStatus(opcode, kUnknownConnection);
            return true;
        }
        CommandStatus(opcode, Hci::kSuccess);
        Event(kEvtReadRemoteVersionComplete, Bytes().u8(Hci::kSuccess).u16(handle).u8(kLmpVersion).u16(kCompany).u16(0).data());
        return true;
    }
    case kRemoteNameRequest: {
        // Answered from what the devices advertise (no classic paging through Windows).
        Reader r(params);
        const std::uint64_t peer = r.le(6);
        std::string name;
        {
            std::lock_guard guard(m_lock);
            if (const auto it = m_names.find(peer); it != m_names.end()) name = it->second;
        }
        CommandStatus(opcode, Hci::kSuccess);
        Bytes complete;
        complete.u8(name.empty() ? kPageTimeout : Hci::kSuccess).le(peer, 6);
        std::vector<std::uint8_t> field(248, 0);
        std::copy_n(name.begin(), std::min<std::size_t>(name.size(), 247), field.begin());
        complete.raw(field);
        Event(kEvtRemoteNameRequestComplete, complete.data());
        return true;
    }
    case kRemoteNameRequestCancel: {
        Reader r(params);
        const std::uint64_t peer = r.le(6);
        CommandComplete(opcode, Hci::kSuccess, Bytes().le(peer, 6).data());
        return true;
    }
    case kLeStartEncryption: {
        // Android encrypts with the key of its bond (or the pairing's STK). The radio link is
        // Windows' and is encrypted by Windows when the device needs it: report success.
        Reader r(params);
        const std::uint16_t handle = static_cast<std::uint16_t>(r.u16() & 0x0FFF);  // 12-bit connection handle
        LeLink* link = nullptr;
        {
            std::lock_guard guard(m_linksLock);
            if (const auto it = m_links.find(handle); it != m_links.end()) link = it->second.link.get();
            if (!link) {
                CommandStatus(opcode, kUnknownConnection);
                return true;
            }
            CommandStatus(opcode, Hci::kSuccess);
            Event(kEvtEncryptionChange, Bytes().u8(Hci::kSuccess).u16(handle).u8(0x01).data());
            link->OnEncrypted();
        }
        return true;
    }
    case kLeConnectionUpdate: {
        // Windows chooses the connection parameters with the device; report the request as done.
        Reader r(params);
        const std::uint16_t handle = static_cast<std::uint16_t>(r.u16() & 0x0FFF), minInterval = r.u16(), maxInterval = r.u16(), latency = r.u16(), timeout = r.u16();
        (void)minInterval;
        if (!HasLink(handle)) {
            CommandStatus(opcode, kUnknownConnection);
            return true;
        }
        CommandStatus(opcode, Hci::kSuccess);
        LeMeta(kLeConnectionUpdateComplete, Bytes().u8(Hci::kSuccess).u16(handle).u16(maxInterval).u16(latency).u16(timeout).data());
        return true;
    }
    default: return false;
    }
}

void Controller::LeCreateConnection(std::span<const std::uint8_t> params) {
    Reader r(params);
    (void)r.u16();  // scan interval
    (void)r.u16();  // scan window
    const std::uint8_t filterPolicy = r.u8();
    const std::uint8_t peerType = r.u8();
    const std::uint64_t peer = r.le(6);
    const std::uint8_t ownType = r.u8();  // 0 public, 1 random (2/3: controller-generated RPAs, not offered)
    const std::uint16_t interval = r.u16();
    (void)r.u16();  // maximum interval
    const std::uint16_t latency = r.u16(), timeout = r.u16();

    if (m_connecting.exchange(true)) {
        CommandStatus(kLeCreateConnection, Hci::kCommandDisallowed);
        return;
    }
    m_connectCancel = false;
    CommandStatus(kLeCreateConnection, Hci::kSuccess);
    const bool acceptList = filterPolicy != 0;
    if (m_connector.joinable()) m_connector.join();  // the previous attempt has reported already
    const unsigned generation = m_generation;
    m_connector = std::jthread([this, generation, acceptList, ownType, peerType, peer, interval, latency, timeout](std::stop_token) {
        std::uint8_t status = kUnknownConnection;
        std::unique_ptr<LeLink> link;
        std::uint16_t handle = 0;
        if (acceptList) {
            // Background connections to bonded devices come with pairing; until then the request
            // waits for its cancellation.
            std::unique_lock guard(m_connectLock);
            m_connectChanged.wait(guard, [this] { return m_connectCancel.load(); });
        } else {
            {
                std::lock_guard guard(m_linksLock);
                handle = m_nextHandle;
                m_nextHandle = static_cast<std::uint16_t>(m_nextHandle >= 0x0EFF ? 0x0040 : m_nextHandle + 1);
            }
            Log("le: connecting to {:012X}", peer);
            auto send = [this, handle](std::uint16_t cid, std::vector<std::uint8_t> payload) {
                for (auto& packet : AclPackets(handle, cid, payload, kLeAclLength)) m_send(Hci::PacketType::Acl, std::move(packet));
            };
            const LinkAddresses addresses{.own = (ownType & 0x01) ? m_randomAddress.load() : m_address,
                                          .ownRandom = (ownType & 0x01) != 0,
                                          .peer = peer,
                                          .peerRandom = (peerType & 0x01) != 0};
            link = LeLink::Connect(addresses, send, [this, handle](std::uint8_t reason) { OnLinkClosed(handle, reason); }, status);
            if (link && m_connectCancel) {
                DestroyLater(std::move(link));
                status = kUnknownConnection;
            }
        }
        if (generation != m_generation) {  // a reset came in between: nobody waits for this
            if (link) DestroyLater(std::move(link));
            m_connecting = false;
            return;
        }
        Bytes complete;
        complete.u8(status).u16(link ? handle : 0).u8(0x00 /* central */).u8(peerType & 0x01).le(peer, 6);
        complete.u16(link ? interval : 0).u16(link ? latency : 0).u16(link ? timeout : 0).u8(0x00);
        std::unique_ptr<LeLink> lost;  // the device left before the link was listed
        if (link) {
            std::lock_guard guard(m_linksLock);
            if (link->Closed()) {
                // Its OnLinkClosed found no handle; under the lock a later one finds it listed.
                lost = std::move(link);
            } else {
                m_links[handle] = Link{std::move(link), {}, peer};
            }
            Log("le: connected to {:012X} as handle 0x{:04X}", peer, handle);
        } else {
            Log("le: connection to {:012X} not established (status 0x{:02X})", peer, status);
        }
        LeMeta(kLeConnectionComplete, complete.data());
        if (lost) {
            DestroyLater(std::move(lost));
            Event(kEvtDisconnectionComplete, Bytes().u8(Hci::kSuccess).u16(handle).u8(kConnectionTimeout).data());
        }
        m_connecting = false;
    });
}

std::vector<std::uint8_t> Controller::LeCreateConnectionCancel(std::span<const std::uint8_t>) {
    if (!m_connecting) throw HciStatus{Hci::kCommandDisallowed};
    CancelConnect();
    return {};
}

void Controller::Disconnect(std::span<const std::uint8_t> params) {
    Reader r(params);
    const std::uint16_t handle = static_cast<std::uint16_t>(r.u16() & 0x0FFF);
    std::unique_ptr<LeLink> link;
    {
        std::lock_guard guard(m_linksLock);
        if (const auto it = m_links.find(handle); it != m_links.end()) {
            link = std::move(it->second.link);
            m_links.erase(it);
        }
    }
    if (!link) {
        CommandStatus(kDisconnect, kUnknownConnection);
        return;
    }
    CommandStatus(kDisconnect, Hci::kSuccess);
    Log("le: disconnecting handle 0x{:04X}", handle);
    DestroyLater(std::move(link));
    Event(kEvtDisconnectionComplete, Bytes().u8(Hci::kSuccess).u16(handle).u8(kLocalHostTerminated).data());
}

void Controller::OnLinkClosed(std::uint16_t handle, std::uint8_t reason) {
    std::unique_ptr<LeLink> link;
    {
        std::lock_guard guard(m_linksLock);
        if (const auto it = m_links.find(handle); it != m_links.end()) {
            link = std::move(it->second.link);
            m_links.erase(it);
        }
    }
    if (!link) return;  // the host disconnected first
    DestroyLater(std::move(link));
    Event(kEvtDisconnectionComplete, Bytes().u8(Hci::kSuccess).u16(handle).u8(reason).data());
}

// Closing a link waits for its thread and WinRT; never do that on the HCI or a WinRT thread.
void Controller::DestroyLater(std::unique_ptr<LeLink> link) {
    if (!link) return;
    {
        std::lock_guard guard(m_retiringLock);
        ++m_retiring;
    }
    std::thread([this, link = std::move(link)]() mutable {
        link.reset();  // after this the link never calls back
        // Notified under the lock: the destructor cannot return (and free the lock) before.
        std::lock_guard guard(m_retiringLock);
        if (--m_retiring == 0) m_retired.notify_all();
    }).detach();
}

bool Controller::HasLink(std::uint16_t handle) {
    std::lock_guard guard(m_linksLock);
    return m_links.contains(handle & 0x0FFF);
}

std::vector<std::uint8_t> Controller::ReadRssi(std::span<const std::uint8_t> params) {
    Reader r(params);
    const std::uint16_t handle = static_cast<std::uint16_t>(r.u16() & 0x0FFF);  // 12-bit connection handle
    std::uint64_t address = 0;
    {
        std::lock_guard guard(m_linksLock);
        const auto it = m_links.find(handle);
        if (it == m_links.end()) throw HciStatus{kUnknownConnection};
        address = it->second.address;
    }
    std::lock_guard guard(m_lock);
    const auto rssi = m_lastRssi.find(address);
    return Bytes().u16(handle).u8(static_cast<std::uint8_t>(rssi != m_lastRssi.end() ? rssi->second : -60)).take();
}

// ---------------------------------------------------------------------------------------------
// Events to the host
// ---------------------------------------------------------------------------------------------

void Controller::CommandComplete(std::uint16_t opcode, std::uint8_t status, std::span<const std::uint8_t> params) {
    // Unsuccessful commands return only the status; "unknown command" has nothing else to say.
    const bool withParams = status == Hci::kSuccess;
    Bytes event;
    event.u8(Hci::kEvtCommandComplete).u8(static_cast<std::uint8_t>(4 + (withParams ? params.size() : 0)));
    event.u8(1).u16(opcode).u8(status);
    if (withParams) event.raw(params);
    m_send(Hci::PacketType::Event, event.take());
}

void Controller::CommandStatus(std::uint16_t opcode, std::uint8_t status) {
    Bytes event;
    event.u8(Hci::kEvtCommandStatus).u8(4).u8(status).u8(1).u16(opcode);
    m_send(Hci::PacketType::Event, event.take());
}

void Controller::Event(std::uint8_t code, std::span<const std::uint8_t> params) {
    Bytes event;
    event.u8(code).u8(static_cast<std::uint8_t>(params.size())).raw(params);
    m_send(Hci::PacketType::Event, event.take());
}

void Controller::LeMeta(std::uint8_t subevent, std::span<const std::uint8_t> params) {
    Bytes event;
    event.u8(Hci::kEvtLeMeta).u8(static_cast<std::uint8_t>(1 + params.size())).u8(subevent).raw(params);
    m_send(Hci::PacketType::Event, event.take());
}

void Controller::OnAdvertisement(const Advertisement& report) {
    {
        std::lock_guard guard(m_lock);
        m_lastRssi[report.address] = report.rssi;
        if (std::string name = AdvertisedName(report.data); !name.empty()) m_names[report.address] = std::move(name);
        if (!m_scanning) return;
        if (!m_activeScan && report.eventType == Hci::kScanRsp) return;
        if (m_filterDuplicates) {
            auto& last = m_reported[{report.address, report.eventType}];
            if (last == report.data && !last.empty()) return;
            last = report.data;
        }
    }
    Bytes params;
    params.u8(1)  // one report
        .u8(report.eventType)
        .u8(report.randomAddress ? 0x01 : 0x00)
        .le(report.address, 6)
        .u8(static_cast<std::uint8_t>(report.data.size()))
        .raw(report.data)
        .u8(static_cast<std::uint8_t>(report.rssi));
    LeMeta(Hci::kLeAdvertisingReport, params.data());
}

// ---------------------------------------------------------------------------------------------
// Command handlers: the Command Complete parameters after the status
// ---------------------------------------------------------------------------------------------

std::vector<std::uint8_t> Controller::Ok(std::span<const std::uint8_t>) { return {}; }

std::vector<std::uint8_t> Controller::Reset(std::span<const std::uint8_t>) {
    m_scanner.Stop();
    m_inquiry.request_stop();
    // Connections end with a reset (the HAL sends one when Bluetooth is turned off); a connection
    // attempt in progress is dropped without an event (its generation is gone).
    ++m_generation;
    CancelConnect();
    {
        std::lock_guard guard(m_linksLock);
        for (auto& entry : m_links) DestroyLater(std::move(entry.second.link));
        m_links.clear();
    }
    std::lock_guard guard(m_lock);
    m_scanning = false;
    m_filterDuplicates = false;
    m_activeScan = true;
    m_reported.clear();
    Log("hci: reset");
    return {};
}

std::vector<std::uint8_t> Controller::ReadLocalName(std::span<const std::uint8_t>) {
    std::lock_guard guard(m_lock);
    std::vector<std::uint8_t> name(248, 0);
    std::copy_n(m_localName.begin(), std::min<std::size_t>(m_localName.size(), 247), name.begin());
    return name;
}

std::vector<std::uint8_t> Controller::WriteLocalName(std::span<const std::uint8_t> params) {
    Reader r(params);
    const auto bytes = r.bytes(std::min<std::size_t>(248, params.size()));
    std::lock_guard guard(m_lock);
    m_localName.assign(bytes.begin(), std::ranges::find(bytes, std::uint8_t{0}));
    Log("hci: local name \"{}\"", m_localName);
    return {};
}

std::vector<std::uint8_t> Controller::ReadScanEnable(std::span<const std::uint8_t>) { return {0x00}; }

std::vector<std::uint8_t> Controller::ReadClassOfDevice(std::span<const std::uint8_t>) {
    return Bytes().le(0x0C010C, 3).take();  // computer, laptop
}

std::vector<std::uint8_t> Controller::ReadVoiceSetting(std::span<const std::uint8_t>) { return Bytes().u16(0x0060).take(); }

std::vector<std::uint8_t> Controller::ReadNumberOfSupportedIac(std::span<const std::uint8_t>) { return {0x01}; }

std::vector<std::uint8_t> Controller::ReadCurrentIacLap(std::span<const std::uint8_t>) {
    return Bytes().u8(1).le(0x9E8B33, 3).take();  // general inquiry access code
}

std::vector<std::uint8_t> Controller::ReadInquiryResponseTxPower(std::span<const std::uint8_t>) { return {0x00}; }

std::vector<std::uint8_t> Controller::ReadLeHostSupport(std::span<const std::uint8_t>) { return {0x01, 0x00}; }

std::vector<std::uint8_t> Controller::ReadDefaultLinkPolicy(std::span<const std::uint8_t>) { return Bytes().u16(0x0005).take(); }

std::vector<std::uint8_t> Controller::ReadLocalVersion(std::span<const std::uint8_t>) {
    return Bytes().u8(kHciVersion).u16(kHciRevision).u8(kLmpVersion).u16(kCompany).u16(kLmpSubversion).take();
}

std::vector<std::uint8_t> Controller::ReadLocalCommands(std::span<const std::uint8_t>) {
    return {m_supportedCommands.begin(), m_supportedCommands.end()};
}

std::vector<std::uint8_t> Controller::ReadLocalFeatures(std::span<const std::uint8_t>) { return Bytes().le(kLmpFeaturesPage0, 8).take(); }

std::vector<std::uint8_t> Controller::ReadLocalExtendedFeatures(std::span<const std::uint8_t> params) {
    Reader r(params);
    const std::uint8_t page = r.u8();
    // Page 1 holds the host's features (the host sets them through its own commands).
    const std::uint64_t features = page == 0 ? kLmpFeaturesPage0 : page == 1 ? 0x0F : page == 2 ? kLmpFeaturesPage2 : 0;
    return Bytes().u8(page).u8(2).le(features, 8).take();
}

std::vector<std::uint8_t> Controller::ReadBufferSize(std::span<const std::uint8_t>) {
    return Bytes().u16(kAclLength).u8(kScoLength).u16(kAclCount).u16(kScoCount).take();
}

std::vector<std::uint8_t> Controller::ReadBdAddr(std::span<const std::uint8_t>) { return Bytes().le(m_address, 6).take(); }

std::vector<std::uint8_t> Controller::LeReadBufferSize(std::span<const std::uint8_t>) {
    return Bytes().u16(kLeAclLength).u8(kLeAclCount).take();
}

std::vector<std::uint8_t> Controller::LeReadLocalFeatures(std::span<const std::uint8_t>) { return Bytes().le(kLeFeatures, 8).take(); }

std::vector<std::uint8_t> Controller::LeReadAdvertisingTxPower(std::span<const std::uint8_t>) { return {0x00}; }

std::vector<std::uint8_t> Controller::LeSetScanParameters(std::span<const std::uint8_t> params) {
    Reader r(params);
    const std::uint8_t type = r.u8();
    std::lock_guard guard(m_lock);
    m_activeScan = type == 0x01;
    return {};
}

std::vector<std::uint8_t> Controller::LeSetScanEnable(std::span<const std::uint8_t> params) {
    Reader r(params);
    const bool enable = r.u8() != 0;
    const bool filter = r.u8() != 0;
    bool active = true;
    {
        std::lock_guard guard(m_lock);
        m_scanning = enable;
        m_filterDuplicates = filter;
        m_reported.clear();
        active = m_activeScan;
    }
    if (enable) m_scanner.Start(active);
    else m_scanner.Stop();
    return {};
}

std::vector<std::uint8_t> Controller::LeReadAcceptListSize(std::span<const std::uint8_t>) { return {kAcceptListSize}; }

std::vector<std::uint8_t> Controller::LeEncrypt(std::span<const std::uint8_t> params) {
    Reader r(params);
    Block key, plaintext;
    std::ranges::copy(r.bytes(16), key.begin());
    std::ranges::copy(r.bytes(16), plaintext.begin());
    const Block result = SecurityE(key, plaintext);
    return {result.begin(), result.end()};
}

std::vector<std::uint8_t> Controller::LeRand(std::span<const std::uint8_t>) {
    std::vector<std::uint8_t> random(8);
    RandomBytes(random);
    return random;
}

std::vector<std::uint8_t> Controller::LeSetRandomAddress(std::span<const std::uint8_t> params) {
    Reader r(params);
    m_randomAddress = r.le(6);
    return {};
}

std::vector<std::uint8_t> Controller::LeReadSupportedStates(std::span<const std::uint8_t>) {
    return Bytes().le(kLeSupportedStates, 8).take();
}

}  // namespace WN::Bt
