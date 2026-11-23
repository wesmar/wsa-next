// WSA-Next © 2026 Marek Wesołowski - wesmar
#include <winsock2.h>
#include <Windows.h>
#include "telephony/ModemBridge.h"
#include "bluetooth/Log.h"
#include "host/Guest.h"
#include "telephony/AtModem.h"
#include "telephony/Modem.h"
#include "telephony/MmsRelay.h"
#include "telephony/Protocol.h"
#include <objbase.h>
#include <cctype>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace WN::Tel {
namespace {

constexpr char kCtrlZ = 0x1A;  // ends a PDU (27.005)
constexpr char kEscape = 0x1B;  // cancels a PDU

// Writes whole lines; a stream may take fewer bytes per call. Serialized by the caller.
bool SendAll(SOCKET socket, const std::string& data) {
    for (std::size_t done = 0; done < data.size();) {
        const int n = ::send(socket, data.data() + done, static_cast<int>(data.size() - done), 0);
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

// "AT", an optional prefix (+ % $ & ^) and the letters after it: "AT+CNMI=1,2" -> "AT+CNMI".
std::string CommandName(const std::string& line) {
    std::size_t end = line.starts_with("AT") ? 2 : 0;
    if (end < line.size() && std::string_view("+%$&^").contains(line[end])) ++end;
    while (end < line.size() && std::isalpha(static_cast<unsigned char>(line[end])) && end < 16) ++end;
    return line.substr(0, end);
}

std::string Framed(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) out += "\r\n" + line + "\r\n";
    return out;
}

// One connection from the RIL: commands in, answers and unsolicited lines out.
void RunSession(Modem& modem, std::uintptr_t handle, std::stop_token) {
    const auto socket = static_cast<SOCKET>(handle);
    AtModem at(modem);
    std::mutex atLock;    // AtModem's state: the session thread and the modem's notifications
    std::mutex sendLock;  // the socket's writes
    auto send = [&](const std::vector<std::string>& lines) {
        if (lines.empty()) return;
        std::lock_guard guard(sendLock);
        SendAll(socket, Framed(lines));
    };

    Modem::Events events;
    events.message = [&](const std::string& pdu, unsigned index) {
        std::vector<std::string> lines;
        {
            std::lock_guard guard(atLock);
            lines = at.NewMessage(pdu, index);
        }
        Bt::Log("modem: SMS for Android ({} octets)", pdu.size() / 2);
        send(lines);
    };
    events.changed = [&] {
        std::vector<std::string> lines;
        {
            std::lock_guard guard(atLock);
            lines = at.Changes();
        }
        send(lines);
    };
    modem.Start(std::move(events));

    std::string input;
    std::set<std::string> seen;  // command names logged once per session
    bool pdu = false;  // after AT+CMGS's prompt: a PDU until Ctrl-Z
    char chunk[1024];
    for (;;) {
        const int n = recv(socket, chunk, static_cast<int>(sizeof chunk), 0);
        if (n <= 0) break;  // the RIL went away, or the host stops (Guest::Serve shuts the socket)
        input.append(chunk, static_cast<std::size_t>(n));
        for (;;) {
            if (pdu) {
                const std::size_t end = input.find_first_of(std::string{kCtrlZ, kEscape});
                if (end == std::string::npos) break;
                const bool cancel = input[end] == kEscape;
                const std::string body = input.substr(0, end);
                input.erase(0, end + 1);
                pdu = false;
                if (cancel) {
                    send({"OK"});
                    continue;
                }
                send(at.SmsPdu(body).lines);  // waits for the network without holding atLock
                continue;
            }
            const std::size_t end = input.find_first_of("\r\n");
            if (end == std::string::npos) break;
            const std::string line = input.substr(0, end);
            input.erase(0, end + 1);
            if (line.empty()) continue;
            // The order in which Android's RIL first uses each command: the name only ("AT+CRSM",
            // "ATD"), never values such as a dialled number.
            if (const std::string name = CommandName(line); seen.insert(name).second) Bt::Log("modem: RIL first sent {}", name);
            AtModem::Reply reply;
            {
                std::lock_guard guard(atLock);
                reply = at.Command(line);
            }
            if (reply.smsPrompt) {
                pdu = true;
                std::lock_guard guard(sendLock);
                SendAll(socket, "\r\n> ");
            } else {
                send(reply.lines);
            }
            // Then the messages that came while Android was off follow. Two signs that Android
            // listens: the RIL turns on message indications (AT+CNMI, sent only from its
            // onSIMReady, which a reconnect may skip), or Android's telephony asks the card's
            // IMSI (AT+CIMI, only ever on Android's request: its handlers are registered by then).
            if (line.contains("+CNMI=") || line.starts_with("AT+CIMI")) {
                Bt::Log("modem: Android takes messages ({}) - giving it the ones waiting in the modem", line.contains("+CNMI=") ? "CNMI" : "CIMI");
                modem.ReadReceived();
            }
        }
    }
    modem.Stop();  // no notification reaches this session's objects after this
}

}  // namespace

void RunBridge(std::stop_token stop) {
    const std::unique_ptr<Modem> modem = Modem::Open();
    if (!modem) {
        Bt::Log("modem: no mobile broadband modem on this PC - Android shows no SIM");
        return;
    }
    const ModemState s = modem->State();
    Bt::Log("modem: {} - SIM {}, network {} ({}), signal {}", s.imei.empty() ? "no IMEI" : "ready", s.simReady ? "ready" : "not ready",
            s.operatorName.empty() ? "none" : s.operatorName, s.operatorId, s.rssi);
    // MMS notices: Windows' SMS router takes them out of the modem's store; it hands them over.
    modem->ReceiveRoutedNotices();
    // MMS: Android's exchanges go through an APN this modem opens for them (its own thread, in
    // the multithreaded apartment its WinRT calls need). Ends with `stop`, like the RIL's.
    const std::jthread mms([stop](std::stop_token) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        RunMmsRelay(stop);
        if (SUCCEEDED(com)) CoUninitialize();
    });
    Guest::Serve(kRilPort, "Android RIL", [&](std::uintptr_t socket, std::stop_token session) { RunSession(*modem, socket, session); },
                 stop);
}

}  // namespace WN::Tel
