// WSA-Next © 2026 Marek Wesołowski - wesmar
// The AT side of the bridge: answers the commands of Cuttlefish's reference RIL (27.007 / 27.005,
// SMS in PDU mode) from the PC's modem. Formats follow Cuttlefish's modem simulator, which that
// RIL is written against. No calls and no packet data: dialing and data contexts answer
// "operation not supported", so Android keeps both off.
#pragma once
#include "telephony/Modem.h"
#include "telephony/SimCard.h"
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace WN::Tel {

class AtModem {
public:
    explicit AtModem(Modem& modem) : m_modem(modem) {}

    struct Reply {
        std::vector<std::string> lines;  // each sent as "\r\n<line>\r\n"
        bool smsPrompt = false;          // then "> ": the next input is a PDU ended by Ctrl-Z
    };
    // One command line from the RIL (without its terminator).
    Reply Command(std::string_view line);
    // The PDU (hex) the RIL wrote after AT+CMGS's prompt: sends it. Blocks until the modem
    // answers; safe to call without the lock that guards Command and Changes.
    Reply SmsPdu(std::string_view pdu);

    // An incoming SMS-DELIVER PDU (hex, with the service centre): the +CMT lines. `index` is
    // deleted from the modem's store once the RIL acknowledges the message (AT+CNMA=1).
    std::vector<std::string> NewMessage(const std::string& pduHex, unsigned index);
    // Unsolicited lines for changes of registration or signal since the last ones sent.
    std::vector<std::string> Changes();

private:
    std::vector<std::string> Single(std::string_view command, bool& ok);
    std::string Registration(std::string_view prefix, int mode, const ModemState& s) const;
    static std::string SignalStrength(const ModemState& s);
    const SimCard& Sim(const ModemState& s);

    Modem& m_modem;
    int m_cregMode = 0, m_cgregMode = 0, m_ceregMode = 0;  // 27.007 <n> of +CREG / +CGREG / +CEREG
    int m_copsFormat = 0;                                  // +COPS=3,<format>: 0 long, 1 short, 2 numeric
    int m_radio = 1;                                       // +CFUN
    unsigned m_smsLength = 0;                              // TPDU octets announced by +CMGS
    unsigned m_messageReference = 0;
    std::optional<SimCard> m_sim;
    std::deque<unsigned> m_unacknowledged;  // store indexes of messages given to the RIL
    // What the RIL last heard unsolicited (registration, technology, signal).
    int m_lastRegistration = -1, m_lastTechnology = -1, m_lastRssi = -1;
};

}  // namespace WN::Tel
