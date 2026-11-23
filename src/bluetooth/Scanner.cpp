// WSA-Next © 2026 Marek Wesołowski - wesmar
#include "bluetooth/Scanner.h"
#include "bluetooth/Log.h"
#include "bluetooth/Hci.h"
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <algorithm>
#include <mutex>
#include <shared_mutex>

#pragma comment(lib, "windowsapp.lib")

namespace WN::Bt {
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;

namespace {

std::uint8_t EventType(BluetoothLEAdvertisementReceivedEventArgs const& a) {
    switch (a.AdvertisementType()) {
    case BluetoothLEAdvertisementType::ConnectableUndirected: return Hci::kAdvInd;
    case BluetoothLEAdvertisementType::ConnectableDirected: return Hci::kAdvDirectInd;
    case BluetoothLEAdvertisementType::ScannableUndirected: return Hci::kAdvScanInd;
    case BluetoothLEAdvertisementType::NonConnectableUndirected: return Hci::kAdvNonconnInd;
    case BluetoothLEAdvertisementType::ScanResponse: return Hci::kScanRsp;
    default:  // extended advertising, reported as the closest legacy type
        return a.IsConnectable() ? Hci::kAdvInd : a.IsScannable() ? Hci::kAdvScanInd : Hci::kAdvNonconnInd;
    }
}

// WinRT hands out the parsed AD structures; HCI wants them as sent: length, type, value.
// Legacy reports carry at most 31 bytes, so whole structures that do not fit are dropped.
std::vector<std::uint8_t> AdvertisingData(BluetoothLEAdvertisement const& advertisement) {
    std::vector<std::uint8_t> out;
    for (auto const& section : advertisement.DataSections()) {
        const auto buffer = section.Data();
        const std::uint32_t length = buffer.Length();
        if (length > 29 || out.size() + 2 + length > 31) continue;
        out.push_back(static_cast<std::uint8_t>(length + 1));
        out.push_back(section.DataType());
        out.insert(out.end(), buffer.data(), buffer.data() + length);
    }
    return out;
}

}  // namespace

struct Scanner::Impl {
    ReportFn onReport;
    BluetoothLEAdvertisementWatcher watcher{nullptr};
    winrt::event_token received{};
    std::mutex lock;               // Start / Stop
    std::shared_mutex delivering;  // held by callbacks: Stop waits for the ones in flight
    bool running = false;
};

Scanner::Scanner(ReportFn onReport) : m_impl(std::make_unique<Impl>()) { m_impl->onReport = std::move(onReport); }

Scanner::~Scanner() { Stop(); }

void Scanner::Start(bool active) {
    std::lock_guard guard(m_impl->lock);
    if (m_impl->running) return;
    try {
        m_impl->watcher = BluetoothLEAdvertisementWatcher();
        m_impl->watcher.ScanningMode(active ? BluetoothLEScanningMode::Active : BluetoothLEScanningMode::Passive);
        m_impl->received = m_impl->watcher.Received([impl = m_impl.get()](auto const&, BluetoothLEAdvertisementReceivedEventArgs const& a) {
            Advertisement report;
            report.address = a.BluetoothAddress();
            report.randomAddress = a.BluetoothAddressType() == BluetoothAddressType::Random;
            report.eventType = EventType(a);
            report.rssi = static_cast<std::int8_t>(std::clamp<int>(a.RawSignalStrengthInDBm(), -127, 20));
            report.data = AdvertisingData(a.Advertisement());
            std::shared_lock guard(impl->delivering);
            if (impl->running) impl->onReport(report);
        });
        {
            std::unique_lock delivering(m_impl->delivering);
            m_impl->running = true;
        }
        m_impl->watcher.Start();
        Log("scan: started ({})", active ? "active" : "passive");
    } catch (winrt::hresult_error const& e) {
        {
            std::unique_lock delivering(m_impl->delivering);
            m_impl->running = false;
        }
        m_impl->watcher = nullptr;
        Log("scan: cannot start: 0x{:08X} {}", static_cast<unsigned>(e.code()), winrt::to_string(e.message()));
    }
}

void Scanner::Stop() {
    std::lock_guard guard(m_impl->lock);
    if (!m_impl->running) return;
    {
        std::unique_lock delivering(m_impl->delivering);  // no report is delivered after this
        m_impl->running = false;
    }
    try {
        m_impl->watcher.Received(m_impl->received);
        m_impl->watcher.Stop();
    } catch (winrt::hresult_error const&) {
    }
    m_impl->watcher = nullptr;
    Log("scan: stopped");
}

std::uint64_t Scanner::AdapterAddress() {
    try {
        const auto adapter = BluetoothAdapter::GetDefaultAsync().get();
        return adapter ? adapter.BluetoothAddress() : 0;
    } catch (winrt::hresult_error const&) {
        return 0;
    }
}

}  // namespace WN::Bt
