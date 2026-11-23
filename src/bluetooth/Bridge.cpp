// WSA-Next © 2026 Marek Wesołowski - wesmar
// Bluetooth bridge: Android's HCI HAL in WSA <-> Hyper-V socket <-> emulated controller <-> WinRT.
#include <winsock2.h>
#include <Windows.h>
#include "bluetooth/Bridge.h"
#include "bluetooth/Log.h"
#include "bluetooth/Controller.h"
#include "bluetooth/H4.h"
#include "host/Guest.h"
#include "bluetooth/Scanner.h"
#include "support/Common.h"
#include "support/HostEvents.h"
#include <mutex>
#include <vector>

namespace WN::Bt {
namespace {

// Signaled while Android's HAL is connected (shown by the window, which hears of the change).
class ConnectedSignal {
public:
    ConnectedSignal() : m_event(CreateEventW(nullptr, TRUE, FALSE, kConnectedEvent)) {
        if (m_event) SetEvent(m_event);
        NotifyHostChanged();
    }
    ~ConnectedSignal() {
        if (m_event) {
            ResetEvent(m_event);
            CloseHandle(m_event);
        }
        NotifyHostChanged();
    }
    ConnectedSignal(const ConnectedSignal&) = delete;
    ConnectedSignal& operator=(const ConnectedSignal&) = delete;

private:
    HANDLE m_event;
};

// One connection of the Android HAL: H4 in, H4 out, until either side closes. Blocking reads;
// a stop request shuts the socket down (Guest::Serve), which ends them.
void RunSession(std::uintptr_t handle, std::uint64_t address, std::stop_token) {
    const ConnectedSignal connected;
    const auto socket = static_cast<SOCKET>(handle);
    std::mutex sendLock;
    auto send = [&](Hci::PacketType type, std::vector<std::uint8_t> packet) {
        const std::vector<std::uint8_t> frame = H4Frame(type, packet);
        std::lock_guard guard(sendLock);
        std::size_t done = 0;
        while (done < frame.size()) {
            const int n = ::send(socket, reinterpret_cast<const char*>(frame.data() + done), static_cast<int>(frame.size() - done), 0);
            if (n <= 0) return;
            done += static_cast<std::size_t>(n);
        }
    };
    Controller controller(send, address);
    H4Reader reader([&](Hci::PacketType type, std::span<const std::uint8_t> packet) {
        switch (type) {
        case Hci::PacketType::Command: controller.OnCommand(packet); break;
        case Hci::PacketType::Acl: controller.OnAcl(packet); break;
        default: Log("hci: packet type {} ignored ({} bytes)", static_cast<int>(type), packet.size()); break;
        }
    });
    std::vector<std::uint8_t> buffer(64 * 1024);
    for (;;) {
        const int n = recv(socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
        if (n <= 0) break;
        if (!reader.Feed(std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(n)))) {
            Log("hci: broken H4 framing, dropping the connection");
            break;
        }
    }
}

}  // namespace

bool AndroidConnected() {
    const HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, kConnectedEvent);
    if (!event) return false;
    const bool connected = WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
    CloseHandle(event);
    return connected;
}

BridgeResult RunBridge(std::stop_token stop) {
    Log("Bluetooth bridge {}", kBridgeVersion);
    if (!IsElevated()) {
        Log("administrator rights are required (Host Compute Service, Hyper-V socket registration)");
        return BridgeResult::NotElevated;
    }
    const std::uint64_t address = Scanner::AdapterAddress();
    if (!address) {
        Log("no Bluetooth adapter is available in Windows");
        return BridgeResult::NoAdapter;
    }
    Log("adapter {:012X}", address);
    Guest::Serve(kBridgePort, "Android Bluetooth stack", [address](std::uintptr_t socket, std::stop_token s) { RunSession(socket, address, s); },
                 stop);
    Log("bridge stopped");
    return BridgeResult::Stopped;
}

}  // namespace WN::Bt
