// WSA-Next © 2026 Marek Wesołowski - wesmar
#include "bluetooth/LeLink.h"
#include "bluetooth/Bytes.h"
#include "bluetooth/Crypto.h"
#include "bluetooth/L2cap.h"
#include "bluetooth/Log.h"
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <thread>

namespace WN::Bt {
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using winrt::Windows::Devices::Enumeration::DevicePairingResultStatus;
using winrt::Windows::Storage::Streams::Buffer;
using winrt::Windows::Storage::Streams::IBuffer;

namespace {

// ATT opcodes (Core Vol 3 Part F 3.4).
enum : std::uint8_t {
    kErrorRsp = 0x01, kMtuReq = 0x02, kMtuRsp = 0x03, kFindInfoReq = 0x04, kFindInfoRsp = 0x05, kFindByTypeReq = 0x06,
    kFindByTypeRsp = 0x07, kReadByTypeReq = 0x08, kReadByTypeRsp = 0x09, kReadReq = 0x0A, kReadRsp = 0x0B, kReadBlobReq = 0x0C,
    kReadBlobRsp = 0x0D, kReadByGroupReq = 0x10, kReadByGroupRsp = 0x11, kWriteReq = 0x12, kWriteRsp = 0x13,
    kPrepareWriteReq = 0x16, kPrepareWriteRsp = 0x17, kExecuteWriteReq = 0x18, kExecuteWriteRsp = 0x19, kNotification = 0x1B,
    kIndication = 0x1D, kConfirmation = 0x1E, kWriteCmd = 0x52, kSignedWriteCmd = 0xD2,
};
// ATT error codes.
enum : std::uint8_t {
    kInvalidHandle = 0x01, kReadNotPermitted = 0x02, kWriteNotPermitted = 0x03, kInvalidPdu = 0x04, kRequestNotSupported = 0x06,
    kInvalidOffset = 0x07, kPrepareQueueFull = 0x09, kAttributeNotFound = 0x0A, kInvalidAttributeValueLength = 0x0D,
    kUnlikelyError = 0x0E,
};
constexpr std::size_t kMaxAttributeValue = 512;
// The link to the host is virtual: Android may use ATT's largest MTU; Windows negotiates the real
// one with the device and splits long values itself.
constexpr std::uint16_t kServerMtu = 517;
// Prepared writes the link keeps for one Execute Write (Android's longest value is 512 bytes);
// each entry also costs its bookkeeping, so a flood of empty ones fills the queue as well.
constexpr std::size_t kPreparedLimit = 64 * 1024;
constexpr std::size_t kPreparedEntryCost = 64;
// Indications the device sent while Android had not confirmed the previous one yet.
constexpr std::size_t kIndicationBacklog = 64;

// SMP codes (Core Vol 3 Part H 3.3).
enum : std::uint8_t {
    kSmpPairingRequest = 0x01, kSmpPairingResponse = 0x02, kSmpPairingConfirm = 0x03, kSmpPairingRandom = 0x04,
    kSmpPairingFailed = 0x05, kSmpEncryptionInformation = 0x06, kSmpMasterIdentification = 0x07,
};

// Queue item for "the host has encrypted the link" (not an L2CAP channel).
constexpr std::uint16_t kEncrypted = 0xFFFF;

// A UUID as ATT carries it (least significant byte first); 16-bit form for Bluetooth base UUIDs.
struct Uuid {
    std::array<std::uint8_t, 16> le{};

    static constexpr std::array<std::uint8_t, 16> kBase{0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0, 0, 0, 0};

    static Uuid From16(std::uint16_t value) {
        Uuid u;
        u.le = kBase;
        u.le[12] = static_cast<std::uint8_t>(value);
        u.le[13] = static_cast<std::uint8_t>(value >> 8);
        return u;
    }
    static Uuid FromGuid(const winrt::guid& g) {
        const std::array<std::uint8_t, 16> be{
            static_cast<std::uint8_t>(g.Data1 >> 24), static_cast<std::uint8_t>(g.Data1 >> 16), static_cast<std::uint8_t>(g.Data1 >> 8),
            static_cast<std::uint8_t>(g.Data1), static_cast<std::uint8_t>(g.Data2 >> 8), static_cast<std::uint8_t>(g.Data2),
            static_cast<std::uint8_t>(g.Data3 >> 8), static_cast<std::uint8_t>(g.Data3), g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
            g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]};
        Uuid u;
        std::reverse_copy(be.begin(), be.end(), u.le.begin());
        return u;
    }
    static std::optional<Uuid> FromBytes(std::span<const std::uint8_t> bytes) {
        if (bytes.size() == 2) return From16(static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8)));
        if (bytes.size() != 16) return std::nullopt;
        Uuid u;
        std::copy(bytes.begin(), bytes.end(), u.le.begin());
        return u;
    }
    [[nodiscard]] bool Is16() const {
        for (std::size_t i = 0; i < 16; ++i)
            if (i != 12 && i != 13 && le[i] != kBase[i]) return false;
        return true;
    }
    [[nodiscard]] std::size_t Size() const { return Is16() ? 2 : 16; }
    void AppendTo(Bytes& out) const {
        if (Is16()) out.u8(le[12]).u8(le[13]);
        else out.raw(le);
    }
    bool operator==(const Uuid&) const = default;
};

const Uuid kPrimaryService = Uuid::From16(0x2800), kInclude = Uuid::From16(0x2802), kCharacteristic = Uuid::From16(0x2803),
           kCccd = Uuid::From16(0x2902), kHidService = Uuid::From16(0x1812);

std::vector<std::uint8_t> ToBytes(const IBuffer& buffer) {
    if (!buffer) return {};
    return {buffer.data(), buffer.data() + buffer.Length()};
}

IBuffer ToBuffer(std::span<const std::uint8_t> bytes) {
    Buffer buffer(static_cast<std::uint32_t>(bytes.size()));
    if (!bytes.empty()) std::memcpy(buffer.data(), bytes.data(), bytes.size());
    buffer.Length(static_cast<std::uint32_t>(bytes.size()));
    return buffer;
}

std::uint8_t AttError(GattCommunicationStatus status, const winrt::Windows::Foundation::IReference<std::uint8_t>& protocolError, bool read) {
    if (status == GattCommunicationStatus::ProtocolError && protocolError) return protocolError.Value();
    if (status == GattCommunicationStatus::AccessDenied) return read ? kReadNotPermitted : kWriteNotPermitted;
    return kUnlikelyError;
}

}  // namespace

struct LeLink::Impl : std::enable_shared_from_this<LeLink::Impl> {
    struct Descriptor {
        std::uint16_t handle = 0;
        Uuid uuid;
        GattDescriptor object{nullptr};  // null: the CCCD Windows manages itself (written through the characteristic)
    };
    struct Characteristic {
        std::uint16_t declaration = 0, value = 0;
        std::uint8_t properties = 0;
        Uuid uuid;
        GattCharacteristic object{nullptr};
        std::vector<Descriptor> descriptors;
        winrt::event_token changed{};
        bool subscribed = false;
        bool indicate = false;  // the mode of the current subscription (the handler has its own copy)
    };
    struct Service {
        std::uint16_t start = 0, end = 0;
        Uuid uuid;
        GattDeviceService object{nullptr};
        std::vector<Characteristic> characteristics;
    };
    enum class Kind { Service, Declaration, Value, Descriptor };
    struct Attribute {
        std::uint16_t handle;
        Kind kind;
        Uuid type;
        std::size_t service, characteristic = 0, descriptor = 0;
    };

    LinkAddresses peers;
    std::uint64_t address = 0;  // the device
    SendFn sendFn;              // use Send(): frames of one link must not interleave
    ClosedFn closed;
    std::mutex sendLock;        // one L2CAP frame (several ACL fragments) at a time on this link
    // Held shared by every WinRT handler while it runs; Shutdown takes it exclusively, so once it
    // has, no handler is inside and none gets past its `closing` check any more.
    std::shared_mutex delivering;

    // Pairing towards Android.
    std::array<std::uint8_t, 7> preq{}, pres{};
    Block mconfirm{}, srand{};
    bool pairing = false, keysToSend = false;
    BluetoothLEDevice device{nullptr};
    GattSession session{nullptr};
    winrt::event_token sessionChanged{};
    std::vector<Service> services;
    std::vector<Attribute> attributes;  // sorted by handle
    std::atomic<std::uint16_t> mtu{23};  // set by the worker, read by value-changed handlers
    // Prepared writes per handle, handles in the order of their first Prepare Write.
    std::vector<std::pair<std::uint16_t, std::vector<std::pair<std::uint16_t, std::vector<std::uint8_t>>>>> prepared;
    std::size_t preparedBytes = 0;

    // ATT allows one indication at a time: the next waits for Android's confirmation.
    std::mutex indicationLock;
    bool indicationPending = false;
    std::deque<std::vector<std::uint8_t>> indications;

    std::mutex queueLock;
    std::condition_variable_any queueReady;
    std::deque<std::pair<std::uint16_t, std::vector<std::uint8_t>>> queue;
    std::atomic<bool> closing{false}, shut{false}, disconnected{false};
    std::jthread worker;

    // --- Database --------------------------------------------------------------------------------

    bool Load() {
        const auto result = device.GetGattServicesAsync(BluetoothCacheMode::Uncached).get();
        if (result.Status() != GattCommunicationStatus::Success) {
            Log("gatt {:012X}: services unavailable (status {})", address, static_cast<int>(result.Status()));
            return false;
        }
        std::vector<GattDeviceService> list;
        for (const auto& s : result.Services()) list.push_back(s);
        std::ranges::sort(list, {}, [](const GattDeviceService& s) { return s.AttributeHandle(); });
        std::size_t characteristicCount = 0;
        for (const auto& s : list) {
            Service service{.start = s.AttributeHandle(), .end = s.AttributeHandle(), .uuid = Uuid::FromGuid(s.Uuid()), .object = s};
            if (service.uuid == kHidService) continue;  // Windows keeps HID for itself
            const auto chars = s.GetCharacteristicsAsync(BluetoothCacheMode::Uncached).get();
            if (chars.Status() == GattCommunicationStatus::Success) {
                for (const auto& c : chars.Characteristics()) {
                    Characteristic ch;
                    ch.declaration = c.AttributeHandle();
                    ch.value = static_cast<std::uint16_t>(ch.declaration + 1);
                    ch.properties = static_cast<std::uint8_t>(static_cast<std::uint32_t>(c.CharacteristicProperties()) & 0xFF);
                    ch.uuid = Uuid::FromGuid(c.Uuid());
                    ch.object = c;
                    const auto descriptors = c.GetDescriptorsAsync(BluetoothCacheMode::Uncached).get();
                    if (descriptors.Status() == GattCommunicationStatus::Success)
                        for (const auto& d : descriptors.Descriptors())
                            ch.descriptors.push_back({d.AttributeHandle(), Uuid::FromGuid(d.Uuid()), d});
                    std::ranges::sort(ch.descriptors, {}, &Descriptor::handle);
                    service.characteristics.push_back(std::move(ch));
                }
            }
            std::ranges::sort(service.characteristics, {}, &Characteristic::declaration);
            // Windows may hide the CCCD of a notifying characteristic: it follows the value handle.
            for (std::size_t i = 0; i < service.characteristics.size(); ++i) {
                auto& ch = service.characteristics[i];
                const bool notifies = (ch.properties & 0x30) != 0;
                const bool hasCccd = std::ranges::any_of(ch.descriptors, [](const Descriptor& d) { return d.uuid == kCccd; });
                const std::uint16_t next = i + 1 < service.characteristics.size() ? service.characteristics[i + 1].declaration : 0xFFFF;
                const std::uint16_t candidate = static_cast<std::uint16_t>(ch.value + 1);
                const bool unused = candidate < next && std::ranges::none_of(ch.descriptors, [&](const Descriptor& d) { return d.handle == candidate; });
                if (notifies && !hasCccd && unused) {
                    ch.descriptors.insert(ch.descriptors.begin(), Descriptor{candidate, kCccd, nullptr});
                }
                service.end = std::max({service.end, ch.value, ch.descriptors.empty() ? ch.value : ch.descriptors.back().handle});
            }
            characteristicCount += service.characteristics.size();
            services.push_back(std::move(service));
        }
        for (std::size_t s = 0; s < services.size(); ++s) {
            attributes.push_back({services[s].start, Kind::Service, kPrimaryService, s});
            for (std::size_t c = 0; c < services[s].characteristics.size(); ++c) {
                const auto& ch = services[s].characteristics[c];
                attributes.push_back({ch.declaration, Kind::Declaration, kCharacteristic, s, c});
                attributes.push_back({ch.value, Kind::Value, ch.uuid, s, c});
                for (std::size_t d = 0; d < ch.descriptors.size(); ++d)
                    attributes.push_back({ch.descriptors[d].handle, Kind::Descriptor, ch.descriptors[d].uuid, s, c, d});
            }
        }
        std::ranges::sort(attributes, {}, &Attribute::handle);
        Log("gatt {:012X}: {} services, {} characteristics, {} attributes", address, services.size(), characteristicCount, attributes.size());
        return true;
    }

    const Attribute* Find(std::uint16_t handle) const {
        const auto it = std::ranges::lower_bound(attributes, handle, {}, &Attribute::handle);
        return it != attributes.end() && it->handle == handle ? &*it : nullptr;
    }

    // --- Replies ---------------------------------------------------------------------------------

    void Send(std::uint16_t cid, std::vector<std::uint8_t> payload) {
        std::lock_guard guard(sendLock);
        sendFn(cid, std::move(payload));
    }

    void Reply(Bytes& pdu) { Send(kCidAtt, pdu.take()); }
    void Reply(Bytes&& pdu) { Reply(pdu); }

    // A value from the device for Android: notifications go at once, indications one at a time.
    void Deliver(std::uint8_t opcode, std::vector<std::uint8_t> pdu) {
        if (opcode != kIndication) return Send(kCidAtt, std::move(pdu));
        std::lock_guard guard(indicationLock);
        if (indicationPending) {
            if (indications.size() >= kIndicationBacklog) indications.pop_front();  // Android stopped confirming
            indications.push_back(std::move(pdu));
            return;
        }
        indicationPending = true;
        Send(kCidAtt, std::move(pdu));
    }

    void OnConfirmation() {
        std::lock_guard guard(indicationLock);
        if (indications.empty()) {
            indicationPending = false;
            return;
        }
        Send(kCidAtt, std::move(indications.front()));
        indications.pop_front();
    }

    void Error(std::uint8_t request, std::uint16_t handle, std::uint8_t code) {
        Reply(Bytes().u8(kErrorRsp).u8(request).u16(handle).u8(code));
    }

    // --- Worker ----------------------------------------------------------------------------------

    void Run(std::stop_token stop) {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        for (;;) {
            std::pair<std::uint16_t, std::vector<std::uint8_t>> item;
            {
                std::unique_lock lock(queueLock);
                if (!queueReady.wait(lock, stop, [this] { return !queue.empty(); })) return;
                item = std::move(queue.front());
                queue.pop_front();
            }
            try {
                if (item.first == kCidAtt) Att(item.second);
                else if (item.first == kCidLeSignaling) Signaling(item.second);
                else if (item.first == kCidSmp) Smp(item.second);
                else if (item.first == kEncrypted) DistributeKeys();
            } catch (const winrt::hresult_error& e) {
                Log("gatt {:012X}: 0x{:08X} {}", address, static_cast<unsigned>(e.code()), winrt::to_string(e.message()));
                if (item.first == kCidAtt && !item.second.empty() && (item.second[0] & 0x40) == 0) Error(item.second[0], 0, kUnlikelyError);
            } catch (const std::out_of_range&) {
                if (item.first == kCidAtt && !item.second.empty() && (item.second[0] & 0x40) == 0) Error(item.second[0], 0, kInvalidPdu);
            } catch (const std::exception& e) {
                // Anything else (out of memory, the RNG): this request fails, the host keeps running.
                Log("gatt {:012X}: {}", address, e.what());
                if (item.first == kCidAtt && !item.second.empty() && (item.second[0] & 0x40) == 0) Error(item.second[0], 0, kUnlikelyError);
            }
        }
    }

    // --- ATT -------------------------------------------------------------------------------------

    void Att(const std::vector<std::uint8_t>& pdu) {
        if (pdu.empty()) return;
        const std::uint8_t op = pdu[0];
        Reader r(std::span(pdu).subspan(1));
        switch (op) {
        case kMtuReq: {
            const std::uint16_t client = r.u16();
            mtu = std::clamp<std::uint16_t>(std::min(client, kServerMtu), std::uint16_t{23}, kServerMtu);
            Reply(Bytes().u8(kMtuRsp).u16(kServerMtu));
            break;
        }
        case kFindInfoReq: {
            const std::uint16_t start = r.u16(), end = r.u16();
            FindInformation(start, end);
            break;
        }
        case kFindByTypeReq: {
            const std::uint16_t start = r.u16(), end = r.u16(), type = r.u16();
            FindByTypeValue(start, end, type, r.bytes(r.left()));
            break;
        }
        case kReadByTypeReq: {
            const std::uint16_t start = r.u16(), end = r.u16();
            const auto type = Uuid::FromBytes(r.bytes(r.left()));
            if (!type) return Error(op, start, kInvalidPdu);
            ReadByType(start, end, *type);
            break;
        }
        case kReadReq: Read(op, r.u16(), 0); break;
        case kReadBlobReq: {
            const std::uint16_t handle = r.u16(), offset = r.u16();
            Read(op, handle, offset);
            break;
        }
        case kReadByGroupReq: {
            const std::uint16_t start = r.u16(), end = r.u16();
            const auto type = Uuid::FromBytes(r.bytes(r.left()));
            if (!type) return Error(op, start, kInvalidPdu);
            ReadByGroupType(start, end, *type);
            break;
        }
        case kWriteReq:
        case kWriteCmd: {
            const std::uint16_t handle = r.u16();
            const auto value = r.bytes(r.left());
            Write(handle, {value.begin(), value.end()}, op == kWriteReq ? kWriteReq : 0);
            break;
        }
        case kPrepareWriteReq: {
            const std::uint16_t handle = r.u16(), offset = r.u16();
            const auto value = r.bytes(r.left());
            if (!Find(handle)) return Error(op, handle, kInvalidHandle);
            // An attribute value has at most 512 octets (Core Vol 3 Part F 3.2.9); every entry
            // counts against the queue, empty ones too.
            if (offset + value.size() > kMaxAttributeValue) return Error(op, handle, kInvalidAttributeValueLength);
            if (preparedBytes + value.size() + kPreparedEntryCost > kPreparedLimit) return Error(op, handle, kPrepareQueueFull);
            auto it = std::ranges::find(prepared, handle, &decltype(prepared)::value_type::first);
            if (it == prepared.end()) it = prepared.insert(prepared.end(), {handle, {}});
            it->second.emplace_back(offset, std::vector<std::uint8_t>(value.begin(), value.end()));
            preparedBytes += value.size() + kPreparedEntryCost;
            Reply(Bytes().u8(kPrepareWriteRsp).u16(handle).u16(offset).raw(value));
            break;
        }
        case kExecuteWriteReq: {
            const std::uint8_t flags = r.u8();
            auto queued = std::move(prepared);
            prepared.clear();
            preparedBytes = 0;
            if (flags == 0x01) {
                // In the order the values were prepared (Core Vol 3 Part F 3.4.6.3).
                for (auto& [handle, parts] : queued) {
                    std::vector<std::uint8_t> value;
                    for (const auto& [offset, data] : parts) {
                        if (value.size() < offset + data.size()) value.resize(offset + data.size());
                        std::ranges::copy(data, value.begin() + offset);
                    }
                    if (!Write(handle, std::move(value), kExecuteWriteReq)) return;
                }
            }
            Reply(Bytes().u8(kExecuteWriteRsp));
            break;
        }
        case kConfirmation: OnConfirmation(); break;
        case kSignedWriteCmd: break;
        default:
            if ((op & 0x40) == 0) Error(op, 0, kRequestNotSupported);  // commands (bit 6) get no answer
            break;
        }
    }

    void FindInformation(std::uint16_t start, std::uint16_t end) {
        if (start == 0 || start > end) return Error(kFindInfoReq, start, kInvalidHandle);
        Bytes out;
        out.u8(kFindInfoRsp).u8(0);
        std::uint8_t format = 0;
        for (const Attribute& a : attributes) {
            if (a.handle < start || a.handle > end) continue;
            const std::uint8_t f = a.type.Is16() ? 1 : 2;
            if (format == 0) format = f;
            if (f != format || out.size() + 2 + a.type.Size() > mtu) break;
            out.u16(a.handle);
            a.type.AppendTo(out);
        }
        if (format == 0) return Error(kFindInfoReq, start, kAttributeNotFound);
        auto pdu = out.take();
        pdu[1] = format;
        Send(kCidAtt, std::move(pdu));
    }

    void FindByTypeValue(std::uint16_t start, std::uint16_t end, std::uint16_t type, std::span<const std::uint8_t> value) {
        const auto wanted = Uuid::FromBytes(value);
        Bytes out;
        out.u8(kFindByTypeRsp);
        if (type == 0x2800 && wanted) {
            for (const Service& s : services) {
                if (s.start < start || s.start > end || !(s.uuid == *wanted)) continue;
                if (out.size() + 4 > mtu) break;
                out.u16(s.start).u16(s.end);
            }
        }
        if (out.size() == 1) return Error(kFindByTypeReq, start, kAttributeNotFound);
        Reply(std::move(out));
    }

    void ReadByGroupType(std::uint16_t start, std::uint16_t end, const Uuid& type) {
        if (start == 0 || start > end) return Error(kReadByGroupReq, start, kInvalidHandle);
        Bytes out;
        out.u8(kReadByGroupRsp).u8(0);
        std::size_t entry = 0;
        if (type == kPrimaryService) {
            for (const Service& s : services) {
                if (s.start < start || s.start > end) continue;
                const std::size_t size = 4 + s.uuid.Size();
                if (entry == 0) entry = size;
                if (size != entry || out.size() + size > mtu) break;
                out.u16(s.start).u16(s.end);
                s.uuid.AppendTo(out);
            }
        }
        if (entry == 0) return Error(kReadByGroupReq, start, kAttributeNotFound);
        auto pdu = out.take();
        pdu[1] = static_cast<std::uint8_t>(entry);
        Send(kCidAtt, std::move(pdu));
    }

    void ReadByType(std::uint16_t start, std::uint16_t end, const Uuid& type) {
        if (start == 0 || start > end) return Error(kReadByTypeReq, start, kInvalidHandle);
        Bytes out;
        out.u8(kReadByTypeRsp).u8(0);
        std::size_t entry = 0;
        if (type == kCharacteristic) {
            for (const Attribute& a : attributes) {
                if (a.kind != Kind::Declaration || a.handle < start || a.handle > end) continue;
                const Characteristic& ch = services[a.service].characteristics[a.characteristic];
                const std::size_t size = 2 + 3 + ch.uuid.Size();
                if (entry == 0) entry = size;
                if (size != entry || out.size() + size > mtu) break;
                out.u16(ch.declaration).u8(ch.properties).u16(ch.value);
                ch.uuid.AppendTo(out);
            }
        } else if (!(type == kInclude)) {
            // Read by UUID: every value or descriptor of that type in the range, as long as they
            // have the length of the first and fit (Core Vol 3 Part F 3.4.4.2). A failing read
            // after the first ends the list; on the first it is the answer.
            const std::size_t longest = std::min<std::size_t>(mtu - 4u, 253);
            for (const Attribute& a : attributes) {
                if (a.handle < start || a.handle > end || !(a.type == type) || (a.kind != Kind::Value && a.kind != Kind::Descriptor)) continue;
                std::uint8_t error = 0;
                auto value = ReadValue(a, error);
                if (error) {
                    if (entry == 0) return Error(kReadByTypeReq, a.handle, error);
                    break;
                }
                value.resize(std::min(value.size(), longest));
                const std::size_t size = 2 + value.size();
                if (entry == 0) entry = size;
                if (size != entry || out.size() + size > mtu) break;
                out.u16(a.handle).raw(value);
                if (value.size() == longest) break;  // a truncated value is the only one (the client reads the rest)
            }
        }
        if (entry == 0) return Error(kReadByTypeReq, start, kAttributeNotFound);
        auto pdu = out.take();
        pdu[1] = static_cast<std::uint8_t>(entry);
        Send(kCidAtt, std::move(pdu));
    }

    // The value of an attribute: declarations from the snapshot, values and descriptors from the device.
    std::vector<std::uint8_t> ReadValue(const Attribute& a, std::uint8_t& error) {
        const Service& s = services[a.service];
        switch (a.kind) {
        case Kind::Service: {
            Bytes b;
            s.uuid.AppendTo(b);
            return b.take();
        }
        case Kind::Declaration: {
            const Characteristic& ch = s.characteristics[a.characteristic];
            Bytes b;
            b.u8(ch.properties).u16(ch.value);
            ch.uuid.AppendTo(b);
            return b.take();
        }
        case Kind::Value: {
            const auto result = s.characteristics[a.characteristic].object.ReadValueAsync(BluetoothCacheMode::Uncached).get();
            if (result.Status() != GattCommunicationStatus::Success) {
                error = AttError(result.Status(), result.ProtocolError(), true);
                return {};
            }
            return ToBytes(result.Value());
        }
        case Kind::Descriptor: {
            const Characteristic& ch = s.characteristics[a.characteristic];
            const Descriptor& d = ch.descriptors[a.descriptor];
            if (d.uuid == kCccd) {
                const auto result = ch.object.ReadClientCharacteristicConfigurationDescriptorAsync().get();
                if (result.Status() != GattCommunicationStatus::Success) {
                    error = AttError(result.Status(), result.ProtocolError(), true);
                    return {};
                }
                return Bytes().u16(static_cast<std::uint16_t>(result.ClientCharacteristicConfigurationDescriptor())).take();
            }
            const auto result = d.object.ReadValueAsync(BluetoothCacheMode::Uncached).get();
            if (result.Status() != GattCommunicationStatus::Success) {
                error = AttError(result.Status(), result.ProtocolError(), true);
                return {};
            }
            return ToBytes(result.Value());
        }
        }
        return {};
    }

    void Read(std::uint8_t op, std::uint16_t handle, std::uint16_t offset) {
        const Attribute* a = Find(handle);
        if (!a) return Error(op, handle, kInvalidHandle);
        std::uint8_t error = 0;
        auto value = ReadValue(*a, error);
        if (error) return Error(op, handle, error);
        if (op == kReadBlobReq) {
            if (offset > value.size()) return Error(op, handle, kInvalidOffset);
            value.erase(value.begin(), value.begin() + offset);
        }
        value.resize(std::min<std::size_t>(value.size(), mtu - 1u));
        Reply(Bytes().u8(op == kReadBlobReq ? kReadBlobRsp : kReadRsp).raw(value));
    }

    // `request`: the ATT request to answer (Write or Execute Write), 0 for a Write Command.
    // Returns false when an error response was sent.
    bool Write(std::uint16_t handle, std::vector<std::uint8_t> value, std::uint8_t request) {
        const Attribute* a = Find(handle);
        auto fail = [&](std::uint8_t code) {
            if (request) Error(request, handle, code);
            return false;
        };
        if (!a) return fail(kInvalidHandle);
        // Service and declaration attributes are read-only (and a service may have no characteristic).
        if (a->kind != Kind::Value && a->kind != Kind::Descriptor) return fail(kWriteNotPermitted);
        Characteristic& ch = services[a->service].characteristics[a->characteristic];
        if (a->kind == Kind::Descriptor && ch.descriptors[a->descriptor].uuid == kCccd) {
            const std::uint16_t bits = value.size() >= 2 ? static_cast<std::uint16_t>(value[0] | (value[1] << 8)) : 0;
            const auto config = (bits & 0x2) ? GattClientCharacteristicConfigurationDescriptorValue::Indicate
                                : (bits & 0x1) ? GattClientCharacteristicConfigurationDescriptorValue::Notify
                                               : GattClientCharacteristicConfigurationDescriptorValue::None;
            const auto result = ch.object.WriteClientCharacteristicConfigurationDescriptorWithResultAsync(config).get();
            if (result.Status() != GattCommunicationStatus::Success) return fail(AttError(result.Status(), result.ProtocolError(), false));
            Subscribe(a->service, a->characteristic, bits != 0, (bits & 0x2) != 0);
        } else if (a->kind == Kind::Value) {
            const auto option = request ? GattWriteOption::WriteWithResponse : GattWriteOption::WriteWithoutResponse;
            const auto result = ch.object.WriteValueWithResultAsync(ToBuffer(value), option).get();
            if (result.Status() != GattCommunicationStatus::Success) return fail(AttError(result.Status(), result.ProtocolError(), false));
        } else if (a->kind == Kind::Descriptor && ch.descriptors[a->descriptor].object) {
            const auto result = ch.descriptors[a->descriptor].object.WriteValueWithResultAsync(ToBuffer(value)).get();
            if (result.Status() != GattCommunicationStatus::Success) return fail(AttError(result.Status(), result.ProtocolError(), false));
        } else {
            return fail(kWriteNotPermitted);
        }
        if (request == kWriteReq) Reply(Bytes().u8(kWriteRsp));
        return true;
    }

    void Subscribe(std::size_t s, std::size_t c, bool on, bool indicate) {
        Characteristic& ch = services[s].characteristics[c];
        if (ch.subscribed && (!on || ch.indicate != indicate)) {  // off, or the other mode
            ch.object.ValueChanged(ch.changed);
            ch.subscribed = false;
        }
        if (on && !ch.subscribed) {
            // The handler gets what it needs by value: it runs on WinRT threads, concurrently with the worker.
            const std::uint8_t opcode = indicate ? kIndication : kNotification;
            const std::uint16_t handle = ch.value;
            ch.changed = ch.object.ValueChanged([weak = weak_from_this(), opcode, handle](const GattCharacteristic&, const GattValueChangedEventArgs& args) {
                const auto self = weak.lock();
                if (!self) return;
                const std::shared_lock running(self->delivering);
                if (self->closing) return;
                auto value = ToBytes(args.CharacteristicValue());
                value.resize(std::min<std::size_t>(value.size(), self->mtu - 3u));
                self->Deliver(opcode, Bytes().u8(opcode).u16(handle).raw(value).take());
            });
            ch.indicate = indicate;
            ch.subscribed = true;
        }
    }

    // --- LE signaling ---------------------------------------------------------------------------

    void Signaling(const std::vector<std::uint8_t>& pdu) {
        if (pdu.size() < 4) return;
        const std::uint8_t code = pdu[0], id = pdu[1];
        if (code == 0x01 || code == 0x13 || code == 0x15 || code == 0x18) return;  // reject, responses
        Send(kCidLeSignaling, Bytes().u8(0x01).u8(id).u16(2).u16(0x0000).take());  // command not understood
    }

    // --- Pairing: real in Windows, LE legacy "Just Works" towards Android ------------------------

    bool PairWithWindows() {
        try {
            const auto windowsPairing = device.DeviceInformation().Pairing();
            if (windowsPairing.IsPaired() || !windowsPairing.CanPair()) return true;
            Log("smp {:012X}: pairing in Windows", address);
            const auto status = windowsPairing.PairAsync().get().Status();
            Log("smp {:012X}: Windows pairing result {}", address, static_cast<int>(status));
            return status == DevicePairingResultStatus::Paired || status == DevicePairingResultStatus::AlreadyPaired;
        } catch (const winrt::hresult_error& e) {
            Log("smp {:012X}: Windows pairing failed: 0x{:08X}", address, static_cast<unsigned>(e.code()));
            return false;
        }
    }

    void SmpFail(std::uint8_t reason) {
        pairing = false;
        keysToSend = false;
        Send(kCidSmp, Bytes().u8(kSmpPairingFailed).u8(reason).take());
    }

    void Smp(const std::vector<std::uint8_t>& pdu) {
        if (pdu.empty()) return;
        const Block zero{};  // TK of Just Works
        switch (pdu[0]) {
        case kSmpPairingRequest: {
            if (pdu.size() < 7) return SmpFail(0x0A);  // invalid parameters
            if (!PairWithWindows()) return SmpFail(0x08);  // unspecified reason
            std::copy_n(pdu.begin(), 7, preq.begin());
            // No input/output, no OOB, bonding as requested (no MITM, no Secure Connections), 16-octet
            // keys; the device's side distributes its encryption key, nothing is asked of Android.
            pres = {kSmpPairingResponse, 0x03, 0x00, static_cast<std::uint8_t>(pdu[3] & 0x01), 0x10, 0x00, static_cast<std::uint8_t>(pdu[6] & 0x01)};
            keysToSend = (pres[6] & 0x01) != 0;
            pairing = true;
            Send(kCidSmp, std::vector<std::uint8_t>(pres.begin(), pres.end()));
            break;
        }
        case kSmpPairingConfirm: {
            if (!pairing || pdu.size() < 17) return SmpFail(0x08);
            std::copy_n(pdu.begin() + 1, 16, mconfirm.begin());
            RandomBytes(srand);
            const Block sconfirm = C1(zero, srand, preq, pres, static_cast<std::uint8_t>(peers.ownRandom), peers.own, static_cast<std::uint8_t>(peers.peerRandom), peers.peer);
            Send(kCidSmp, Bytes().u8(kSmpPairingConfirm).raw(sconfirm).take());
            break;
        }
        case kSmpPairingRandom: {
            if (!pairing || pdu.size() < 17) return SmpFail(0x08);
            Block mrand;
            std::copy_n(pdu.begin() + 1, 16, mrand.begin());
            if (C1(zero, mrand, preq, pres, static_cast<std::uint8_t>(peers.ownRandom), peers.own, static_cast<std::uint8_t>(peers.peerRandom), peers.peer) != mconfirm)
                return SmpFail(0x04);  // confirm value failed
            Send(kCidSmp, Bytes().u8(kSmpPairingRandom).raw(srand).take());
            // Android now encrypts with the STK s1(TK, Srand, Mrand); the keys follow once it has.
            break;
        }
        case kSmpPairingFailed:
            pairing = false;
            keysToSend = false;
            break;
        default: break;  // Android's own keys: nothing to keep
        }
    }

    // After LE Start Encryption: the long-term key that Android stores in its bond.
    void DistributeKeys() {
        if (!keysToSend) return;
        keysToSend = false;
        pairing = false;
        Block ltk{};
        RandomBytes(ltk);
        std::array<std::uint8_t, 10> masterId{};  // EDIV (2) and Rand (8)
        RandomBytes(masterId);
        Send(kCidSmp, Bytes().u8(kSmpEncryptionInformation).raw(ltk).take());
        Send(kCidSmp, Bytes().u8(kSmpMasterIdentification).raw(masterId).take());
        Log("smp {:012X}: bonded with Android", address);
    }

    // --- Lifetime --------------------------------------------------------------------------------

    void OnSessionClosed() {
        if (closing.exchange(true)) return;
        disconnected = true;
        Log("gatt {:012X}: the device disconnected", address);
        closed(0x08);  // connection timeout
    }

    void Shutdown() {
        if (shut.exchange(true)) return;
        closing = true;
        worker.request_stop();
        if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) worker.join();
        // Handlers already past their `closing` check finish first: after this nothing of the link
        // calls `sendFn` or `closed`, whose owner may be gone right after Close returns.
        { const std::unique_lock drained(delivering); }
        try {
            for (auto& s : services)
                for (auto& ch : s.characteristics)
                    if (ch.subscribed) {
                        ch.object.ValueChanged(ch.changed);
                        ch.subscribed = false;
                    }
            if (session) {
                session.SessionStatusChanged(sessionChanged);
                session.Close();
            }
            for (auto& s : services) s.object.Close();
            if (device) device.Close();
        } catch (const winrt::hresult_error&) {
        }
        session = nullptr;
        device = nullptr;
    }
};

LeLink::LeLink(std::shared_ptr<Impl> impl) : m_impl(std::move(impl)) {}

LeLink::~LeLink() { Close(); }

std::unique_ptr<LeLink> LeLink::Connect(const LinkAddresses& addresses, SendFn send, ClosedFn closed, std::uint8_t& status) {
    const std::uint64_t address = addresses.peer;
    const bool randomAddress = addresses.peerRandom;
    auto impl = std::make_shared<Impl>();
    impl->peers = addresses;
    impl->address = address;
    impl->sendFn = std::move(send);
    impl->closed = std::move(closed);
    status = 0x3E;  // connection failed to be established
    try {
        impl->device = BluetoothLEDevice::FromBluetoothAddressAsync(address, randomAddress ? BluetoothAddressType::Random : BluetoothAddressType::Public).get();
        if (!impl->device) {
            Log("gatt {:012X}: device not found", address);
            return nullptr;
        }
        impl->session = GattSession::FromDeviceIdAsync(impl->device.BluetoothDeviceId()).get();
        impl->session.MaintainConnection(true);
        if (!impl->Load()) {
            impl->Shutdown();
            return nullptr;
        }
        impl->sessionChanged = impl->session.SessionStatusChanged([weak = std::weak_ptr<Impl>(impl)](const GattSession&, const GattSessionStatusChangedEventArgs& args) {
            if (args.Status() != GattSessionStatus::Closed) return;
            const auto self = weak.lock();
            if (!self) return;
            const std::shared_lock running(self->delivering);
            if (!self->shut) self->OnSessionClosed();
        });
        // The device may have gone during the database snapshot, before the handler existed.
        if (impl->session.SessionStatus() == GattSessionStatus::Closed && impl->device.ConnectionStatus() == BluetoothConnectionStatus::Disconnected)
            impl->OnSessionClosed();
    } catch (const winrt::hresult_error& e) {
        Log("gatt {:012X}: cannot connect: 0x{:08X} {}", address, static_cast<unsigned>(e.code()), winrt::to_string(e.message()));
        impl->Shutdown();
        return nullptr;
    }
    impl->worker = std::jthread([raw = impl.get()](std::stop_token stop) { raw->Run(stop); });
    status = 0x00;
    return std::unique_ptr<LeLink>(new LeLink(std::move(impl)));
}

void LeLink::OnFrame(std::uint16_t cid, std::vector<std::uint8_t> payload) {
    {
        std::lock_guard lock(m_impl->queueLock);
        m_impl->queue.emplace_back(cid, std::move(payload));
    }
    m_impl->queueReady.notify_one();
}

void LeLink::OnEncrypted() { OnFrame(kEncrypted, {}); }

void LeLink::Close() {
    if (m_impl) m_impl->Shutdown();
}

bool LeLink::Closed() const { return m_impl && m_impl->disconnected; }

}  // namespace WN::Bt
