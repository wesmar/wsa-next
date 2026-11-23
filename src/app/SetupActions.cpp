// WSA-Next © 2026 Marek Wesołowski - wesmar
#include "app/SetupActions.h"
#include "support/Common.h"
#include "support/Handle.h"
#include <Windows.h>
#include <WtsApi32.h>
#include <sddl.h>
#include <UserEnv.h>
#include <format>
#include <string>
#include <vector>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "advapi32.lib")

namespace WN::SetupActions {
namespace {

constexpr int kOk = 0, kFailed = 1, kUsage = 2, kUnavailable = 3;  // as the command line's

// The sessions Windows has now (WTSEnumerateSessions), as ids.
std::vector<DWORD> Sessions() {
    std::vector<DWORD> ids;
    PWTS_SESSION_INFOW list = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &list, &count)) return ids;
    for (DWORD i = 0; i < count; ++i) ids.push_back(list[i].SessionId);
    WTSFreeMemory(list);
    return ids;
}

// The host keeps "Local\WSA-Next.Host" (a mutex it owns while it runs) and "Local\WSA-Next.Host.Stop"
// in its session; from session 0 they are "Global\Session\<id>\...". Each host is told to stop, then
// waited for: its mutex is abandoned when it has ended. The hosts stop together, 10 s at most.
int StopHosts() {
    struct Running {
        DWORD session;
        KernelHandle mutex;
    };
    std::vector<Running> hosts;
    for (const DWORD id : Sessions()) {
        const std::wstring prefix = std::format(L"Global\\Session\\{}\\WSA-Next.Host", id);
        KernelHandle mutex(OpenMutexW(SYNCHRONIZE, FALSE, prefix.c_str()));
        if (!mutex) continue;
        const KernelHandle stop(OpenEventW(EVENT_MODIFY_STATE, FALSE, (prefix + L".Stop").c_str()));
        if (!stop || !SetEvent(stop.get())) continue;
        hosts.push_back({id, std::move(mutex)});
    }
    const ULONGLONG deadline = GetTickCount64() + 10000;
    int result = kOk;
    for (Running& host : hosts) {
        const ULONGLONG now = GetTickCount64();
        const DWORD wait = WaitForSingleObject(host.mutex.get(), now < deadline ? static_cast<DWORD>(deadline - now) : 0);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) ReleaseMutex(host.mutex.get());
        else result = kFailed;  // still running: Windows Installer replaces the file at the next restart
    }
    return result;
}

bool EnablePrivilege(const wchar_t* name) {
    KernelHandle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, token.put())) return false;
    TOKEN_PRIVILEGES privileges{1, {{{}, SE_PRIVILEGE_ENABLED}}};
    if (!LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid)) return false;
    return AdjustTokenPrivileges(token.get(), FALSE, &privileges, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
}

bool TokenIsUser(HANDLE token, PSID user) {
    alignas(TOKEN_USER) unsigned char buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    DWORD size = 0;
    return GetTokenInformation(token, TokenUser, buffer, sizeof buffer, &size) &&
           EqualSid(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid, user);
}

// The token of the user with `sid` in one of the user's sessions (an active one first), the full
// one when UAC filtered it: the removal changes %ProgramFiles%, HKLM and Windows features.
KernelHandle UserToken(PSID sid, DWORD& session) {
    KernelHandle found;
    for (const bool activeOnly : {true, false}) {
        for (const DWORD id : Sessions()) {
            if (activeOnly) {
                WTS_CONNECTSTATE_CLASS* state = nullptr;
                DWORD bytes = 0;
                if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, id, WTSConnectState, reinterpret_cast<LPWSTR*>(&state), &bytes))
                    continue;
                const bool active = *state == WTSActive;
                WTSFreeMemory(state);
                if (!active) continue;
            }
            KernelHandle token;
            if (!WTSQueryUserToken(id, token.put()) || !TokenIsUser(token.get(), sid)) continue;
            session = id;
            found = std::move(token);
            break;
        }
        if (found) break;
    }
    if (!found) return found;
    TOKEN_ELEVATION_TYPE type{};
    DWORD size = 0;
    if (GetTokenInformation(found.get(), TokenElevationType, &type, sizeof type, &size) && type == TokenElevationTypeLimited) {
        TOKEN_LINKED_TOKEN linked{};
        if (GetTokenInformation(found.get(), TokenLinkedToken, &linked, sizeof linked, &size)) return KernelHandle(linked.LinkedToken);
    }
    return found;
}

int UninstallAsUser(const wchar_t* sidText, bool everything) {
    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(sidText, &sid)) return kUsage;
    EnablePrivilege(SE_TCB_NAME);  // WTSQueryUserToken
    EnablePrivilege(SE_ASSIGNPRIMARYTOKEN_NAME);
    EnablePrivilege(SE_INCREASE_QUOTA_NAME);
    // Failures here exit with a Win32 error code, so that Setup's log tells them from the exit codes
    // (0-5) of the removal itself.
    DWORD session = 0;
    const KernelHandle token = UserToken(sid, session);
    LocalFree(sid);
    if (!token) return ERROR_NO_TOKEN;  // not signed in (or not running as LocalSystem)

    void* environment = nullptr;
    if (!CreateEnvironmentBlock(&environment, token.get(), FALSE)) environment = nullptr;
    // Output nowhere: no console window appears in the user's session.
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    const KernelHandle nul(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr));
    STARTUPINFOW si{sizeof si};
    si.lpDesktop = const_cast<wchar_t*>(L"winsta0\\default");
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = si.hStdOutput = si.hStdError = nul.get();
    const std::wstring exe = ModulePath();
    std::wstring command = std::format(L"\"{}\" uninstall --yes --from-setup{}", exe, everything ? L" --all" : L"");
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessAsUserW(token.get(), exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                                              CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, environment, system, &si, &pi);
    const DWORD error = GetLastError();
    if (environment) DestroyEnvironmentBlock(environment);
    if (!started) return static_cast<int>(error);
    CloseHandle(pi.hThread);
    const KernelHandle process(pi.hProcess);
    WaitForSingleObject(process.get(), INFINITE);
    DWORD code = kFailed;
    GetExitCodeProcess(process.get(), &code);
    return static_cast<int>(code);
}

}  // namespace

int Run(int argc, wchar_t** argv) {
    if (!IsElevated()) return kUnavailable;
    if (argc == 3 && _wcsicmp(argv[2], L"stop-hosts") == 0) return StopHosts();
    if (argc >= 4 && argc <= 5 && _wcsicmp(argv[2], L"uninstall") == 0) {
        const bool everything = argc == 5 && _wcsicmp(argv[4], L"--all") == 0;
        if (argc == 5 && !everything) return kUsage;
        return UninstallAsUser(argv[3], everything);
    }
    return kUsage;
}

}  // namespace WN::SetupActions
