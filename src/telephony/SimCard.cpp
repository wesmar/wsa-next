// WSA-Next © 2026 Marek Wesołowski - wesmar
#include "telephony/SimCard.h"
#include <format>
#include <string_view>

namespace WN::Tel {
namespace {

// 27.007 AT+CRSM commands (ETSI TS 102 221 instruction bytes).
constexpr int kReadBinary = 176;    // B0
constexpr int kReadRecord = 178;    // B2
constexpr int kGetResponse = 192;   // C0
constexpr int kUpdateBinary = 214;  // D6
constexpr int kUpdateRecord = 220;  // DC

enum class Structure { Transparent, LinearFixed };

// A file of the served USIM: its FCP template (the GET RESPONSE data) and its content. Linear
// fixed files answer every record with `content`. Layouts from Cuttlefish's
// iccprofile_for_sim0.xml; the test card's fake numbers (MSISDN, voice mail, forbidden networks)
// are left out - empty records, as on a card that has none.
struct File {
    int id;
    Structure structure;
    std::string_view fcp;
    std::string_view content;
    int records;  // linear fixed only
};

constexpr std::string_view kEmpty28 = "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF";

constexpr File kFiles[] = {
    // EF_DIR: the card's applications (CSIM and USIM).
    {0x2F00, Structure::LinearFixed, "621A8205422100300483022F008A01058B032F0601800200C08801F0", {}, 4},
    // EF_ICCID: content from the real card (SimCard::m_iccidBcd).
    {0x2FE2, Structure::Transparent, "62178202412183022FE28A01058B032F06038002000A880110", {}, 0},
    // EF_PL: no preferred languages.
    {0x2F05, Structure::Transparent, "62178202412183022F058A01058B032F060280020004880128", "FFFFFFFF", 0},
    // EF_MSISDN: no number stored (Android lets the person enter theirs).
    {0x6F40, Structure::LinearFixed, "621982054221001C0283026F408A01058B036F0605800200388800", kEmpty28, 1},
    // EF_MBI, EF_MBDN: no voice mail.
    {0x6FC9, Structure::LinearFixed, "62198205422100050183026FC98A01058B036F0602800200058800", "0000000000", 1},
    {0x6FC7, Structure::LinearFixed, "621982054221001C0283026F408A01058B036F06058002003E8800", kEmpty28, 1},
    // EF_AD: content from the real operator (the length of its MNC, SimCard::m_mncLength).
    {0x6FAD, Structure::Transparent, "62178202412183026FAD8A01058B036F060180020004880118", {}, 0},
    // EF_MWIS: no message waiting.
    {0x6FCA, Structure::LinearFixed, "62198205422100050183026FCA8A01058B036F060E800200058800", "0000000000", 1},
    // EF_FPLMN: no forbidden networks.
    {0x6F7B, Structure::Transparent, "621C8202412183026F7BA5038001718A01058B036F06038002001E880168",
     "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF", 0},
    // EF_FDN: no fixed dialing numbers.
    {0x6F3B, Structure::LinearFixed, "621982054221001C0A83026F3B8A01058B036F0605800201188800", kEmpty28, 10},
};

constexpr std::string_view kNotFound = "+CRSM: 106,130";  // 6A82: file not found
constexpr std::string_view kNoRecord = "+CRSM: 106,131";  // 6A83: record not found

// EF_DIR records: the CSIM and USIM application templates, then unused ones.
constexpr std::string_view kDirRecords[] = {
    "61184F10A0000003431002FF86FF0389FFFFFFFF50044353494DFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",
    "61184F10A0000000871002FF86FF0389FFFFFFFF50045553494DFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",
};
constexpr std::string_view kDirUnused =
    "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF";

}  // namespace

SimCard::SimCard(std::string iccid, unsigned mncLength) : m_mncLength(mncLength == 3 ? 3 : 2) {
    // TS 102 221: ICCID digits in BCD, each byte's nibbles swapped; padded with F.
    if (iccid.size() % 2) iccid += 'F';
    for (std::size_t i = 0; i + 1 < iccid.size(); i += 2) {
        m_iccidBcd += iccid[i + 1];
        m_iccidBcd += iccid[i];
    }
}

std::string SimCard::Command(int command, int fileId, int p1, int /*p2*/, int /*p3*/) const {
    const File* file = nullptr;
    for (const File& f : kFiles)
        if (f.id == fileId) file = &f;
    if (!file) return std::string(kNotFound);

    switch (command) {
    case kGetResponse: return std::format("+CRSM: 144,0,{}", file->fcp);
    case kReadBinary:
        if (file->structure != Structure::Transparent) return std::string(kNotFound);
        if (fileId == 0x2FE2) return std::format("+CRSM: 144,0,{}", m_iccidBcd);
        if (fileId == 0x6FAD) return std::format("+CRSM: 144,0,000000{:02X}", m_mncLength);
        return std::format("+CRSM: 144,0,{}", file->content);
    case kReadRecord:
        if (file->structure != Structure::LinearFixed) return std::string(kNotFound);
        if (p1 < 1 || p1 > file->records) return std::string(kNoRecord);
        if (fileId == 0x2F00)
            return std::format("+CRSM: 144,0,{}", p1 <= static_cast<int>(std::size(kDirRecords)) ? kDirRecords[p1 - 1] : kDirUnused);
        return std::format("+CRSM: 144,0,{}", file->content);
    case kUpdateBinary:
    case kUpdateRecord:
        return "+CRSM: 144,0";  // accepted and forgotten: nothing is written to the real card
    default: return std::string(kNotFound);
    }
}

}  // namespace WN::Tel
