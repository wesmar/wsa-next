// WSA-Next © 2026 Marek Wesołowski - wesmar
#include "telephony/AtModem.h"
#include "bluetooth/Log.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <climits>
#include <format>

namespace WN::Tel {
namespace {

constexpr std::string_view kNotSupported = "+CME ERROR: 4";  // operation not supported
constexpr std::string_view kNoSim = "+CME ERROR: 10";        // SIM not inserted
// The serving cell is not known through the mobile broadband API: fixed identifiers (hex).
constexpr std::string_view kAreaCode = "0001";
constexpr std::string_view kCellId = "00000001";

bool StartsWith(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
        if (std::toupper(static_cast<unsigned char>(text[i])) != std::toupper(static_cast<unsigned char>(prefix[i]))) return false;
    return true;
}

// The comma separated integers after '=' ("+CRSM=176,12258,0,0,10" -> 176 12258 0 0 10).
std::vector<int> Numbers(std::string_view command) {
    std::vector<int> values;
    const std::size_t equals = command.find('=');
    if (equals == std::string_view::npos) return values;
    std::string_view rest = command.substr(equals + 1);
    while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        const std::string_view field = rest.substr(0, comma);
        int value = 0;
        std::from_chars(field.data(), field.data() + field.size(), value);
        values.push_back(value);
        if (comma == std::string_view::npos) break;
        rest.remove_prefix(comma + 1);
    }
    return values;
}

bool Registered(const ModemState& s) { return s.registration == 1 || s.registration == 5; }

// The length of the MNC in the operator's MCC + MNC (Poland's 260 06: two digits).
unsigned MncLength(const ModemState& s) { return s.operatorId.size() == 6 ? 3 : 2; }

}  // namespace

const SimCard& AtModem::Sim(const ModemState& s) {
    if (!m_sim) m_sim.emplace(s.iccid, MncLength(s));
    return *m_sim;
}

std::string AtModem::Registration(std::string_view prefix, int mode, const ModemState& s) const {
    std::string line = std::format("{} {},{}", prefix, mode, s.registration);
    if (mode == 2 && Registered(s)) line += std::format(",\"{}\",\"{}\",{}", kAreaCode, kCellId, s.accessTechnology);
    return line;
}

// Cuttlefish's AT+CSQ: 22 values for every radio technology (TS 27.007 8.5 has only the first
// two). The modem reports one RSSI; it goes to GSM, LTE and WCDMA, LTE also gets an RSRP derived
// from it (a 20 MHz carrier: RSRP ~ RSSI - 27 dB), everything else is "unknown".
std::string AtModem::SignalStrength(const ModemState& s) {
    const int rssi = s.rssi;
    int rsrp = INT_MAX;
    if (rssi >= 0 && rssi <= 31) rsrp = std::clamp(113 - 2 * rssi + 27, 44, 140);  // dBm * -1
    return std::format("+CSQ: {},99,-1,-1,-1,-1,-1,{},{},{},{},{},{},{},{},99,{},{},{},{},{},{}", rssi, rssi, rsrp, INT_MAX, INT_MAX, INT_MAX,
                       INT_MAX, INT_MAX, rssi, INT_MAX, INT_MAX, INT_MAX, INT_MAX, INT_MAX, INT_MAX);
}

AtModem::Reply AtModem::Command(std::string_view line) {
    Reply reply;
    if (!StartsWith(line, "AT")) return reply;  // not a command (noise)
    std::string_view rest = line.substr(2);
    if (StartsWith(rest, "D") || (StartsWith(rest, "A") && rest.size() == 1)) {  // ATD...: dial, ATA: answer
        reply.lines.emplace_back(kNotSupported);
        return reply;
    }
    if (StartsWith(rest, "+CMGS=")) {  // the PDU follows the prompt
        const std::vector<int> values = Numbers(rest);
        m_smsLength = values.empty() ? 0 : static_cast<unsigned>(values[0]);
        reply.smsPrompt = true;
        return reply;
    }
    // Commands chained with ';' ("AT+COPS=3,0;+COPS?;+COPS=3,1;+COPS?"): each answers, one final result.
    while (true) {
        const std::size_t semicolon = rest.find(';');
        bool ok = true;
        std::vector<std::string> lines = Single(rest.substr(0, semicolon), ok);
        if (!ok) {
            reply.lines.push_back(lines.empty() ? std::string(kNotSupported) : lines.back());
            return reply;
        }
        reply.lines.insert(reply.lines.end(), lines.begin(), lines.end());
        if (semicolon == std::string_view::npos) break;
        rest.remove_prefix(semicolon + 1);
    }
    reply.lines.emplace_back("OK");
    return reply;
}

// One command without "AT" ("+CREG?", "E0Q0V1"). `ok` false: the last line is the error.
std::vector<std::string> AtModem::Single(std::string_view c, bool& ok) {
    auto fail = [&ok](std::string_view error) {
        ok = false;
        return std::vector<std::string>{std::string(error)};
    };

    // Settings this modem accepts and keeps no state for.
    for (std::string_view accepted : {"E", "S0=", "H", "+CMEE=", "+CCWA=", "+CMOD=", "+CMUT=", "+CSSN=", "+COLP=", "+CSCS=", "+CUSD=",
                                      "+CGEREP=", "+CMGF=0", "+CNMI=", "+CSMS=", "+CSCB=", "%CPI=", "%CSTAT=", "%CTZV=", "%CPHS=",
                                      "+CLIR=", "+CGQREQ=", "+CGQMIN=", "+WSOS=", "+CSCA=", "+CREG=", "+CGREG=", "+CEREG="}) {
        if (!StartsWith(c, accepted)) continue;
        if (StartsWith(c, "+CREG=")) m_cregMode = Numbers(c).empty() ? 0 : Numbers(c)[0];
        if (StartsWith(c, "+CGREG=")) m_cgregMode = Numbers(c).empty() ? 0 : Numbers(c)[0];
        if (StartsWith(c, "+CEREG=")) m_ceregMode = Numbers(c).empty() ? 0 : Numbers(c)[0];
        return {};
    }
    if (c.empty()) return {};  // plain "AT"

    if (StartsWith(c, "+CFUN?")) return {std::format("+CFUN: {}", m_radio)};
    if (StartsWith(c, "+CFUN=")) {
        const std::vector<int> v = Numbers(c);
        m_radio = !v.empty() && v[0] != 0 ? 1 : 0;
        return {};
    }

    const ModemState s = m_modem.State();  // the commands below answer from the modem
    if (StartsWith(c, "+CPIN?")) return s.simReady ? std::vector<std::string>{"+CPIN: READY"} : fail(kNoSim);
    if (StartsWith(c, "+CIMI")) return s.simReady ? std::vector<std::string>{s.imsi} : fail(kNoSim);
    if (StartsWith(c, "+CICCID")) return s.simReady ? std::vector<std::string>{s.iccid} : fail(kNoSim);
    if (StartsWith(c, "+CGSN")) return {s.imei};
    if (StartsWith(c, "+CRSM=")) {
        if (!s.simReady) return fail(kNoSim);
        const std::vector<int> v = Numbers(c);
        if (v.size() < 5) return fail(kNotSupported);
        return {Sim(s).Command(v[0], v[1], v[2], v[3], v[4])};
    }
    if (StartsWith(c, "+CLCK=\"SC\",2") || StartsWith(c, "+CLCK=\"FD\",2")) return {"+CLCK: 0"};  // no PIN lock, no FDN

    if (StartsWith(c, "+COPS?")) {
        if (!Registered(s) || m_radio == 0) return {"+COPS: 0"};
        const std::string& value = m_copsFormat == 2 ? s.operatorId : s.operatorName;
        return {std::format("+COPS: 0,{},\"{}\",{}", m_copsFormat, value, s.accessTechnology)};
    }
    if (StartsWith(c, "+COPS=?")) {
        if (!Registered(s)) return {"+COPS: "};
        return {std::format("+COPS: (2,\"{}\",\"{}\",\"{}\",{})", s.operatorName, s.operatorName, s.operatorId, s.accessTechnology)};
    }
    if (StartsWith(c, "+COPS=")) {
        const std::vector<int> v = Numbers(c);
        if (!v.empty() && v[0] == 3 && v.size() > 1) m_copsFormat = v[1];  // the format of +COPS?
        else if (!v.empty() && v[0] == 1) return fail(kNotSupported);  // manual selection: the PC's modem chooses
        return {};
    }
    if (StartsWith(c, "+CREG?")) return {Registration("+CREG:", m_cregMode, s)};
    if (StartsWith(c, "+CGREG?")) return {Registration("+CGREG:", m_cgregMode, s)};
    if (StartsWith(c, "+CEREG?")) return {Registration("+CEREG:", m_ceregMode, s)};
    if (StartsWith(c, "+CSQ")) return {SignalStrength(s)};
    if (StartsWith(c, "+CSCA?")) {
        if (s.smsc.empty()) return fail("+CMS ERROR: 330");  // SMSC address unknown
        return {std::format("+CSCA: \"{}\",{}", s.smsc, s.smsc.starts_with('+') ? 145 : 129)};
    }
    if (StartsWith(c, "+CNMA")) {
        const std::vector<int> v = Numbers(c);
        if (!m_unacknowledged.empty()) {
            const unsigned index = m_unacknowledged.front();
            m_unacknowledged.pop_front();
            if (v.empty() || v[0] != 2) m_modem.Delete(index);  // Android has it
            else m_modem.Release(index);                         // refused (e.g. Android's storage is full): kept for later
        }
        return {};
    }
    if (StartsWith(c, "+CMUT?")) return {"+CMUT: 0"};
    if (StartsWith(c, "+CLCC") || StartsWith(c, "+CGACT?") || StartsWith(c, "+CGDCONT?") || StartsWith(c, "+CNUM")) return {};  // none
    if (StartsWith(c, "+CUSATD?")) return {"+CUSATD: 0,0"};

    // Calls, packet data, CDMA, the card's channels and anything else: not this modem.
    Bt::Log("modem: unsupported AT{}", std::string(c.substr(0, 40)));
    return fail(kNotSupported);
}

AtModem::Reply AtModem::SmsPdu(std::string_view pdu) {
    Reply reply;
    // The RIL writes the service centre part ("00": the default one) and the TPDU, as AT+CMGS
    // takes them; the mobile broadband API takes the same, with the TPDU's length.
    std::string hex(pdu);
    std::erase_if(hex, [](char ch) { return std::isspace(static_cast<unsigned char>(ch)) != 0; });
    if (!hex.empty() && m_modem.Send(hex, m_smsLength)) {
        reply.lines.push_back(std::format("+CMGS: {}", m_messageReference));
        m_messageReference = (m_messageReference + 1) % 256;
        reply.lines.emplace_back("OK");
    } else {
        reply.lines.emplace_back("+CMS ERROR: 500");  // unknown error
    }
    return reply;
}

std::vector<std::string> AtModem::NewMessage(const std::string& pduHex, unsigned index) {
    // 27.005 +CMT: ,<length> with the TPDU's length (the PDU minus the service centre part).
    unsigned smscOctets = 0;
    if (pduHex.size() >= 2) std::from_chars(pduHex.data(), pduHex.data() + 2, smscOctets, 16);
    const std::size_t octets = pduHex.size() / 2;
    if (octets <= smscOctets + 1) {
        m_modem.Release(index);  // not a PDU Android could take
        return {};
    }
    m_unacknowledged.push_back(index);
    return {std::format("+CMT: ,{}", octets - smscOctets - 1), pduHex};
}

std::vector<std::string> AtModem::Changes() {
    std::vector<std::string> lines;
    const ModemState s = m_modem.State();
    if (s.registration != m_lastRegistration || s.accessTechnology != m_lastTechnology) {
        m_lastRegistration = s.registration;
        m_lastTechnology = s.accessTechnology;
        // The unsolicited form has no <n>: "+CREG: <stat>[,<lac>,<ci>,<AcT>]".
        auto unsolicited = [&](std::string_view prefix, int mode) {
            if (mode == 0) return;
            std::string line = std::format("{} {}", prefix, s.registration);
            if (mode == 2 && Registered(s)) line += std::format(",\"{}\",\"{}\",{}", kAreaCode, kCellId, s.accessTechnology);
            lines.push_back(std::move(line));
        };
        unsolicited("+CREG:", m_cregMode);
        unsolicited("+CGREG:", m_cgregMode);
    }
    if (s.rssi != m_lastRssi) {
        m_lastRssi = s.rssi;
        lines.push_back(SignalStrength(s));
    }
    return lines;
}

}  // namespace WN::Tel
