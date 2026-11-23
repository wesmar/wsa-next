// WSA-Next © 2026 Marek Wesołowski - wesmar
// The mobile broadband COM API (mbnapi.h) is marked deprecated in favour of WinRT; it stays the
// plain Win32 way to the modem, so the deprecation warning is off for this file.
#pragma warning(disable : 4995)
#include <Windows.h>
#include <oleauto.h>
#include <ocidl.h>
#include <mbnapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Sms.h>
#include <winrt/Windows.Storage.Streams.h>
#include "telephony/Modem.h"
#include "bluetooth/Log.h"
#include "support/Common.h"
#include "support/Handle.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <utility>
#include <vector>

#pragma comment(lib, "mbnapi_uuid.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "windowsapp.lib")

namespace WN::Tel {
using Microsoft::WRL::ComPtr;

namespace {

constexpr DWORD kSendTimeoutMs = 120'000;  // a guard: the network answers in seconds

std::string Narrow(BSTR text) { return text ? ToUtf8(std::wstring_view(text, SysStringLen(text))) : std::string(); }

// 27.007 <AcT> from the modem's current data class.
int AccessTechnology(ULONG dataClass) {
    if (dataClass & MBN_DATA_CLASS_5G_SA) return 11;
    if (dataClass & (MBN_DATA_CLASS_5G_NSA | MBN_DATA_CLASS_LTE)) return 7;
    if ((dataClass & MBN_DATA_CLASS_HSDPA) && (dataClass & MBN_DATA_CLASS_HSUPA)) return 6;
    if (dataClass & MBN_DATA_CLASS_HSUPA) return 5;
    if (dataClass & MBN_DATA_CLASS_HSDPA) return 4;
    if (dataClass & MBN_DATA_CLASS_UMTS) return 2;
    if (dataClass & MBN_DATA_CLASS_EDGE) return 3;
    if (dataClass & MBN_DATA_CLASS_GPRS) return 0;
    return 7;
}

// 27.007 <stat> from the registration state.
int Registration(MBN_REGISTER_STATE state) {
    switch (state) {
    case MBN_REGISTER_STATE_HOME: return 1;
    case MBN_REGISTER_STATE_SEARCHING: return 2;
    case MBN_REGISTER_STATE_DENIED: return 3;
    case MBN_REGISTER_STATE_ROAMING:
    case MBN_REGISTER_STATE_PARTNER: return 5;
    default: return 0;
    }
}

// Calls `each` for every element of a SAFEARRAY of interface pointers.
template <class Each>
void ForEach(SAFEARRAY* array, Each each) {
    LONG low = 0, high = -1;
    if (!array || FAILED(SafeArrayGetLBound(array, 1, &low)) || FAILED(SafeArrayGetUBound(array, 1, &high))) return;
    for (LONG i = low; i <= high; ++i) {
        IUnknown* item = nullptr;
        if (SUCCEEDED(SafeArrayGetElement(array, &i, &item)) && item) {
            each(item);
            item->Release();
        }
    }
}

// What the notification sink and the Modem share; outlives either (callbacks may still run while
// the modem closes).
struct Shared {
    std::mutex lock;
    Modem::Events events;
    struct Pending {
        KernelHandle done{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        HRESULT status = E_PENDING;
    };
    std::map<ULONG, std::shared_ptr<Pending>> sends;  // SMS send requests by request ID
    struct Read {
        KernelHandle done{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        HRESULT status = E_PENDING;
        std::vector<StoredMessage> messages;
    };
    std::map<ULONG, std::shared_ptr<Read>> reads;  // Modem::ReadStore requests by request ID
    // Store indexes given to Android and not acknowledged yet: Windows reports one new message
    // with more than one status change (seen twice within 6 ms), and each one reads it again.
    std::set<ULONG> delivered;
    // MMS notices from Windows' SMS router (it takes them out of the modem's store), as SMS-DELIVER
    // PDUs, until Android acknowledges them. Indexes from kRoutedBase on: never the store's.
    struct Routed {
        unsigned index = 0;
        std::string pdu;
        bool sent = false;  // given to the current session
    };
    std::vector<Routed> routed;
    unsigned nextRouted = 0;
    std::uint8_t reference = 0;  // concatenation reference of the next notice
};

constexpr unsigned kRoutedBase = 0x40000000;
constexpr wchar_t kRegistrationId[] = L"WSA-Next";

// --- WAP push for Android (WAP-230 WSP push in SMS, 23.040 user data header) ---------------------

void HexByte(std::string& out, unsigned value) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    out += kDigits[(value >> 4) & 0xF];
    out += kDigits[value & 0xF];
}

// The originating address: digits (international with "+"), or up to 11 letters packed in the
// GSM 7-bit alphabet (letters, digits and space are the same as in ASCII).
std::string OriginatingAddress(std::string_view from) {
    std::string out;
    const bool international = from.starts_with('+');
    const std::string_view digits = international ? from.substr(1) : from;
    if (!digits.empty() && digits.size() <= 20 && digits.find_first_not_of("0123456789") == std::string_view::npos) {
        HexByte(out, static_cast<unsigned>(digits.size()));
        HexByte(out, international ? 0x91 : 0x81);
        for (std::size_t i = 0; i < digits.size(); i += 2) {
            const unsigned low = static_cast<unsigned>(digits[i] - '0');
            const unsigned high = i + 1 < digits.size() ? static_cast<unsigned>(digits[i + 1] - '0') : 0xF;
            HexByte(out, high << 4 | low);
        }
        return out;
    }
    std::string text;
    for (const char c : from.substr(0, 11)) text += (std::isalnum(static_cast<unsigned char>(c)) || c == ' ') ? c : '?';
    if (text.empty()) text = "MMS";
    std::vector<unsigned> packed;
    unsigned bits = 0, carry = 0;
    for (const char c : text) {
        carry |= static_cast<unsigned>(c & 0x7F) << bits;
        bits += 7;
        while (bits >= 8) {
            packed.push_back(carry & 0xFF);
            carry >>= 8;
            bits -= 8;
        }
    }
    if (bits > 0) packed.push_back(carry & 0xFF);
    HexByte(out, static_cast<unsigned>((text.size() * 7 + 3) / 4));  // semi-octets used
    HexByte(out, 0xD0);                                               // alphanumeric
    for (const unsigned o : packed) HexByte(out, o);
    return out;
}

// The service centre time stamp (UTC, zone 0) in swapped semi-octets.
std::string TimeStamp(const SYSTEMTIME& t) {
    std::string out;
    for (const unsigned v : {static_cast<unsigned>(t.wYear % 100), unsigned{t.wMonth}, unsigned{t.wDay}, unsigned{t.wHour}, unsigned{t.wMinute},
                             unsigned{t.wSecond}, 0u})
        HexByte(out, (v % 10) << 4 | v / 10);
    return out;
}

// SMS-DELIVER PDUs (hex, empty service centre part) carrying a WAP push to the MMS client's port
// 2948, split into concatenated parts when it does not fit one message.
std::vector<std::string> WapPushPdus(std::string_view from, const SYSTEMTIME& time, std::string_view contentType,
                                     std::span<const std::uint8_t> body, std::uint8_t reference) {
    std::vector<std::uint8_t> headers;
    if (contentType == "application/vnd.wap.mms-message") {
        headers = {0xBE, 0xAF, 0x84};  // well-known content type 0x3E; X-Wap-Application-Id: mms.ua
    } else {
        headers.assign(contentType.begin(), contentType.end());
        headers.push_back(0);
    }
    std::vector<std::uint8_t> push = {0x00, 0x06, static_cast<std::uint8_t>(headers.size())};  // TID, push, header length
    push.insert(push.end(), headers.begin(), headers.end());
    push.insert(push.end(), body.begin(), body.end());

    constexpr std::size_t kUserData = 140;
    constexpr std::size_t kSingle = kUserData - 1 - 6;    // header length + ports
    constexpr std::size_t kPart = kUserData - 1 - 6 - 5;  // + concatenation
    const std::size_t parts = push.size() <= kSingle ? 1 : (push.size() + kPart - 1) / kPart;
    if (parts > 255) return {};
    const std::string address = OriginatingAddress(from), stamp = TimeStamp(time);
    std::vector<std::string> pdus;
    for (std::size_t part = 0; part < parts; ++part) {
        const std::size_t size = parts == 1 ? kSingle : kPart;
        const std::size_t begin = part * size, end = std::min(push.size(), begin + size);
        std::vector<std::uint8_t> header = {0x05, 0x04, 0x0B, 0x84, 0x23, 0xF0};  // ports 2948 <- 9200
        if (parts > 1) header.insert(header.end(), {0x00, 0x03, reference, static_cast<std::uint8_t>(parts), static_cast<std::uint8_t>(part + 1)});
        std::string pdu = "00";  // no service centre
        HexByte(pdu, 0x44);      // SMS-DELIVER, user data header, no more messages
        pdu += address;
        pdu += "0004";  // protocol identifier, 8-bit data
        pdu += stamp;
        HexByte(pdu, static_cast<unsigned>(1 + header.size() + (end - begin)));
        HexByte(pdu, static_cast<unsigned>(header.size()));
        for (const std::uint8_t o : header) HexByte(pdu, o);
        for (std::size_t i = begin; i < end; ++i) HexByte(pdu, push[i]);
        pdus.push_back(std::move(pdu));
    }
    return pdus;
}

StoredStatus Status(MBN_MSG_STATUS status) {
    switch (status) {
    case MBN_MSG_STATUS_NEW: return StoredStatus::Unread;
    case MBN_MSG_STATUS_OLD: return StoredStatus::Read;
    case MBN_MSG_STATUS_DRAFT: return StoredStatus::Draft;
    default: return StoredStatus::Sent;
    }
}

// Calls `each(index, status, pduHex)` for the PDU messages of a read result.
template <class Each>
void ForEachPdu(MBN_SMS_FORMAT format, SAFEARRAY* messages, Each each) {
    if (format != MBN_SMS_FORMAT_PDU) return;
    ForEach(messages, [&](IUnknown* item) {
        ComPtr<IMbnSmsReadMsgPdu> message;
        if (FAILED(item->QueryInterface(IID_PPV_ARGS(message.GetAddressOf())))) return;
        ULONG index = 0;
        MBN_MSG_STATUS status = MBN_MSG_STATUS_OLD;
        BSTR pdu = nullptr;
        if (SUCCEEDED(message->get_Index(&index)) && SUCCEEDED(message->get_PduData(&pdu))) {
            message->get_Status(&status);
            const std::string hex = Narrow(pdu);
            if (!hex.empty()) each(index, status, hex);
        }
        SysFreeString(pdu);
    });
}


class Sink : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMbnSmsEvents,
                                                 IMbnSignalEvents, IMbnRegistrationEvents> {
public:
    explicit Sink(std::shared_ptr<Shared> shared) : m_shared(std::move(shared)) {}

    // IMbnSmsEvents
    STDMETHODIMP OnSmsConfigurationChange(IMbnSms*) override { return S_OK; }
    STDMETHODIMP OnSetSmsConfigurationComplete(IMbnSms*, ULONG, HRESULT) override { return S_OK; }
    STDMETHODIMP OnSmsSendComplete(IMbnSms*, ULONG requestId, HRESULT status) override {
        std::lock_guard guard(m_shared->lock);
        if (const auto it = m_shared->sends.find(requestId); it != m_shared->sends.end()) {
            it->second->status = status;
            SetEvent(it->second->done.get());
            m_shared->sends.erase(it);
        }
        return S_OK;
    }
    STDMETHODIMP OnSmsReadComplete(IMbnSms*, MBN_SMS_FORMAT format, SAFEARRAY* messages, VARIANT_BOOL more, ULONG requestId,
                                   HRESULT status) override {
        std::shared_ptr<Shared::Read> read;
        {
            std::lock_guard guard(m_shared->lock);
            if (const auto it = m_shared->reads.find(requestId); it != m_shared->reads.end()) read = it->second;
        }
        if (!read) {
            if (SUCCEEDED(status)) Deliver(format, messages);
            return S_OK;
        }
        // A ReadStore request: collected, not given to Android.
        if (SUCCEEDED(status))
            ForEachPdu(format, messages, [&](ULONG index, MBN_MSG_STATUS state, const std::string& pdu) {
                read->messages.push_back({index, Status(state), pdu});
            });
        if (!more || FAILED(status)) {
            std::lock_guard guard(m_shared->lock);
            read->status = status;
            m_shared->reads.erase(requestId);
            SetEvent(read->done.get());
        }
        return S_OK;
    }
    STDMETHODIMP OnSmsNewClass0Message(IMbnSms*, MBN_SMS_FORMAT format, SAFEARRAY* messages) override {
        Deliver(format, messages);
        return S_OK;
    }
    STDMETHODIMP OnSmsDeleteComplete(IMbnSms*, ULONG, HRESULT status) override {
        // The message stays in the store and goes to Android again in its next session.
        if (FAILED(status)) Bt::Log("modem: a message Android has could not be deleted from the modem (0x{:08X})", static_cast<unsigned>(status));
        return S_OK;
    }
    STDMETHODIMP OnSmsStatusChange(IMbnSms* sms) override {
        MBN_SMS_STATUS_INFO info{};
        if (!sms || FAILED(sms->GetSmsStatus(&info))) return S_OK;
        if (info.flag & MBN_SMS_FLAG_NEW_MESSAGE) {
            MBN_SMS_FILTER filter{MBN_SMS_FLAG_INDEX, info.messageIndex};
            ULONG request = 0;
            sms->SmsRead(&filter, MBN_SMS_FORMAT_PDU, &request);  // answered by OnSmsReadComplete
        }
        if (info.flag & MBN_SMS_FLAG_MESSAGE_STORE_FULL) Bt::Log("modem: the message store is full");
        return S_OK;
    }

    // IMbnSignalEvents
    STDMETHODIMP OnSignalStateChange(IMbnSignal*) override { return Changed(); }

    // IMbnRegistrationEvents
    STDMETHODIMP OnRegisterModeAvailable(IMbnRegistration*) override { return S_OK; }
    STDMETHODIMP OnRegisterStateChange(IMbnRegistration*) override { return Changed(); }
    STDMETHODIMP OnPacketServiceStateChange(IMbnRegistration*) override { return Changed(); }
    STDMETHODIMP OnSetRegisterModeComplete(IMbnRegistration*, ULONG, HRESULT) override { return S_OK; }

private:
    void Deliver(MBN_SMS_FORMAT format, SAFEARRAY* messages) {
        ForEachPdu(format, messages, [&](ULONG index, MBN_MSG_STATUS status, const std::string& pdu) {
            if (status == MBN_MSG_STATUS_DRAFT || status == MBN_MSG_STATUS_SENT) return;  // Android gets received messages only
            std::function<void(const std::string&, unsigned)> sink;
            {
                std::lock_guard guard(m_shared->lock);
                sink = m_shared->events.message;
                if (!sink || !m_shared->delivered.insert(index).second) return;  // once per message, and only to a session
            }
            sink(pdu, index);
        });
    }
    HRESULT Changed() {
        std::function<void()> sink;
        {
            std::lock_guard guard(m_shared->lock);
            sink = m_shared->events.changed;
        }
        if (sink) sink();
        return S_OK;
    }

    std::shared_ptr<Shared> m_shared;
};

}  // namespace

struct Modem::Impl {
    ComPtr<IMbnInterfaceManager> manager;
    ComPtr<IMbnInterface> modem;
    ComPtr<IMbnRegistration> registration;
    ComPtr<IMbnSignal> signal;
    ComPtr<IMbnSms> sms;
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();
    ComPtr<Sink> sink;
    std::vector<std::pair<ComPtr<IConnectionPoint>, DWORD>> advised;
    winrt::Windows::Devices::Sms::SmsMessageRegistration router{nullptr};  // MMS notices from the SMS router

    void Advise(REFIID iid) {
        ComPtr<IConnectionPointContainer> container;
        ComPtr<IConnectionPoint> point;
        DWORD cookie = 0;
        if (SUCCEEDED(manager.As(&container)) && SUCCEEDED(container->FindConnectionPoint(iid, point.GetAddressOf())) &&
            SUCCEEDED(point->Advise(static_cast<IMbnSmsEvents*>(sink.Get()), &cookie)))  // the point asks for its interface
            advised.emplace_back(point, cookie);
    }
};

Modem::Modem(std::unique_ptr<Impl> impl) : m(std::move(impl)) {}

Modem::~Modem() {
    Stop();
    try {
        if (m->router) m->router.Unregister();
    } catch (const winrt::hresult_error&) {
    }
}

namespace {

// An MMS notice from Windows' SMS router: kept as PDUs for Android until it acknowledges them,
// given at once to a session that runs.
void OnRouted(const std::shared_ptr<Shared>& shared, const winrt::Windows::Devices::Sms::SmsMessageReceivedTriggerDetails& details) {
    using namespace winrt::Windows::Devices::Sms;
    if (details.MessageType() != SmsMessageType::Wap) return;
    const SmsWapMessage wap = details.WapMessage();
    const auto body = wap.BinaryBody();
    SYSTEMTIME time{};
    const winrt::file_time stamp = winrt::clock::to_file_time(wap.Timestamp());
    FILETIME ft{static_cast<DWORD>(stamp.value), static_cast<DWORD>(stamp.value >> 32)};
    if (!FileTimeToSystemTime(&ft, &time)) GetSystemTime(&time);
    std::vector<std::pair<unsigned, std::string>> due;
    std::function<void(const std::string&, unsigned)> sink;
    {
        std::lock_guard guard(shared->lock);
        const std::vector<std::string> pdus = WapPushPdus(ToUtf8(wap.From()), time, ToUtf8(wap.ContentType()),
                                                          std::span<const std::uint8_t>(body.data(), body.Length()), shared->reference++);
        sink = shared->events.message;
        for (const std::string& pdu : pdus) {
            const unsigned index = kRoutedBase + shared->nextRouted++ % kRoutedBase;
            shared->routed.push_back({index, pdu, static_cast<bool>(sink)});
            if (sink) due.emplace_back(index, pdu);
        }
        Bt::Log("modem: an MMS notice from Windows' SMS router ({} octets, {} part{}){}", body.Length(), pdus.size(), pdus.size() == 1 ? "" : "s",
                sink ? "" : " - kept until Android starts");
    }
    for (const auto& [index, pdu] : due) sink(pdu, index);
}

}  // namespace

void Modem::ReceiveRoutedNotices() {
    using namespace winrt::Windows::Devices::Sms;
    try {
        // A registration outlives the process that made it: an earlier host's is replaced.
        for (const SmsMessageRegistration& old : SmsMessageRegistration::AllRegistrations())
            if (old.Id() == kRegistrationId) old.Unregister();
        // Peek: the router goes on as before for everyone else.
        SmsFilterRules rules(SmsFilterActionType::Peek);
        rules.Rules().Append(SmsFilterRule(SmsMessageType::Wap));
        m->router = SmsMessageRegistration::Register(kRegistrationId, rules);
        m->router.MessageReceived([shared = m->shared](const SmsMessageRegistration&, const SmsMessageReceivedTriggerDetails& details) {
            try {
                OnRouted(shared, details);
            } catch (const winrt::hresult_error& e) {
                Bt::Log("modem: an MMS notice from Windows' SMS router could not be read (0x{:08X})", static_cast<unsigned>(e.code()));
            }
        });
    } catch (const winrt::hresult_error& e) {
        m->router = nullptr;
        Bt::Log("modem: Windows' SMS router gives no MMS notices (0x{:08X}): only those read from the modem first reach Android",
                static_cast<unsigned>(e.code()));
    }
}

void Modem::Stop() {
    for (auto& [point, cookie] : m->advised) point->Unadvise(cookie);
    m->advised.clear();
    m->sink.Reset();
    std::lock_guard guard(m->shared->lock);
    m->shared->events = {};  // a callback still running finds nothing to call
}

std::unique_ptr<Modem> Modem::Open() {
    ComPtr<IMbnInterfaceManager> manager;
    if (FAILED(CoCreateInstance(CLSID_MbnInterfaceManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(manager.GetAddressOf())))) return nullptr;
    SAFEARRAY* interfaces = nullptr;
    if (FAILED(manager->GetInterfaces(&interfaces))) return nullptr;
    ComPtr<IMbnInterface> first;
    ForEach(interfaces, [&](IUnknown* item) {
        if (!first) item->QueryInterface(IID_PPV_ARGS(first.GetAddressOf()));
    });
    SafeArrayDestroy(interfaces);
    if (!first) return nullptr;

    auto impl = std::make_unique<Impl>();
    impl->manager = manager;
    impl->modem = first;
    first.As(&impl->registration);
    first.As(&impl->signal);
    if (FAILED(first.As(&impl->sms))) return nullptr;  // a modem without SMS gives Android nothing to use
    return std::unique_ptr<Modem>(new Modem(std::move(impl)));
}

ModemState Modem::State() const {
    ModemState s;
    MBN_READY_STATE ready = MBN_READY_STATE_OFF;
    s.simReady = SUCCEEDED(m->modem->GetReadyState(&ready)) && ready == MBN_READY_STATE_INITIALIZED;

    MBN_INTERFACE_CAPS caps{};
    if (SUCCEEDED(m->modem->GetInterfaceCapability(&caps))) {
        s.imei = Narrow(caps.deviceID);
        for (BSTR text : {caps.customDataClass, caps.customBandClass, caps.deviceID, caps.manufacturer, caps.model, caps.firmwareInfo})
            SysFreeString(text);
    }
    ComPtr<IMbnSubscriberInformation> subscriber;
    if (s.simReady && SUCCEEDED(m->modem->GetSubscriberInformation(subscriber.GetAddressOf())) && subscriber) {
        BSTR text = nullptr;
        if (SUCCEEDED(subscriber->get_SubscriberID(&text))) s.imsi = Narrow(text);
        SysFreeString(text);
        text = nullptr;
        if (SUCCEEDED(subscriber->get_SimIccID(&text))) s.iccid = Narrow(text);
        SysFreeString(text);
    }
    if (m->registration) {
        MBN_REGISTER_STATE state = MBN_REGISTER_STATE_NONE;
        if (SUCCEEDED(m->registration->GetRegisterState(&state))) s.registration = Registration(state);
        BSTR text = nullptr;
        if (SUCCEEDED(m->registration->GetProviderID(&text))) s.operatorId = Narrow(text);
        SysFreeString(text);
        text = nullptr;
        if (SUCCEEDED(m->registration->GetProviderName(&text))) s.operatorName = Narrow(text);
        SysFreeString(text);
        ULONG dataClass = 0;
        if (SUCCEEDED(m->registration->GetCurrentDataClass(&dataClass))) s.accessTechnology = AccessTechnology(dataClass);
    }
    ULONG strength = MBN_RSSI_UNKNOWN;
    if (m->signal && SUCCEEDED(m->signal->GetSignalStrength(&strength)))
        s.rssi = strength <= 31 ? static_cast<int>(strength) : 99;
    ComPtr<IMbnSmsConfiguration> configuration;
    if (SUCCEEDED(m->sms->GetSmsConfiguration(configuration.GetAddressOf())) && configuration) {
        BSTR text = nullptr;
        if (SUCCEEDED(configuration->get_ServiceCenterAddress(&text))) s.smsc = Narrow(text);
        SysFreeString(text);
    }
    return s;
}

bool Modem::Send(const std::string& pduHex, unsigned length) {
    auto pending = std::make_shared<Shared::Pending>();
    const std::wstring pdu = FromUtf8(pduHex);
    {
        // Held across the call: the completion (another thread) finds the request registered.
        std::lock_guard guard(m->shared->lock);
        ULONG request = 0;
        const HRESULT hr = m->sms->SmsSendPdu(pdu.c_str(), static_cast<BYTE>(length), &request);
        if (FAILED(hr)) {
            Bt::Log("modem: sending an SMS failed at once (0x{:08X})", static_cast<unsigned>(hr));
            return false;
        }
        m->shared->sends[request] = pending;
    }
    if (WaitForSingleObject(pending->done.get(), kSendTimeoutMs) != WAIT_OBJECT_0) {
        Bt::Log("modem: no answer to an SMS within {} s", kSendTimeoutMs / 1000);
        return false;
    }
    if (FAILED(pending->status)) Bt::Log("modem: the SMS was not sent (0x{:08X})", static_cast<unsigned>(pending->status));
    return SUCCEEDED(pending->status);
}

void Modem::Start(Events events) {
    {
        std::lock_guard guard(m->shared->lock);
        m->shared->events = std::move(events);
        m->shared->delivered.clear();  // what an earlier session did not acknowledge is due again
        for (Shared::Routed& notice : m->shared->routed) notice.sent = false;
    }
    m->sink = Microsoft::WRL::Make<Sink>(m->shared);
    m->Advise(__uuidof(IMbnSmsEvents));
    m->Advise(__uuidof(IMbnSignalEvents));
    m->Advise(__uuidof(IMbnRegistrationEvents));
}

void Modem::ReadReceived() {
    // Every received message, read or not: Windows marks a message read as it arrives when no
    // session takes it (seen: a message that came while the host was off was "read" right away).
    // The bridge deletes what Android acknowledges, so what stays in the store is not in Android.
    MBN_SMS_FILTER filter{MBN_SMS_FLAG_ALL, 0};
    ULONG request = 0;
    m->sms->SmsRead(&filter, MBN_SMS_FORMAT_PDU, &request);  // answered by OnSmsReadComplete

    // And the MMS notices Windows' SMS router handed over meanwhile.
    std::vector<std::pair<unsigned, std::string>> due;
    std::function<void(const std::string&, unsigned)> sink;
    {
        std::lock_guard guard(m->shared->lock);
        sink = m->shared->events.message;
        if (!sink) return;
        for (Shared::Routed& notice : m->shared->routed)
            if (!notice.sent) {
                notice.sent = true;
                due.emplace_back(notice.index, notice.pdu);
            }
    }
    for (const auto& [index, pdu] : due) sink(pdu, index);
}

void Modem::Delete(unsigned index) {
    if (index >= kRoutedBase) {  // a notice from the SMS router: Android has it
        std::lock_guard guard(m->shared->lock);
        std::erase_if(m->shared->routed, [index](const Shared::Routed& notice) { return notice.index == index; });
        return;
    }
    MBN_SMS_FILTER filter{MBN_SMS_FLAG_INDEX, index};
    ULONG request = 0;
    m->sms->SmsDelete(&filter, &request);
    std::lock_guard guard(m->shared->lock);
    m->shared->delivered.erase(index);  // the index is free for a later message
}

void Modem::Release(unsigned index) {
    std::lock_guard guard(m->shared->lock);
    m->shared->delivered.erase(index);  // stays in the store; given again by the next read
    for (Shared::Routed& notice : m->shared->routed)
        if (notice.index == index) notice.sent = false;
}

std::optional<std::vector<StoredMessage>> Modem::ReadStore(bool unreadOnly, unsigned timeoutMs) {
    if (!m->sink) return std::nullopt;  // the answer comes through the sink: Start first
    auto read = std::make_shared<Shared::Read>();
    {
        // Held across the call: the completion (another thread) finds the request registered.
        std::lock_guard guard(m->shared->lock);
        MBN_SMS_FILTER filter{unreadOnly ? MBN_SMS_FLAG_NEW : MBN_SMS_FLAG_ALL, 0};
        ULONG request = 0;
        const HRESULT hr = m->sms->SmsRead(&filter, MBN_SMS_FORMAT_PDU, &request);
        if (FAILED(hr)) {
            Bt::Log("modem: reading the message store failed at once (0x{:08X})", static_cast<unsigned>(hr));
            return std::nullopt;
        }
        m->shared->reads[request] = read;
    }
    if (WaitForSingleObject(read->done.get(), timeoutMs) != WAIT_OBJECT_0) {
        Bt::Log("modem: the message store did not answer within {} s", timeoutMs / 1000);
        return std::nullopt;
    }
    if (FAILED(read->status)) {
        Bt::Log("modem: reading the message store failed (0x{:08X})", static_cast<unsigned>(read->status));
        return std::nullopt;
    }
    return std::move(read->messages);
}

}  // namespace WN::Tel
