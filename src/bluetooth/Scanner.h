// WSA-Next © 2026 Marek Wesołowski - wesmar
// BLE advertisements from the PC's adapter (WinRT BluetoothLEAdvertisementWatcher). Works for
// any device in range - no pairing, no Windows driver - and delivers the advertising data
// itself (manufacturer data, service data: what scales, sensors and beacons broadcast).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace WN::Bt {

struct Advertisement {
    std::uint64_t address = 0;       // 48-bit, as WinRT reports it (most significant byte first in text)
    bool randomAddress = false;
    std::uint8_t eventType = 0;      // HCI legacy report type (Hci::kAdvInd ...)
    std::int8_t rssi = 0;
    std::vector<std::uint8_t> data;  // AD structures: length, type, value - at most 31 bytes
};

class Scanner {
public:
    using ReportFn = std::function<void(const Advertisement&)>;
    explicit Scanner(ReportFn onReport);
    ~Scanner();
    Scanner(const Scanner&) = delete;
    Scanner& operator=(const Scanner&) = delete;

    void Start(bool active);
    void Stop();

    // Address of the PC's default Bluetooth adapter (0 = none).
    static std::uint64_t AdapterAddress();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace WN::Bt
