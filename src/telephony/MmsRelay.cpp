// WSA-Next © 2026 Marek Wesołowski - wesmar
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <windns.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include "telephony/MmsRelay.h"
#include "bluetooth/Log.h"
#include "host/Guest.h"
#include "support/Common.h"
#include "support/Handle.h"
#include "telephony/Protocol.h"
#include <charconv>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#pragma comment(lib, "dnsapi.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "ws2_32.lib")

namespace WN::Tel {
namespace {
using namespace winrt::Windows::Networking::Connectivity;

constexpr std::size_t kMaxRequest = 2048;
// A guard: Windows gives the APN's new interface its address within seconds.
constexpr DWORD kAddressWaitMs = 15'000;

// "MMS\t<apn>\t<auth type>\t<user>\t<password>\t<host>\t<port>" (WsaMmsRelay.java). The auth type
// is Android's (ApnSetting.AUTH_TYPE_*: -1 unset, 0 none, 1 PAP, 2 CHAP, 3 PAP or CHAP).
struct Request {
    std::string apn;
    int authType = -1;
    std::string user, password, host;
    unsigned short port = 0;
};

std::optional<Request> Parse(std::string_view line) {
    std::vector<std::string_view> fields;
    for (std::size_t start = 0;;) {
        const std::size_t tab = line.find('\t', start);
        fields.push_back(line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start));
        if (tab == std::string_view::npos) break;
        start = tab + 1;
    }
    if (fields.size() != 7 || fields[0] != "MMS" || fields[1].empty() || fields[5].empty()) return std::nullopt;
    Request r;
    r.apn = fields[1];
    (void)std::from_chars(fields[2].data(), fields[2].data() + fields[2].size(), r.authType);  // unset stays -1
    r.user = fields[3];
    r.password = fields[4];
    r.host = fields[5];
    const std::from_chars_result port = std::from_chars(fields[6].data(), fields[6].data() + fields[6].size(), r.port);
    if (port.ec != std::errc{} || r.port == 0) return std::nullopt;
    return r;
}

bool SendAll(SOCKET socket, const char* data, std::size_t size) {
    for (std::size_t done = 0; done < size;) {
        const int n = ::send(socket, data + done, static_cast<int>(size - done), 0);
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

bool Reply(SOCKET android, std::string_view line) {
    const std::string text = std::string(line) + "\n";
    return SendAll(android, text.data(), text.size());
}

// The request line. The connection stays idle until Android has an MMS to move; one byte at a
// time, so nothing after the line is taken.
std::optional<std::string> ReadLine(SOCKET android) {
    std::string line;
    while (line.size() < kMaxRequest) {
        char c = 0;
        if (recv(android, &c, 1, 0) <= 0) return std::nullopt;
        if (c == '\n') return line;
        line += c;
    }
    return std::nullopt;
}

CellularApnAuthenticationType AuthType(const Request& r) {
    switch (r.authType) {
    case 0: return CellularApnAuthenticationType::None;
    case 1: return CellularApnAuthenticationType::Pap;
    case 2:
    case 3: return CellularApnAuthenticationType::Chap;
    default:  // unset: Android tries PAP or CHAP when the APN has a user name
        return r.user.empty() ? CellularApnAuthenticationType::None : CellularApnAuthenticationType::Pap;
    }
}

std::size_t AddressSize(const SOCKADDR_INET& address) {
    return address.si_family == AF_INET ? sizeof(SOCKADDR_IN) : sizeof(SOCKADDR_IN6);
}

// The interface's usable address of `family` (IPv6: not link-local).
std::optional<SOCKADDR_INET> InterfaceAddress(const NET_LUID& luid, ADDRESS_FAMILY family) {
    PMIB_UNICASTIPADDRESS_TABLE table = nullptr;
    if (GetUnicastIpAddressTable(family, &table) != NO_ERROR) return std::nullopt;
    std::optional<SOCKADDR_INET> found;
    for (ULONG i = 0; i < table->NumEntries && !found; ++i) {
        const MIB_UNICASTIPADDRESS_ROW& row = table->Table[i];
        if (row.InterfaceLuid.Value != luid.Value || row.DadState != IpDadStatePreferred) continue;
        if (family == AF_INET6 && IN6_IS_ADDR_LINKLOCAL(&row.Address.Ipv6.sin6_addr)) continue;
        found = row.Address;
    }
    FreeMibTable(table);
    return found;
}

void CALLBACK OnAddressChange(PVOID context, PMIB_UNICASTIPADDRESS_ROW, MIB_NOTIFICATION_TYPE) {
    SetEvent(static_cast<HANDLE>(context));
}

// The new interface's address (IPv4 first); Windows may assign it just after the connection.
std::optional<SOCKADDR_INET> WaitForAddress(const NET_LUID& luid) {
    const KernelHandle changed(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    HANDLE notification = nullptr;
    if (NotifyUnicastIpAddressChange(AF_UNSPEC, &OnAddressChange, changed.get(), FALSE, &notification) != NO_ERROR) notification = nullptr;
    const ULONGLONG deadline = GetTickCount64() + kAddressWaitMs;
    std::optional<SOCKADDR_INET> address;
    for (;;) {
        address = InterfaceAddress(luid, AF_INET);
        if (!address) address = InterfaceAddress(luid, AF_INET6);
        const ULONGLONG now = GetTickCount64();
        if (address || !notification || now >= deadline) break;
        WaitForSingleObject(changed.get(), static_cast<DWORD>(deadline - now));
    }
    if (notification) CancelMibChangeNotify2(notification);
    return address;
}

// The APN opened on the modem: a connection of its own, with its own interface.
struct ApnLink {
    ConnectionSession session{nullptr};
    NET_IFINDEX index = 0;
    SOCKADDR_INET address{};

    ApnLink() = default;
    ApnLink(const ApnLink&) = delete;
    ApnLink& operator=(const ApnLink&) = delete;
    ApnLink(ApnLink&&) = default;
    ApnLink& operator=(ApnLink&&) = default;
    ~ApnLink() {
        try {
            if (session) session.Close();  // Windows ends the packet context
        } catch (const winrt::hresult_error&) {
        }
    }
};

std::optional<ApnLink> OpenApn(const Request& r) {
    try {
        CellularApnContext context;
        context.AccessPointName(winrt::to_hstring(r.apn));
        context.UserName(winrt::to_hstring(r.user));
        context.Password(winrt::to_hstring(r.password));
        context.AuthenticationType(AuthType(r));
        ApnLink link;
        link.session = ConnectivityManager::AcquireConnectionAsync(context).get();
        ConnectionProfile profile{nullptr};
        if (link.session) profile = link.session.ConnectionProfile();
        NetworkAdapter adapter{nullptr};
        if (profile) adapter = profile.NetworkAdapter();
        if (!adapter) {
            Bt::Log("mms: Windows opened APN {} but gave no interface", r.apn);
            return std::nullopt;
        }
        const winrt::guid adapterId = adapter.NetworkAdapterId();
        GUID id{};
        std::memcpy(&id, &adapterId, sizeof id);
        NET_LUID luid{};
        if (ConvertInterfaceGuidToLuid(&id, &luid) != NO_ERROR || ConvertInterfaceLuidToIndex(&luid, &link.index) != NO_ERROR) {
            Bt::Log("mms: the interface of APN {} is not known to the network stack", r.apn);
            return std::nullopt;
        }
        const auto address = WaitForAddress(luid);
        if (!address) {
            Bt::Log("mms: APN {} got no address within {} s", r.apn, kAddressWaitMs / 1000);
            return std::nullopt;
        }
        link.address = *address;
        return link;
    } catch (const winrt::hresult_error& e) {
        Bt::Log("mms: Windows did not open APN {} (0x{:08X})", r.apn, static_cast<unsigned>(e.code()));
        return std::nullopt;
    }
}

// The server's addresses in the APN's address family: a literal, or the name looked up through
// the APN's own name servers (an operator's MMS server may have a name only its network knows).
std::vector<SOCKADDR_INET> Resolve(const Request& r, const ApnLink& link) {
    const ADDRESS_FAMILY family = link.address.si_family;
    SOCKADDR_INET target{};
    target.si_family = family;
    if (family == AF_INET) target.Ipv4.sin_port = htons(r.port);
    else target.Ipv6.sin6_port = htons(r.port);
    void* literal = family == AF_INET ? static_cast<void*>(&target.Ipv4.sin_addr) : static_cast<void*>(&target.Ipv6.sin6_addr);
    if (InetPtonA(family, r.host.c_str(), literal) == 1) return {target};

    std::vector<SOCKADDR_INET> found;
    const std::wstring name = FromUtf8(r.host);
    DNS_QUERY_REQUEST request{};
    request.Version = DNS_QUERY_REQUEST_VERSION1;
    request.QueryName = name.c_str();
    request.QueryType = family == AF_INET ? DNS_TYPE_A : DNS_TYPE_AAAA;
    request.QueryOptions = DNS_QUERY_STANDARD;
    request.InterfaceIndex = link.index;
    DNS_QUERY_RESULT result{};
    result.Version = DNS_QUERY_RESULTS_VERSION1;
    if (DnsQueryEx(&request, &result, nullptr) == ERROR_SUCCESS) {
        for (PDNS_RECORD record = result.pQueryRecords; record; record = record->pNext) {
            if (family == AF_INET && record->wType == DNS_TYPE_A) {
                target.Ipv4.sin_addr.S_un.S_addr = record->Data.A.IpAddress;
                found.push_back(target);
            } else if (family == AF_INET6 && record->wType == DNS_TYPE_AAAA) {
                std::memcpy(&target.Ipv6.sin6_addr, &record->Data.AAAA.Ip6Address, sizeof target.Ipv6.sin6_addr);
                found.push_back(target);
            }
        }
        if (result.pQueryRecords) DnsRecordListFree(result.pQueryRecords, DnsFreeRecordList);
    }
    if (found.empty()) Bt::Log("mms: the name {} is not known through APN {}", r.host, r.apn);
    return found;
}

// A connection out of the APN's interface only: from its address, with the interface pinned.
SOCKET ConnectThrough(const ApnLink& link, const std::vector<SOCKADDR_INET>& targets) {
    for (const SOCKADDR_INET& target : targets) {
        const SOCKET s = socket(target.si_family, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) continue;
        if (target.si_family == AF_INET) {
            const DWORD index = htonl(link.index);  // IPv4 wants it in network order
            setsockopt(s, IPPROTO_IP, IP_UNICAST_IF, reinterpret_cast<const char*>(&index), sizeof index);
        } else {
            const DWORD index = link.index;
            setsockopt(s, IPPROTO_IPV6, IPV6_UNICAST_IF, reinterpret_cast<const char*>(&index), sizeof index);
        }
        if (bind(s, reinterpret_cast<const sockaddr*>(&link.address), static_cast<int>(AddressSize(link.address))) == 0 &&
            connect(s, reinterpret_cast<const sockaddr*>(&target), static_cast<int>(AddressSize(target))) == 0)
            return s;
        closesocket(s);
    }
    return INVALID_SOCKET;
}

// Bytes both ways. The server's end of data goes to Android as a half-close; Android's end ends
// the exchange (MmsService closes the tunnel once it has the answer), which also ends the
// server's direction.
void Pipe(SOCKET android, SOCKET server, std::uint64_t& out, std::uint64_t& in) {
    std::jthread fromServer([&] {
        char buffer[16 * 1024];
        for (;;) {
            const int n = recv(server, buffer, static_cast<int>(sizeof buffer), 0);
            if (n <= 0 || !SendAll(android, buffer, static_cast<std::size_t>(n))) break;
            in += static_cast<std::uint64_t>(n);
        }
        shutdown(android, SD_SEND);
    });
    char buffer[16 * 1024];
    for (;;) {
        const int n = recv(android, buffer, static_cast<int>(sizeof buffer), 0);
        if (n <= 0 || !SendAll(server, buffer, static_cast<std::size_t>(n))) break;
        out += static_cast<std::uint64_t>(n);
    }
    shutdown(server, SD_BOTH);
    CancelIoEx(reinterpret_cast<HANDLE>(server), nullptr);  // a receive still waiting returns
}

void RunSession(std::uintptr_t handle) {
    const auto android = static_cast<SOCKET>(handle);
    const auto line = ReadLine(android);
    if (!line) return;  // WSA ends, or the daemon restarted
    const auto request = Parse(*line);
    if (!request) {
        Bt::Log("mms: Android sent a request the host does not understand");
        Reply(android, "ERR\tbad request");
        return;
    }
    const auto link = OpenApn(*request);
    if (!link) {
        Reply(android, "ERR\tAPN " + request->apn + " not opened");
        return;
    }
    const SOCKET server = ConnectThrough(*link, Resolve(*request, *link));
    if (server == INVALID_SOCKET) {
        Bt::Log("mms: {}:{} did not answer through APN {}", request->host, request->port, request->apn);
        Reply(android, "ERR\tserver unreachable");
        return;
    }
    Bt::Log("mms: Android's MMS exchange with {}:{} through APN {}", request->host, request->port, request->apn);
    std::uint64_t out = 0, in = 0;
    if (Reply(android, "OK")) Pipe(android, server, out, in);
    closesocket(server);
    Bt::Log("mms: exchange done ({} bytes out, {} bytes in)", out, in);
}

}  // namespace

void RunMmsRelay(std::stop_token stop) {
    Guest::Serve(kMmsPort, "Android MMS relay", [](std::uintptr_t socket, std::stop_token) { RunSession(socket); }, stop);
}

}  // namespace WN::Tel
