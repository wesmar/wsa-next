// WSA-Next © 2026 Marek Wesołowski - wesmar
// The SIM files Android reads through AT+CRSM. Windows does not give raw access to the card, so
// the bridge serves a small USIM: the file layouts (FCP templates) of the Android emulator's test
// card (Cuttlefish modem simulator, iccprofile_for_sim0.xml, Apache 2.0) with the real card's
// ICCID and the length of its MNC. Files it does not have answer "file not found" (6A82), as a
// card does.
#pragma once
#include <string>

namespace WN::Tel {

class SimCard {
public:
    // iccid: digits as on the card; mncLength: 2 or 3 (from the operator's MCC+MNC).
    SimCard(std::string iccid, unsigned mncLength);

    // AT+CRSM=<command>,<fileid>,<p1>,<p2>,<p3>[,<data>]: the "+CRSM: <sw1>,<sw2>[,<data>]" line.
    std::string Command(int command, int fileId, int p1, int p2, int p3) const;

private:
    std::string m_iccidBcd;  // EF_ICCID content: the digits nibble-swapped, padded with F
    unsigned m_mncLength;
};

}  // namespace WN::Tel
