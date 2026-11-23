// WSA-Next © 2026 Marek Wesołowski - wesmar
// Elevation bridge: Android's elevation daemon in WSA <-> Hyper-V socket ("WSEV") <-> Windows Hello.
#include <winsock2.h>
#include <Windows.h>
#include <objbase.h>
#include "biometrics/ElevationBridge.h"
#include "biometrics/ElevationProtocol.h"
#include "biometrics/WindowsHello.h"
#include "bluetooth/Log.h"
#include "host/Guest.h"
#include "support/Common.h"
#include "support/HostEvents.h"
#include "support/Text.h"
#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace WN::Bio::Elevation {
namespace {

constexpr std::size_t kShownPackage = 200, kShownCommand = 300;

bool RecvExact(SOCKET s, void* dst, std::size_t size) {
    auto* out = static_cast<char*>(dst);
    std::size_t done = 0;
    while (done < size) {
        const int n = recv(s, out + done, static_cast<int>(size - done), 0);
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

bool SendAll(SOCKET s, const void* src, std::size_t size) {
    const auto* in = static_cast<const char*>(src);
    std::size_t done = 0;
    while (done < size) {
        const int n = ::send(s, in + done, static_cast<int>(size - done), 0);
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

struct Request {
    std::uint32_t id = 0, uid = 0;
    std::string package, command;
};

std::optional<Request> ParseRequest(std::span<const std::uint8_t> p) {
    std::size_t at = 0;
    auto u32 = [&](std::uint32_t& v) {
        if (p.size() - at < 4) return false;
        v = p[at] | (p[at + 1] << 8) | (p[at + 2] << 16) | (static_cast<std::uint32_t>(p[at + 3]) << 24);
        at += 4;
        return true;
    };
    auto text = [&](std::string& v) {
        if (p.size() - at < 2) return false;
        const std::size_t n = p[at] | (p[at + 1] << 8);
        at += 2;
        if (p.size() - at < n) return false;
        v.assign(reinterpret_cast<const char*>(p.data() + at), n);
        at += n;
        return true;
    };
    Request r;
    if (!u32(r.id) || !u32(r.uid) || !text(r.package) || !text(r.command)) return std::nullopt;
    return r;
}

// What the guest says is shown, but tamed: package names keep their own alphabet, a command loses
// control and direction-changing characters (which could make it read as something else).
std::wstring ShownPackage(const std::string& package) {
    std::wstring out;
    for (const char c : package.substr(0, kShownPackage)) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ',' || c == ':' || c == '-';
        out += plain ? static_cast<wchar_t>(c) : L'?';
    }
    return out;
}

std::wstring ShownCommand(const std::string& command) {
    std::wstring out;
    for (const wchar_t c : FromUtf8(command)) {
        // C0/C1 controls, zero-width and direction marks, line/paragraph separators, BOM.
        const bool hidden = c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0x061C || (c >= 0x200B && c <= 0x200F) || c == 0x2028 ||
                            c == 0x2029 || (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069) || c == 0xFEFF;
        out += hidden ? L'?' : c;
        if (out.size() >= kShownCommand) {
            out += L"…";
            break;
        }
    }
    return out;
}

// For the log: one line per entry, whatever the guest sent.
std::string OneLine(std::string text) {
    for (char& c : text)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = '?';
    return text;
}

// Who asks and for what, as the dialog and the notification name them.
std::wstring ShownApp(const Request& r) {
    if (r.uid == 0) return L"root";
    if (r.uid == 2000) return L"adb shell";
    if (!r.package.empty()) return ShownPackage(r.package);
    return Text::Get(IDS_ELEVATE_APP_UID);
}

std::wstring ShownWhat(const Request& r) {
    return r.command.empty() ? Text::Get(IDS_ELEVATE_SHELL) : L"“" + ShownCommand(r.command) + L"”";
}

Status MapVerify(VerifyResult result) {
    switch (result) {
    case VerifyResult::Verified: return Status::Granted;
    case VerifyResult::NotAvailable: return Status::NotAvailable;
    case VerifyResult::Busy: return Status::Busy;
    default: return Status::Denied;
    }
}

const char* StatusName(Status s) {
    switch (s) {
    case Status::Granted: return "granted";
    case Status::NotAvailable: return "refused (Windows Hello is not set up)";
    case Status::Busy: return "refused (busy)";
    default: return "denied";
    }
}

void RunSession(std::uintptr_t handle, bool suForApps) {
    const auto socket = static_cast<SOCKET>(handle);
    std::mutex sendLock;
    {
        const FrameHeader config{kMagic, static_cast<std::uint8_t>(Op::Config), static_cast<std::uint8_t>(suForApps ? 1 : 0), 0};
        SendAll(socket, &config, sizeof config);
    }
    auto answer = [&](Status status, std::uint32_t id) {
        // One message, one buffer: header and the request id.
        std::uint8_t message[sizeof(FrameHeader) + 4];
        const FrameHeader frame{kMagic, static_cast<std::uint8_t>(Op::Answer), static_cast<std::uint8_t>(status), 4};
        std::memcpy(message, &frame, sizeof frame);
        for (int i = 0; i < 4; ++i) message[sizeof frame + i] = static_cast<std::uint8_t>(id >> (8 * i));
        std::lock_guard guard(sendLock);
        SendAll(socket, message, sizeof message);
    };

    std::atomic<bool> deciding{false};
    std::atomic<std::uint32_t> current{0};
    std::stop_source cancel;
    std::jthread decider;
    for (;;) {
        FrameHeader frame{};
        if (!RecvExact(socket, &frame, sizeof frame)) break;
        if (frame.magic != kMagic || frame.payloadLen > kMaxPayload) {
            Bt::Log("elevate: invalid frame (magic=0x{:08X}, len={}), dropping the connection", frame.magic, frame.payloadLen);
            break;
        }
        std::vector<std::uint8_t> payload(frame.payloadLen);
        if (!payload.empty() && !RecvExact(socket, payload.data(), payload.size())) break;

        if (static_cast<Op>(frame.op) == Op::Request) {
            const auto request = ParseRequest(payload);
            if (!request) {
                Bt::Log("elevate: malformed request, dropping the connection");
                break;
            }
            if (deciding.exchange(true)) {
                answer(Status::Busy, request->id);
                continue;
            }
            if (decider.joinable()) decider.join();  // the previous request has been answered
            cancel = std::stop_source();
            current = request->id;
            decider = std::jthread([&, r = *request, token = cancel.get_token()](std::stop_token) {
                const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                const std::wstring app = ShownApp(r), what = ShownWhat(r);
                const std::wstring prompt = Text::Format(IDS_ELEVATE_PROMPT, app, r.uid, what);
                Bt::Log("elevate: {} (uid {}) asks for root: {}", OneLine(r.package.empty() ? "?" : r.package), r.uid,
                        r.command.empty() ? std::string("<interactive shell>") : OneLine(r.command));
                const Status status = MapVerify(RequestVerification(prompt, token));
                Bt::Log("elevate: request {} {}", r.id, StatusName(status));
                answer(status, r.id);
                // Every grant shows on Windows afterwards too (the host's icon), not only in the log.
                if (status == Status::Granted) HostNotice(Text::Get(IDS_TRAY_ROOT_GRANTED), Text::Format(IDS_TRAY_ROOT_GRANTED_TEXT, app, r.uid, what));
                deciding = false;
                if (SUCCEEDED(com)) CoUninitialize();
            });
        } else if (static_cast<Op>(frame.op) == Op::Cancel && payload.size() >= 4) {
            const std::uint32_t id = payload[0] | (payload[1] << 8) | (payload[2] << 16) | (static_cast<std::uint32_t>(payload[3]) << 24);
            if (deciding && id == current) cancel.request_stop();
        }
    }
    cancel.request_stop();  // the daemon is gone: close a dialog that is still open
}

}  // namespace

void RunBridge(std::stop_token stop, bool suForApps) {
    if (!IsElevated()) {
        Bt::Log("elevate: administrator rights are required (Host Compute Service)");
        return;
    }
    Bt::Log("elevation bridge ready (vsock port 0x{:08X}, su for root apps {})", kBridgePort, suForApps ? "on" : "off");
    Guest::Serve(kBridgePort, "Android elevation daemon", [suForApps](std::uintptr_t socket, std::stop_token) { RunSession(socket, suForApps); }, stop);
    Bt::Log("elevation bridge stopped");
}

}  // namespace WN::Bio::Elevation
