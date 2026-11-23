// WSA-Next © 2026 Marek Wesołowski - wesmar
#include "app/Cli.h"
#include "app/Console.h"
#include "biometrics/HelloBridge.h"
#include "biometrics/WindowsHello.h"
#include "bluetooth/Bridge.h"
#include "host/Host.h"
#include "support/Signature.h"
#include "host/Guest.h"
#include "bluetooth/Log.h"
#include "core/Install.h"
#include "core/Manifest.h"
#include "core/Release.h"
#include "core/System.h"
#include "core/Uninstall.h"
#include "install/Package.h"
#include "install/Setup.h"
#include "install/Traces.h"
#include "support/Config.h"
#include "support/Settings.h"
#include "support/Text.h"
#include "telephony/AtModem.h"
#include "telephony/Modem.h"
#include <Windows.h>
#include <objbase.h>
#include <algorithm>
#include <charconv>
#include <cwctype>
#include <format>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace WN::Cli {
namespace {
namespace fs = std::filesystem;

std::wstring Lower(std::wstring_view s) {
    std::wstring out(s);
    for (auto& c : out) c = static_cast<wchar_t>(std::towlower(c));
    return out;
}

bool IsHelp(std::wstring_view arg) {
    static constexpr std::wstring_view kHelp[] = {L"help", L"--help", L"-help", L"-h", L"-?", L"/?", L"/h", L"/help"};
    return std::ranges::contains(kHelp, Lower(arg));
}

struct Args {
    std::wstring command;
    std::vector<std::wstring> positional;
    std::wstring source, dir;
    bool yes = false, filesOnly = false, keepDownloads = false, noProgress = false, all = false, listVms = false,
         helloCheck = false, helloTest = false, fromSetup = false;
};

std::optional<std::wstring> Parse(std::span<wchar_t*> argv, Args& args) {
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const std::wstring word(argv[i]);
        const std::wstring name = Lower(word);
        auto value = [&](std::wstring& out) -> std::optional<std::wstring> {
            if (i + 1 >= argv.size()) return std::format(L"{} needs a value", word);
            out = argv[++i];
            return std::nullopt;
        };
        if (name == L"--source") { if (auto e = value(args.source)) return e; continue; }
        if (name == L"--dir") { if (auto e = value(args.dir)) return e; continue; }
        if (name == L"--yes" || name == L"-y") { args.yes = true; continue; }
        if (name == L"--files-only") { args.filesOnly = true; continue; }
        if (name == L"--keep-downloads") { args.keepDownloads = true; continue; }
        if (name == L"--no-progress") { args.noProgress = true; continue; }
        if (name == L"--all") { args.all = true; continue; }
        if (name == L"--from-setup") { args.fromSetup = true; continue; }
        if (name == L"--list-vms") { args.listVms = true; continue; }
        if (name == L"--check") { args.helloCheck = true; continue; }
        if (name == L"--test") { args.helloTest = true; continue; }
        if (name == L"--version") { args.command = L"version"; continue; }
        if (name.starts_with(L"-")) return std::format(L"unknown option {}", word);
        if (args.command.empty()) args.command = name;
        else args.positional.push_back(word);
    }
    return std::nullopt;
}

std::wstring YesNo(std::optional<bool> v) { return !v ? L"unknown" : *v ? L"yes" : L"no"; }

fs::path FolderOf(const Args& args) {
    if (!args.dir.empty()) return fs::absolute(args.dir);
    const std::wstring saved = Settings::Read(L"InstallFolder");
    return saved.empty() ? Core::DefaultFolder() : fs::path(saved);
}

std::wstring SourceOf(const Args& args) {
    if (!args.source.empty()) return SourceUrl(args.source);
    const std::wstring saved = Settings::Read(L"Source", L"github");  // the GUI's choice
    return saved == L"custom" ? Settings::Read(L"CustomManifest") : SourceUrl(saved);
}

std::wstring HelloRememberText(std::uint32_t minutes) {
    if (minutes == 0) return L"off (Windows Hello asks every time)";
    if (minutes == Host::kHelloUntilLocked) return L"until Windows is locked";
    return std::format(L"for {} minutes", minutes);
}

int Status(Console& con, const Args& args) {
    const Core::SystemInfo s = Core::Inspect();
    const Signature::Origin origin = Signature::Check();
    con.Line(std::format(L"WSA-Next build            {}", origin == Signature::Origin::Official      ? L"official (publisher signature)"
                                                          : origin == Signature::Origin::Unsigned    ? L"unofficial (not signed)"
                                                          : origin == Signature::Origin::OtherSigner ? L"unofficial (signed by someone else)"
                                                                                                     : L"modified after signing"));
    con.Line(std::format(L"Windows                   {}", s.Version()));
    con.Line(std::format(L"Supported                 {}", s.Supported() ? L"yes" : L"no (Windows 11 required)"));
    con.Line(std::format(L"Virtual Machine Platform  {}", YesNo(s.virtualMachinePlatform)));
    con.Line(std::format(L"Developer mode            {}", s.developerMode ? L"on" : L"off"));
    if (s.NeedsPatch()) con.Line(L"WSAPatch                  needed on this build");
    if (s.package)
        con.Line(std::format(L"WSA package               {} ({}{})", s.package->version, s.package->location.wstring(),
                             s.package->developmentMode ? L"" : L", Microsoft Store"));
    else
        con.Line(L"WSA package               not registered");
    const fs::path folder = FolderOf(args);
    const Core::InstalledState state = Core::ReadState(folder);
    con.Line(std::format(L"Install folder            {}", folder.wstring()));
    con.Line(std::format(L"Installed release         {}", state.version.empty() ? std::wstring(L"none") : state.version));
    using Core::Release::State;
    const State verdict = Core::Release::HostVerdict();
    con.Line(std::format(L"Release check (host)      {}", verdict == State::Official   ? L"official release, unchanged"
                                                          : verdict == State::Checking ? L"checking the Android disks"
                                                          : verdict == State::Unsigned ? L"not an official release"
                                                          : verdict == State::Modified ? L"official release changed after installing"
                                                          : verdict == State::Missing  ? L"WSA not installed"
                                                                                       : L"not checked yet"));
    con.Line(std::format(L"Windows Hello             {}", Bio::StatusText(Bio::CheckAvailability())));
    con.Line(std::format(L"Remember Windows Hello    {}", HelloRememberText(Host::HelloRememberMinutes())));
    con.Line(std::format(L"Root for Android apps     {}", Host::ElevationEnabled() ? L"on (confirmed with Windows Hello)" : L"off"));
    return kOk;
}

int Check(Console& con, const Args& args) {
    const std::wstring url = SourceOf(args);
    auto manifest = Core::LoadManifest(url, {});
    if (!manifest) {
        con.Status(Level::Error, manifest.error());
        return kUnavailable;
    }
    const fs::path folder = FolderOf(args);
    const Core::Plan plan = Core::MakePlan(*manifest, Core::ReadState(folder), Core::Inspect(), folder);
    con.Line(std::format(L"{} {}  {}  {}", manifest->name, manifest->version, manifest->android, manifest->variant));
    con.Line(std::format(L"Source  {}", url));
    con.Line(std::format(L"Folder  {}", folder.wstring()));
    con.Line(std::format(L"Release {}", manifest->official      ? L"official (publisher signature)"
                                        : manifest->catalog.empty() ? L"unofficial (not signed)"
                                                                    : L"unofficial (signature not valid for this manifest)"));
    con.Line();
    for (const Core::PlanItem& item : plan.items) {
        const Core::Component& c = *item.component;
        con.Line(std::format(L"  {:<11} {:<22} {:>10} -> {:>10}  {}", c.id, c.target, FormatBytes(c.packedSize), FormatBytes(c.size),
                             item.needed ? L"download" : L"installed"));
    }
    con.Line();
    if (plan.UpToDate()) con.Status(Level::Success, L"up to date");
    else con.Line(std::format(L"To download: {}, unpacked: {}", FormatBytes(plan.downloadBytes), FormatBytes(plan.diskBytes)));
    return kOk;
}

int Install(Console& con, const Args& args) {
    if (!IsElevated()) {
        con.Status(Level::Error, L"run this from an elevated (administrator) prompt");
        return kUnavailable;
    }
    const std::wstring url = SourceOf(args);
    con.Status(Level::Info, std::format(L"manifest {}", url));
    auto manifest = Core::LoadManifest(url, {});
    if (!manifest) {
        con.Status(Level::Error, manifest.error());
        return kUnavailable;
    }
    con.set_progress_enabled(!args.noProgress);
    Core::Options options;
    options.folder = FolderOf(args);
    options.registerPackage = !args.filesOnly;
    options.keepDownloads = args.keepDownloads;

    Core::Callbacks cb;
    cb.progress = [&](const Core::Progress& p) {
        if (p.permille < 0 || !p.total) return;
        std::wstring detail = std::format(L"{} / {}", FormatBytes(p.done), FormatBytes(p.total));
        if (p.speed) detail += std::format(L"  {}/s", FormatBytes(p.speed));
        con.Progress(p.phase, static_cast<double>(p.done) / static_cast<double>(p.total), detail);
    };
    cb.log = [&](Level level, std::wstring text) { con.Status(level, text); };
    cb.confirm = [&](const std::wstring& question) {
        con.Line(question);
        if (args.yes) {
            con.Line(L"  --yes: accepted");
            return true;
        }
        con.Line(L"  not accepted (add --yes to accept)");
        return false;
    };
    auto outcome = Core::Run(*manifest, options, cb, {});
    con.EndProgress();
    if (!outcome) {
        con.Status(Level::Error, outcome.error());
        return outcome.error() == Text::English(IDS_CANCELED) ? kCanceled : kFailed;
    }
    con.Status(Level::Success, std::format(L"{} {} installed in {}", manifest->name, manifest->version, options.folder.wstring()));
    if (outcome->restartNeeded) {
        con.Status(Level::Warning, L"restart Windows to finish enabling Virtual Machine Platform");
        return kRestart;
    }
    return kOk;
}

int Uninstall(Console& con, const Args& args) {
    if (!IsElevated()) {
        con.Status(Level::Error, L"run this from an elevated (administrator) prompt");
        return kUnavailable;
    }
    const fs::path folder = FolderOf(args);
    const fs::path data = Install::PackageDataFolder();
    if (!args.yes) {
        if (args.all)
            con.Line(std::format(L"uninstall --all removes WSA-Next ({}) and everything WSA left in Windows, including the Android apps, "
                                 L"their data and the Google account sign-in ({} in {}).",
                                 folder.wstring(), FormatBytes(Install::FolderSize(data)), data.wstring()));
        else
            con.Line(std::format(L"uninstall removes WSA-Next ({}); the Android apps and their data stay in {} for the next installation.",
                                 folder.wstring(), data.wstring()));
        con.Line(L"  not done (add --yes to confirm)");
        return kCanceled;
    }
    // Installed with WSA-Next Setup: Windows Installer removes WSA-Next, and calls back here
    // (--from-setup) before it removes its own files.
    if (!args.fromSetup) {
        if (const auto product = Install::SetupProduct()) {
            if (auto r = Install::RemoveThroughSetup(*product, args.all); !r) {
                con.Status(Level::Error, r.error());
                return kFailed;
            }
            con.Status(Level::Info, std::format(L"WSA-Next Setup ({}) removes WSA-Next now", *product));
            return kOk;
        }
    }
    Core::Callbacks cb;
    cb.progress = [](const Core::Progress&) {};
    cb.log = [&](Level level, std::wstring text) { con.Status(level, text); };
    cb.confirm = [](const std::wstring&) { return true; };
    auto outcome = Core::Uninstall(folder, args.all ? Core::Removal::Everything : Core::Removal::KeepData, cb);
    if (!outcome) {
        con.Status(Level::Error, outcome.error());
        return kFailed;
    }
    if (!outcome->leftovers.empty()) {
        con.Status(Level::Warning, std::format(L"{} item(s) could not be removed (see above)", outcome->leftovers.size()));
        return kFailed;
    }
    con.Status(Level::Success, Text::Get(args.all ? IDS_STATUS_REMOVED_ALL : IDS_STATUS_REMOVED));
    if (outcome->restartNeeded) {
        con.Status(Level::Warning, L"restart Windows to finish disabling Virtual Machine Platform");
        return kRestart;
    }
    return kOk;
}

std::stop_source g_bluetoothStop;

BOOL WINAPI StopBluetooth(DWORD) {
    g_bluetoothStop.request_stop();
    return TRUE;
}

// The Bluetooth bridge in the foreground: the PC's adapter for Android until Ctrl+C.
int Bluetooth(Console& con, const Args& args) {
    if (!IsElevated()) {
        con.Status(Level::Error, L"run this from an elevated (administrator) prompt");
        return kUnavailable;
    }
    if (args.listVms) {
        con.Line(Guest::ComputeSystemsJson());
        return kOk;
    }
    if (!Signature::OfficialFeatures()) {
        con.Status(Level::Error, L"Bluetooth for Android is only in the official (signed) WSA-Next");
        return kUnavailable;
    }
    if (Host::Running() && Host::BluetoothEnabled()) {
        con.Status(Level::Warning, L"the WSA-Next background host already serves Bluetooth (turn it off in the window's Bluetooth menu first)");
        return kUnavailable;
    }
    Bt::SetLogSink([&con](std::string_view line) { con.Line(FromUtf8(line)); });
    SetConsoleCtrlHandler(StopBluetooth, TRUE);
    const Bt::BridgeResult result = Bt::RunBridge(g_bluetoothStop.get_token());
    SetConsoleCtrlHandler(StopBluetooth, FALSE);
    Bt::SetLogSink(nullptr);
    return result == Bt::BridgeResult::Stopped ? kOk : kUnavailable;
}

std::stop_source g_helloStop;

BOOL WINAPI StopHello(DWORD) {
    g_helloStop.request_stop();
    return TRUE;
}

// hello remember [off|5|30|lock]: how long a Windows Hello verification lets Android apps in.
int HelloRemember(Console& con, const Args& args) {
    if (args.positional.empty()) {
        con.Line(std::format(L"Remember Windows Hello  {}", HelloRememberText(Host::HelloRememberMinutes())));
        return kOk;
    }
    const std::wstring value = Lower(args.positional[0]);
    const std::uint32_t minutes = value == L"off" ? 0u : value == L"5" ? 5u : value == L"30" ? 30u : value == L"lock" ? Host::kHelloUntilLocked : 1u;
    if (minutes == 1u) {
        con.Err(L"hello remember: off, 5, 30 or lock\n");
        return kUsage;
    }
    if (minutes != 0 && !args.yes) {
        con.Line(L"After one Windows Hello confirmation, Android apps that ask for a fingerprint (a bank or a broker,\n"
                 L"for example) get in without asking again; so does anyone using this PC meanwhile. Locking Windows,\n"
                 L"sleep with a sign-in and signing out end it; root for Android apps still asks every time.");
        con.Status(Level::Warning, L"add --yes to accept this at your own responsibility");
        return kCanceled;
    }
    Host::SetHelloRememberMinutes(minutes);
    con.Status(Level::Success, std::format(L"Remember Windows Hello: {}", HelloRememberText(minutes)));
    return kOk;
}

// Windows Hello bridge (or availability/prompt self-test) for Android biometrics.
int Hello(Console& con, const Args& args) {
    if (!args.positional.empty() && Lower(args.positional[0]) == L"remember") {
        Args rest = args;
        rest.positional.erase(rest.positional.begin());
        return HelloRemember(con, rest);
    }
    if (args.helloCheck) {
        const Bio::HelloStatus status = Bio::CheckAvailability();
        con.Line(std::format(L"Windows Hello  {}", Bio::StatusText(status)));
        return status == Bio::HelloStatus::Available ? kOk : kUnavailable;
    }
    if (args.helloTest) {
        const Bio::HelloStatus status = Bio::CheckAvailability();
        con.Line(std::format(L"Windows Hello  {}", Bio::StatusText(status)));
        if (status != Bio::HelloStatus::Available) return kUnavailable;
        const Bio::VerifyResult r = Bio::RequestVerification(L"WSA-Next Windows Hello test");
        const wchar_t* text = r == Bio::VerifyResult::Verified ? L"verified"
                              : r == Bio::VerifyResult::Canceled ? L"canceled"
                              : r == Bio::VerifyResult::Busy ? L"busy"
                              : r == Bio::VerifyResult::NotAvailable ? L"not available"
                                                                     : L"failed";
        con.Status(r == Bio::VerifyResult::Verified ? Level::Success : Level::Warning, std::format(L"Result: {}", text));
        return r == Bio::VerifyResult::Verified ? kOk : r == Bio::VerifyResult::Canceled ? kCanceled : kFailed;
    }
    if (!IsElevated()) {
        con.Status(Level::Error, L"run this from an elevated (administrator) prompt");
        return kUnavailable;
    }
    if (!Signature::OfficialFeatures()) {
        con.Status(Level::Error, L"Windows Hello for Android is only in the official (signed) WSA-Next");
        return kUnavailable;
    }
    if (Host::Running()) {
        con.Status(Level::Warning, L"the WSA-Next background host already serves Windows Hello (see the log in %LOCALAPPDATA%\\WSA-Next)");
        return kUnavailable;
    }
    Bt::SetLogSink([&con](std::string_view line) { con.Line(FromUtf8(line)); });
    SetConsoleCtrlHandler(StopHello, TRUE);
    const Bio::BridgeResult result = Bio::RunBridge(g_helloStop.get_token());
    SetConsoleCtrlHandler(StopHello, FALSE);
    Bt::SetLogSink(nullptr);
    return result == Bio::BridgeResult::Stopped ? kOk : kUnavailable;
}

// The PC's modem as Android's RIL sees it: the answers to its first questions (diagnostics; the
// card's and the device's numbers are not shown).
// What the store's SMS-DELIVER PDU (hex, with the service centre part) is: the service centre's
// time stamp ("YYYY-MM-DD hh:mm:ss") and its kind from the user data header - "text", "MMS
// notice" (WAP push to port 2948), "port N", with "part i/n" for a piece of a longer one. Empty
// for other PDUs. Neither the sender nor the text is read.
struct DeliverInfo {
    std::wstring time, kind;
};

DeliverInfo Describe(const std::string& hex) {
    std::vector<unsigned> octets;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        unsigned value = 0;
        std::from_chars(hex.data() + i, hex.data() + i + 2, value, 16);
        octets.push_back(value);
    }
    auto at = [&](std::size_t i) { return i < octets.size() ? octets[i] : 0u; };
    std::size_t pos = 1 + at(0);  // past the service centre
    const unsigned first = at(pos);
    if ((first & 0x03) != 0) return {};  // not an SMS-DELIVER
    pos += 1;
    pos += 2 + (at(pos) + 1) / 2;  // the sender: its length in digits, its type, its digits
    pos += 2;                      // protocol identifier, data coding
    if (pos + 8 > octets.size()) return {};
    auto field = [&](std::size_t i) { const unsigned o = at(pos + i); return (o & 0x0F) * 10 + (o >> 4); };  // swapped semi-octets
    DeliverInfo info;
    info.time = std::format(L"20{:02}-{:02}-{:02} {:02}:{:02}:{:02}", field(0), field(1), field(2), field(3), field(4), field(5));
    pos += 7 + 1;  // the time stamp, the user data length
    info.kind = L"text";
    if (first & 0x40) {  // a user data header: information elements (23.040 9.2.3.24)
        const std::size_t end = pos + 1 + at(pos);
        std::wstring part;
        for (std::size_t ie = pos + 1; ie + 1 < end && ie + 1 < octets.size(); ie += 2 + at(ie + 1)) {
            const unsigned id = at(ie);
            if (id == 0x00) part = std::format(L" part {}/{}", at(ie + 4), at(ie + 3));       // 8-bit reference
            else if (id == 0x08) part = std::format(L" part {}/{}", at(ie + 5), at(ie + 4));  // 16-bit reference
            else if (id == 0x05) {
                const unsigned port = at(ie + 2) << 8 | at(ie + 3);
                info.kind = port == 2948 ? L"MMS notice" : std::format(L"port {}", port);
            } else if (id == 0x04) {
                info.kind = std::format(L"port {}", at(ie + 2));
            }
        }
        info.kind += part;
    }
    return info;
}

int ModemCheck(Console& con) {
    const auto modem = Tel::Modem::Open();
    if (!modem) {
        con.Line(L"Modem  none (no mobile broadband modem on this PC): Android shows no SIM");
        return kUnavailable;
    }
    // The message store, as the bridge sees it when Android connects (no sender, no text).
    modem->Start({});
    if (const auto stored = modem->ReadStore(false, 15'000)) {
        con.Line(std::format(L"Messages in the modem  {}", stored->size()));
        for (const Tel::StoredMessage& message : *stored) {
            static constexpr const wchar_t* kStatus[] = {L"unread", L"read", L"draft", L"sent"};
            const DeliverInfo info = Describe(message.pdu);
            con.Line(std::format(L"  #{:<3} {:<7} {:<19}  {:>3} octets  {}", message.index, kStatus[static_cast<int>(message.status)],
                                 info.time.empty() ? std::wstring(L"-") : info.time, message.pdu.size() / 2, info.kind));
        }
    } else {
        con.Line(L"Messages in the modem  no answer from the message store");
    }
    modem->Stop();
    Tel::AtModem at(*modem);
    for (const char* command : {"AT+CPIN?", "AT+COPS=3,0;+COPS?;+COPS=3,2;+COPS?", "AT+CREG=2", "AT+CREG?", "AT+CSQ", "AT+CSCA?",
                                "AT+CRSM=192,28589,0,0,15", "AT+CRSM=176,28589,0,0,4", "ATD123;"}) {
        const Tel::AtModem::Reply reply = at.Command(command);
        std::wstring answer;
        for (const std::string& line : reply.lines) answer += (answer.empty() ? L"" : L" | ") + FromUtf8(line);
        con.Line(std::format(L"{:<40} {}", FromUtf8(command), answer));
    }
    return kOk;
}

// Root for Android apps (every request confirmed with Windows Hello): show or switch the setting.
int Elevation(Console& con, const Args& args) {
    if (args.positional.empty()) {
        con.Line(std::format(L"Root for Android apps  {}", Host::ElevationEnabled() ? L"on (every request is confirmed with Windows Hello)" : L"off"));
        con.Line(std::format(L"su for root apps       {}", Host::ElevationSuEnabled() ? L"on (apps see su, a link to 0)" : L"off"));
        return kOk;
    }
    if (Lower(args.positional[0]) == L"su") {
        const std::wstring value = args.positional.size() > 1 ? Lower(args.positional[1]) : std::wstring();
        if (value != L"on" && value != L"off") {
            con.Err(L"elevation su: on or off\n");
            return kUsage;
        }
        if (auto r = Host::SetElevationSuEnabled(value == L"on"); !r) {
            con.Status(Level::Error, r.error());
            return kFailed;
        }
        con.Status(Level::Success, value == L"on" ? L"su for root apps is on (while root for Android apps is on)" : L"su for root apps is off");
        return kOk;
    }
    const std::wstring value = Lower(args.positional[0]);
    if (value != L"on" && value != L"off") {
        con.Err(L"elevation: on or off\n");
        return kUsage;
    }
    const bool enable = value == L"on";
    if (enable && !Signature::OfficialFeatures()) {
        con.Status(Level::Error, L"root for Android apps is only in the official (signed) WSA-Next");
        return kUnavailable;
    }
    if (enable && Bio::CheckAvailability() != Bio::HelloStatus::Available)
        con.Status(Level::Warning, L"Windows Hello is not set up for this account: requests will be refused until it is");
    if (auto r = Host::SetElevationEnabled(enable); !r) {
        con.Status(Level::Error, r.error());
        return kFailed;
    }
    con.Status(Level::Success, enable ? L"root for Android apps is on: apps ask with the command 0, Windows Hello decides"
                                      : L"root for Android apps is off");
    return kOk;
}

}  // namespace

std::wstring SourceUrl(const std::wstring& source) {
    const std::wstring s = Lower(source);
    if (s == L"github") return std::format(L"https://github.com/{}/releases/download/components/{}", Config::kGitHubRepo, Config::kManifestAsset);
    if (s == L"mirror") return Config::kMirrorManifest;
    if (s == L"local" || s.empty()) return Config::kLocalManifest;
    return source;
}

std::wstring HelpText() {
    return std::format(
        L"{0} {1} - Windows Subsystem for Android with Android 15\n"
        L"\n"
        L"Usage: {2} <command> [options]   (no command: graphical interface)\n"
        L"\n"
        L"Commands:\n"
        L"  status               Windows, Virtual Machine Platform, developer mode, installed WSA\n"
        L"  check                read the release manifest and show what would be downloaded\n"
        L"  install              download, verify, unpack and register WSA-Next (administrator)\n"
        L"  uninstall            remove WSA-Next; Android apps and data stay for the next installation\n"
        L"  uninstall --all      remove everything: also Android apps and data, their Start menu entries\n"
        L"                       and associations, WSA-Next's settings; developer mode and Virtual Machine\n"
        L"                       Platform are turned off again if WSA-Next turned them on (administrator)\n"
        L"                       Installed with WSA-Next Setup (WSA-Next.msi): Windows Installer removes it\n"
        L"  bluetooth            give Android the PC's Bluetooth adapter until Ctrl+C (administrator)\n"
        L"  bluetooth --list-vms what the Host Compute Service reports (diagnostics)\n"
        L"  hello                bridge Windows Hello (fingerprint/PIN) to Android until Ctrl+C (administrator)\n"
        L"  hello --check        check whether Windows Hello is configured for this Windows account\n"
        L"  hello --test         prompt Windows Hello right now to verify the reader/PIN\n"
        L"  hello remember [off|5|30|lock] [--yes]\n"
        L"                       let Android apps that ask for a fingerprint in without asking again for 5 or\n"
        L"                       30 minutes, or until Windows is locked, after one Windows Hello confirmation\n"
        L"                       (off by default; locking Windows always ends it; root still asks every time)\n"
        L"  modem                the PC's modem as Android sees it: SIM, network, signal, SMS centre (diagnostics)\n"
        L"  elevation [on|off]   root for Android apps: an app runs \"0 [command]\" (root is uid 0) and Windows\n"
        L"                       Hello confirms every request on this PC (off by default)\n"
        L"  elevation su on|off  also offer root apps the name they look for, su (a link to 0)\n"
        L"  launch               start Android (the Play Store, or Android's Settings)\n"
        L"  stop                 stop WSA and free its memory, nothing is removed; it starts again\n"
        L"                       with the next Android app (administrator)\n"
        L"  verify [folder]      check that the installed Android is an official release, unchanged\n"
        L"  version              print the version\n"
        L"  lng-export <file>    write the English strings (start of a translation)\n"
        L"  lng-check [file]     coverage and placeholder check of language files\n"
        L"  msi stop-hosts | msi uninstall <SID> [--all]\n"
        L"                       used by WSA-Next Setup (LocalSystem): stop the background host in every\n"
        L"                       session; remove WSA-Next as the user who removes Setup, without a second\n"
        L"                       UAC prompt\n"
        L"\n"
        L"Options:\n"
        L"  --source <s>         local (http://localhost/wsa-next), mirror (kvc.pl), github,\n"
        L"                       or the URL of a manifest.json\n"
        L"  --dir <folder>       install folder (default {3})\n"
        L"  --files-only         install: files only, do not touch the Windows registration\n"
        L"  --keep-downloads     install: keep the compressed downloads\n"
        L"  --yes                install: accept the questions (developer mode, Virtual Machine Platform,\n"
        L"                       replacing WSA from the Microsoft Store); uninstall: confirm\n"
        L"  --no-progress        no progress bar\n"
        L"\n"
        L"Exit codes: 0 ok, 1 failed, 2 usage, 3 unavailable, 4 canceled, 5 restart Windows to finish\n",
        Config::kAppTitle, Config::kAppVersion, Config::kExeName, Core::DefaultFolder().wstring());
}

int Run(int argc, wchar_t** argv) {
    Console con;
    Text::Use(L"en");
    Args args;
    if (argc > 1 && IsHelp(argv[1])) {
        con.Out(HelpText());
        return kOk;
    }
    if (auto error = Parse(std::span<wchar_t*>(argv + 1, static_cast<std::size_t>(argc - 1)), args)) {
        con.Err(*error + L"\n\n");
        con.Out(HelpText());
        return kUsage;
    }
    // Package manager and WMI calls need COM (multithreaded: their async operations block).
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ComExit {
        HRESULT hr;
        ~ComExit() { if (SUCCEEDED(hr)) CoUninitialize(); }
    } comExit{com};

    if (args.command == L"version") {
        con.Line(std::format(L"{} {}", Config::kAppTitle, Config::kAppVersion));
        return kOk;
    }
    if (args.command == L"status") return Status(con, args);
    if (args.command == L"check") return Check(con, args);
    if (args.command == L"install") return Install(con, args);
    if (args.command == L"uninstall") return Uninstall(con, args);
    if (args.command == L"bluetooth") return Bluetooth(con, args);
    if (args.command == L"hello") return Hello(con, args);
    if (args.command == L"elevation") return Elevation(con, args);
    if (args.command == L"modem") return ModemCheck(con);
    if (args.command == L"verify") {
        // The background host's check, run here: the WSA package folder (or the given one).
        fs::path folder;
        if (!args.positional.empty()) folder = args.positional[0];
        else if (const auto package = Install::FindWsa()) folder = package->location;
        using Core::Release::State;
        const State state = Core::Release::VerifyInstalled(folder, {});
        con.Line(std::format(L"Folder  {}", folder.empty() ? std::wstring(L"(WSA not installed)") : folder.wstring()));
        switch (state) {
        case State::Official: con.Status(Level::Success, L"official WSA-Next release, unchanged"); return kOk;
        case State::Modified: con.Status(Level::Error, L"official release, but its Android files were changed after installing"); return kFailed;
        case State::Missing: con.Status(Level::Error, L"WSA is not installed"); return kUnavailable;
        default: con.Status(Level::Error, L"not an official WSA-Next release (no valid publisher signature)"); return kFailed;
        }
    }
    if (args.command == L"stop") {
        if (!IsElevated()) {
            con.Status(Level::Error, L"run this from an elevated (administrator) prompt");
            return kUnavailable;
        }
        switch (Install::Stop()) {
        case Guest::VmStop::ShutDown: con.Status(Level::Success, Text::Get(IDS_STATUS_WSA_STOPPED)); return kOk;
        case Guest::VmStop::Terminated: con.Status(Level::Warning, Text::Get(IDS_STATUS_WSA_TERMINATED)); return kOk;
        case Guest::VmStop::NotRunning: con.Status(Level::Info, Text::Get(IDS_STATUS_WSA_NOT_RUNNING)); return kOk;
        case Guest::VmStop::Failed: break;
        }
        con.Status(Level::Error, Text::Get(IDS_STATUS_WSA_STOP_FAILED));
        return kFailed;
    }
    if (args.command == L"launch") {
        if (auto r = Install::LaunchAndroid(); !r) {
            con.Status(Level::Error, r.error());
            return kFailed;
        }
        return kOk;
    }
    if (args.command == L"lng-export") {
        if (args.positional.size() != 1) {
            con.Err(L"lng-export needs a file name\n");
            return kUsage;
        }
        return Text::ExportTemplate(args.positional[0]) ? kOk : kFailed;
    }
    if (args.command == L"lng-check") {
        const auto reports = Text::Check(args.positional.empty() ? std::wstring() : args.positional[0]);
        int worst = kOk;
        for (const auto& r : reports) {
            con.Line(std::format(L"{} ({}): {}% translated, {} missing, {} unknown, {} broken placeholders", r.file, r.language.name,
                                 r.language.Coverage(), r.missing.size(), r.unknown.size(), r.placeholders.size()));
            if (!r.placeholders.empty() || !r.unknown.empty()) worst = kFailed;
        }
        return worst;
    }
    con.Err(std::format(L"unknown command {}\n\n", args.command));
    con.Out(HelpText());
    return kUsage;
}

}  // namespace WN::Cli
