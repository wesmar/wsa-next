// WSA-Next © 2026 Marek Wesołowski - wesmar
#include <winsock2.h>
#include <Windows.h>
#include "host/Registration.h"
#include "bluetooth/Log.h"
#include "host/Guest.h"
#include "support/Browser.h"
#include "support/HostEvents.h"
#include "support/Settings.h"
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <string>

namespace WN::Host {
namespace {

constexpr wchar_t kPage[] = L"https://www.google.com/android/uncertified";
constexpr const wchar_t* kDoneSetting = kGooglePlayRegisteredSetting;  // a digest of the ID registered last

std::mutex g_lock;
std::wstring g_pending;  // the ID Android offered last
std::atomic<bool> g_requested{false};  // asked for from the window or the command line before the ID came
std::wstring g_shown;    // the ID whose notification this host has shown (Android offers it at
                         // every start and every check-in of Play services: several in seconds)

// FNV-1a, 64 bits: stable between runs; the ID itself is not kept.
std::wstring Digest(const std::wstring& id) {
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (const wchar_t c : id) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 0x100000001B3ull;
    }
    return std::format(L"{:016X}", hash);
}

bool RecvAll(SOCKET socket, char* data, std::size_t size) {
    for (std::size_t done = 0; done < size;) {
        const int n = recv(socket, data + done, static_cast<int>(size - done), 0);
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

bool CopyToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    bool copied = false;
    EmptyClipboard();
    if (const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t))) {
        if (void* target = GlobalLock(memory)) {
            std::memcpy(target, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(memory);
            copied = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        }
        if (!copied) GlobalFree(memory);  // the clipboard owns it once set
    }
    CloseClipboard();
    return copied;
}

// One connection from wsa-elevated: "RGID", one length byte and the decimal ID, any number of
// times; one reply byte for each.
void Serve(SOCKET socket) {
    for (;;) {
        char magic[4]{};
        unsigned char length = 0;
        if (!RecvAll(socket, magic, sizeof magic) || std::memcmp(magic, "RGID", 4) != 0) return;
        if (!RecvAll(socket, reinterpret_cast<char*>(&length), 1) || length == 0 || length > 20) return;
        std::string digits(length, '\0');
        if (!RecvAll(socket, digits.data(), digits.size()) || digits.find_first_not_of("0123456789") != std::string::npos) return;
        const std::wstring id(digits.begin(), digits.end());
        bool shownBefore = false, known = false;
        {
            std::lock_guard guard(g_lock);
            known = !g_pending.empty();
            g_pending = id;
            shownBefore = g_shown == id;
        }
        if (!known) {
            Settings::WriteDword(kGooglePlayIdKnownSetting, 1);
            NotifyHostChanged();  // the window's "Register" no longer has to start Android first
        }
        char reply = 'O';
        if (g_requested.exchange(false) && RegisterWithGoogle(nullptr)) {
            Bt::Log("host: Google Play registration - asked for from WSA-Next, done now that Android handed over the ID");
        } else if (Settings::Read(kDoneSetting) == Digest(id)) {
            Bt::Log("host: Google Play registration - this installation was registered from Windows before");
        } else if (shownBefore) {
            reply = 'S';  // the notification is out already
        } else if (HostRegistrationNotice()) {
            reply = 'S';
            std::lock_guard guard(g_lock);
            g_shown = id;
            Bt::Log("host: Google Play registration - offered in a Windows notification");
        } else {
            reply = 'N';
            Bt::Log("host: Google Play registration - no notification-area icon, Android shows its own notification");
        }
        if (::send(socket, &reply, 1, 0) != 1) return;
    }
}

}  // namespace

void RunRegistration(std::stop_token stop) {
    Guest::Serve(kRegistrationPort, "Android registration helper", [](std::uintptr_t socket, std::stop_token) { Serve(static_cast<SOCKET>(socket)); },
                 stop);
}

std::wstring PendingRegistrationId() {
    std::lock_guard guard(g_lock);
    return g_pending;
}

bool RegisterWithGoogle(void* owner) {
    const std::wstring id = PendingRegistrationId();
    if (id.empty()) return false;
    const bool copied = CopyToClipboard(static_cast<HWND>(owner), id);
    OpenInBrowser(kPage);  // unelevated, its window to the front (the user's click allows it)
    Settings::Write(kDoneSetting, Digest(id));
    Bt::Log("host: Google Play registration page opened{}", copied ? ", device ID copied" : ", the device ID could not be copied");
    NotifyHostChanged();  // the window shows "registered from this PC"
    return true;
}

void RequestRegistration() {
    if (RegisterWithGoogle(nullptr)) return;
    g_requested = true;
    Bt::Log("host: Google Play registration asked for - done when Android hands over the device ID");
}

}  // namespace WN::Host
