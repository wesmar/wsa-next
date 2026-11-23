// WSA-Next © 2026 Marek Wesołowski - wesmar
// The PC's modem through the Windows mobile broadband API (mbnapi.h, plain COM): the card, the
// network, the signal and SMS in PDU form. One modem: the first mobile broadband interface.
#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace WN::Tel {

struct ModemState {
    bool simReady = false;
    std::string imei, iccid, imsi;
    // 27.007 <stat>: 0 not registered, 1 home, 2 searching, 3 denied, 5 roaming.
    int registration = 0;
    std::string operatorId;    // MCC + MNC digits, e.g. "26006"
    std::string operatorName;  // e.g. "Play"
    int accessTechnology = 7;  // 27.007 <AcT>: 0 GSM, 2 UTRAN, 7 E-UTRAN, 13 NR (5G SA)
    int rssi = 99;             // 27.007 <rssi>: 0..31, 99 unknown
    std::string smsc;          // the service centre's number ("+" and digits); empty when unknown
};

enum class StoredStatus { Unread, Read, Draft, Sent };

// A message in the modem's store.
struct StoredMessage {
    unsigned index = 0;
    StoredStatus status = StoredStatus::Read;
    std::string pdu;  // hex, with the service centre address
};

class Modem {
public:
    // The first mobile broadband interface of this PC; null without one.
    static std::unique_ptr<Modem> Open();
    ~Modem();
    Modem(const Modem&) = delete;
    Modem& operator=(const Modem&) = delete;

    [[nodiscard]] ModemState State() const;

    // Sends an SMS-SUBMIT TPDU (hex, without the service centre part; `length` in octets) and
    // waits for the modem's answer. True when the network took the message.
    bool Send(const std::string& tpduHex, unsigned length);

    struct Events {
        // An incoming SMS-DELIVER PDU (hex, with the service centre address) and its index in
        // the modem's message store.
        std::function<void(const std::string& pduHex, unsigned index)> message;
        // The network or the signal changed.
        std::function<void()> changed;
    };
    // Subscribes to the modem's notifications. Events come on COM threads until Stop.
    void Start(Events events);
    void Stop();
    // Reads the received messages waiting in the modem's store, read or not (they come as
    // `message` events, each once per session), and the MMS notices kept from the SMS router.
    void ReadReceived();
    // MMS notices (WAP push) from Windows' SMS router, which takes them out of the modem's store
    // as they arrive: kept for Android as SMS-DELIVER PDUs (`message` events, indexes the store
    // never uses) until it acknowledges them; kept in memory only. For the bridge, once.
    void ReceiveRoutedNotices();
    // Removes a message from the modem's store once Android has it.
    void Delete(unsigned index);
    // Android refused a message (AT+CNMA=2): it stays in the store and the next read gives it again.
    void Release(unsigned index);
    // The store's messages (all, or the unread ones), without giving them to Android. Needs
    // Start; waits up to `timeoutMs` for the modem. Empty optional when the store gave no answer.
    std::optional<std::vector<StoredMessage>> ReadStore(bool unreadOnly, unsigned timeoutMs);

private:
    struct Impl;
    explicit Modem(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m;
};

}  // namespace WN::Tel
