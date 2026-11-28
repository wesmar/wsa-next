// WSA-Next © 2026 Marek Wesołowski - wesmar
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include "host/Updates.h"
#include "bluetooth/Log.h"
#include "core/Manifest.h"
#include "core/Update.h"
#include "support/Handle.h"
#include "support/HostEvents.h"
#include <string>

#pragma comment(lib, "iphlpapi.lib")

namespace WN::Host {
namespace {

// The background check gives a server less time than the window does: a weak link costs a few
// seconds, never a stuck host when it is told to stop.
constexpr unsigned long kConnectMs = 5000, kIoMs = 8000;

VOID CALLBACK OnConnectivity(PVOID context, NL_NETWORK_CONNECTIVITY_HINT) { SetEvent(static_cast<HANDLE>(context)); }

// Internet access: not the local network alone, not a captive portal. The PC's mobile modem counts
// like any other network (no metered-connection exceptions).
bool Online() {
    NL_NETWORK_CONNECTIVITY_HINT hint{};
    if (GetNetworkConnectivityHint(&hint) != NO_ERROR) return false;
    return hint.ConnectivityLevel == NetworkConnectivityLevelHintInternetAccess;
}

std::string Ascii(const std::wstring& text) {
    std::string out;
    for (wchar_t c : text) out += c < 0x80 ? static_cast<char>(c) : '?';
    return out;
}

}  // namespace

void RunUpdateCheck(std::stop_token stop) {
    const KernelHandle wake(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    const KernelHandle stopped(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!wake || !stopped) return;
    std::stop_callback onStop(stop, [&stopped] { SetEvent(stopped.get()); });
    // The first report comes at once (initial notification): the check at logon.
    HANDLE notification = nullptr;
    if (NotifyNetworkConnectivityHintChange(&OnConnectivity, wake.get(), TRUE, &notification) != NO_ERROR) {
        notification = nullptr;
        SetEvent(wake.get());  // no reports: one check now
    }
    const HANDLE events[] = {stopped.get(), wake.get()};
    while (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        if (!Core::Update::AutomaticCheck() || !Core::Update::CheckDue() || !Online()) continue;
        const std::wstring url = Core::Update::SavedSourceUrl();
        if (url.empty()) continue;
        auto manifest = Core::LoadManifest(url, stop, kConnectMs, kIoMs);
        if (!manifest || !manifest->official) continue;  // tried again at the next change of the network
        if (!Core::Update::Record(*manifest, Core::Update::SavedFolder())) continue;
        const Core::Update::Pending pending = Core::Update::Available(Core::Update::SavedFolder());
        if (!pending.app.empty()) Bt::Log("host: WSA-Next {} is available", Ascii(pending.app));
        if (!pending.release.empty()) Bt::Log("host: Android release {} is available", Ascii(pending.release));
        NotifyHostChanged();  // the icon's dot, the window
    }
    if (notification) CancelMibChangeNotify2(notification);  // waits for a report in progress
}

}  // namespace WN::Host
