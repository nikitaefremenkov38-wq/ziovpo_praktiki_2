#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <rpc.h>

#include <filesystem>
#include <string>
#include <vector>

#include "common.h"
#include "WitcherControl_h.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "rpcrt4.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")

namespace {

SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_stop_event = nullptr;
CRITICAL_SECTION g_process_lock;
CRITICAL_SECTION g_state_lock;
std::vector<PROCESS_INFORMATION> g_children;
struct UserRecord {
    std::wstring username;
    std::wstring password;
};
std::vector<UserRecord> g_users;

struct AuthState {
    bool authenticated = false;
    std::wstring username;
    std::wstring access_token;
    std::wstring refresh_token;
    ULONGLONG access_expiry_ms = 0;
    ULONGLONG refresh_expiry_ms = 0;
};

struct LicenseState {
    bool has_license = false;
    std::wstring expires_at;
    ULONGLONG expiry_ms = 0;
};

AuthState g_auth_state;
LicenseState g_license_state;
HANDLE g_refresh_thread = nullptr;

bool InstallService() {
    wchar_t module_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module_path, MAX_PATH);
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        return false;
    }
    SC_HANDLE service = CreateServiceW(
        scm,
        witcher::kServiceName,
        witcher::kServiceDisplayName,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        module_path,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr);
    if (!service && GetLastError() == ERROR_SERVICE_EXISTS) {
        service = OpenServiceW(scm, witcher::kServiceName, SERVICE_CHANGE_CONFIG);
    }
    if (service) {
        CloseServiceHandle(service);
    }
    CloseServiceHandle(scm);
    return service != nullptr;
}

bool UninstallService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        return false;
    }
    SC_HANDLE service = OpenServiceW(scm, witcher::kServiceName, DELETE);
    if (!service) {
        CloseServiceHandle(scm);
        return false;
    }
    bool ok = DeleteService(service) != FALSE;
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return ok;
}

ULONGLONG NowMs() {
    return GetTickCount64();
}

void SetServiceStatusValue(DWORD state, DWORD win32_exit_code = NO_ERROR, DWORD wait_hint = 0) {
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwControlsAccepted = (state == SERVICE_RUNNING) ? SERVICE_ACCEPT_SESSIONCHANGE : 0;
    g_status.dwWin32ExitCode = win32_exit_code;
    g_status.dwServiceSpecificExitCode = 0;
    g_status.dwCheckPoint = 0;
    g_status.dwWaitHint = wait_hint;
    SetServiceStatus(g_status_handle, &g_status);
}

std::filesystem::path GetModuleDirectory() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

bool IsSessionAlreadyStarted(DWORD session_id) {
    EnterCriticalSection(&g_process_lock);
    for (auto it = g_children.begin(); it != g_children.end();) {
        DWORD exit_code = 0;
        if (GetExitCodeProcess(it->hProcess, &exit_code) && exit_code != STILL_ACTIVE) {
            CloseHandle(it->hProcess);
            CloseHandle(it->hThread);
            it = g_children.erase(it);
            continue;
        }
        DWORD child_session = 0;
        if (ProcessIdToSessionId(it->dwProcessId, &child_session) && child_session == session_id) {
            LeaveCriticalSection(&g_process_lock);
            return true;
        }
        ++it;
    }
    LeaveCriticalSection(&g_process_lock);
    return false;
}

bool LaunchTrayAppInSession(DWORD session_id) {
    if (session_id == 0 || IsSessionAlreadyStarted(session_id)) {
        return false;
    }

    HANDLE user_token = nullptr;
    if (!WTSQueryUserToken(session_id, &user_token)) {
        return false;
    }

    HANDLE primary_token = nullptr;
    SECURITY_ATTRIBUTES token_attributes{};
    token_attributes.nLength = sizeof(token_attributes);
    if (!DuplicateTokenEx(
            user_token,
            TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
            &token_attributes,
            SecurityIdentification,
            TokenPrimary,
            &primary_token)) {
        CloseHandle(user_token);
        return false;
    }
    CloseHandle(user_token);

    void* environment = nullptr;
    CreateEnvironmentBlock(&environment, primary_token, FALSE);

    const auto module_directory = GetModuleDirectory();
    const auto app_path = module_directory / witcher::kTrayAppExeName;
    std::wstring command_line = L"\"" + app_path.wstring() + L"\" --hidden --service-child";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

    PROCESS_INFORMATION process{};
    BOOL created = CreateProcessAsUserW(
        primary_token,
        app_path.c_str(),
        command_line.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_UNICODE_ENVIRONMENT,
        environment,
        module_directory.c_str(),
        &startup,
        &process);

    if (environment) {
        DestroyEnvironmentBlock(environment);
    }
    CloseHandle(primary_token);

    if (!created) {
        return false;
    }

    EnterCriticalSection(&g_process_lock);
    g_children.push_back(process);
    LeaveCriticalSection(&g_process_lock);
    return true;
}

void LaunchTrayAppsInExistingSessions() {
    WTS_SESSION_INFOW* sessions = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
        return;
    }
    for (DWORD i = 0; i < count; ++i) {
        const auto& session = sessions[i];
        if (session.SessionId != 0 &&
            (session.State == WTSActive || session.State == WTSConnected || session.State == WTSDisconnected)) {
            LaunchTrayAppInSession(session.SessionId);
        }
    }
    WTSFreeMemory(sessions);
}

void TerminateChildren() {
    EnterCriticalSection(&g_process_lock);
    for (auto& child : g_children) {
        DWORD exit_code = 0;
        if (GetExitCodeProcess(child.hProcess, &exit_code) && exit_code == STILL_ACTIVE) {
            TerminateProcess(child.hProcess, 0);
            WaitForSingleObject(child.hProcess, 5000);
        }
        CloseHandle(child.hProcess);
        CloseHandle(child.hThread);
    }
    g_children.clear();
    LeaveCriticalSection(&g_process_lock);
}

void SetDemoTokensLocked(const std::wstring& username) {
    const ULONGLONG now = NowMs();
    g_auth_state.authenticated = true;
    g_auth_state.username = username;
    g_auth_state.access_token = L"demo_access_token";
    g_auth_state.refresh_token = L"demo_refresh_token";
    g_auth_state.access_expiry_ms = now + 10ull * 60ull * 1000ull;
    g_auth_state.refresh_expiry_ms = now + 24ull * 60ull * 60ull * 1000ull;
}

const UserRecord* FindUserLocked(const std::wstring& username) {
    for (const auto& user : g_users) {
        if (_wcsicmp(user.username.c_str(), username.c_str()) == 0) {
            return &user;
        }
    }
    return nullptr;
}

void ClearAuthAndLicenseLocked() {
    g_auth_state = AuthState{};
    g_license_state = LicenseState{};
}

void RefreshTokensLocked() {
    if (!g_auth_state.authenticated) {
        return;
    }
    const ULONGLONG now = NowMs();
    g_auth_state.access_expiry_ms = now + 10ull * 60ull * 1000ull;
    g_auth_state.refresh_expiry_ms = now + 24ull * 60ull * 60ull * 1000ull;
}

void RefreshLicenseLocked() {
    if (!g_license_state.has_license) {
        return;
    }
    const ULONGLONG now = NowMs();
    g_license_state.expiry_ms = now + 30ull * 24ull * 60ull * 60ull * 1000ull;
    g_license_state.expires_at = L"30 days from now";
}

DWORD WINAPI RefreshThreadProc(void*) {
    while (WaitForSingleObject(g_stop_event, 1000) == WAIT_TIMEOUT) {
        EnterCriticalSection(&g_state_lock);
        const ULONGLONG now = NowMs();
        if (g_auth_state.authenticated &&
            g_auth_state.access_expiry_ms > 0 &&
            g_auth_state.access_expiry_ms <= now + 60ull * 1000ull) {
            RefreshTokensLocked();
        }
        if (g_auth_state.authenticated &&
            g_auth_state.refresh_expiry_ms > 0 &&
            g_auth_state.refresh_expiry_ms <= now + 5ull * 60ull * 1000ull) {
            RefreshTokensLocked();
        }
        if (g_license_state.has_license &&
            g_license_state.expiry_ms > 0 &&
            g_license_state.expiry_ms <= now + 60ull * 1000ull) {
            RefreshLicenseLocked();
        }
        LeaveCriticalSection(&g_state_lock);
    }
    return 0;
}

DWORD WINAPI RpcServerThread(void*) {
    RPC_STATUS status = RpcServerUseProtseqEpW(
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(L"ncalrpc")),
        RPC_C_PROTSEQ_MAX_REQS_DEFAULT,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(witcher::kRpcEndpoint)),
        nullptr);

    if (status != RPC_S_OK && status != RPC_S_DUPLICATE_ENDPOINT) {
        SetEvent(g_stop_event);
        return status;
    }

    status = RpcServerRegisterIf2(
        WitcherControl_v1_0_s_ifspec,
        nullptr,
        nullptr,
        RPC_IF_ALLOW_LOCAL_ONLY,
        RPC_C_LISTEN_MAX_CALLS_DEFAULT,
        static_cast<unsigned>(-1),
        nullptr);

    if (status != RPC_S_OK) {
        SetEvent(g_stop_event);
        return status;
    }

    status = RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, TRUE);
    if (status != RPC_S_OK) {
        SetEvent(g_stop_event);
    }
    return status;
}

DWORD WINAPI ServiceControlHandlerEx(DWORD control, DWORD event_type, void* event_data, void*) {
    if (control == SERVICE_CONTROL_SESSIONCHANGE) {
        if (event_type == WTS_SESSION_LOGON ||
            event_type == WTS_SESSION_UNLOCK ||
            event_type == WTS_CONSOLE_CONNECT ||
            event_type == WTS_REMOTE_CONNECT) {
            auto* notification = static_cast<WTSSESSION_NOTIFICATION*>(event_data);
            if (notification && notification->dwSessionId != 0) {
                LaunchTrayAppInSession(notification->dwSessionId);
            }
        }
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_status_handle = RegisterServiceCtrlHandlerExW(witcher::kServiceName, ServiceControlHandlerEx, nullptr);
    if (!g_status_handle) {
        return;
    }

    SetServiceStatusValue(SERVICE_START_PENDING, NO_ERROR, 3000);
    InitializeCriticalSection(&g_process_lock);
    InitializeCriticalSection(&g_state_lock);
    g_users.push_back({L"admin", L"123456"});

    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop_event) {
        SetServiceStatusValue(SERVICE_STOPPED, GetLastError());
        DeleteCriticalSection(&g_process_lock);
        DeleteCriticalSection(&g_state_lock);
        return;
    }

    HANDLE rpc_thread = CreateThread(nullptr, 0, RpcServerThread, nullptr, 0, nullptr);
    if (!rpc_thread) {
        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
        SetServiceStatusValue(SERVICE_STOPPED, GetLastError());
        DeleteCriticalSection(&g_process_lock);
        DeleteCriticalSection(&g_state_lock);
        return;
    }

    g_refresh_thread = CreateThread(nullptr, 0, RefreshThreadProc, nullptr, 0, nullptr);
    LaunchTrayAppsInExistingSessions();
    SetServiceStatusValue(SERVICE_RUNNING);

    WaitForSingleObject(g_stop_event, INFINITE);
    SetServiceStatusValue(SERVICE_STOP_PENDING, NO_ERROR, 5000);

    RpcMgmtStopServerListening(nullptr);
    WaitForSingleObject(rpc_thread, 5000);
    RpcServerUnregisterIf(WitcherControl_v1_0_s_ifspec, nullptr, FALSE);
    if (g_refresh_thread) {
        WaitForSingleObject(g_refresh_thread, 3000);
        CloseHandle(g_refresh_thread);
        g_refresh_thread = nullptr;
    }
    CloseHandle(rpc_thread);

    TerminateChildren();
    EnterCriticalSection(&g_state_lock);
    ClearAuthAndLicenseLocked();
    LeaveCriticalSection(&g_state_lock);

    CloseHandle(g_stop_event);
    g_stop_event = nullptr;
    DeleteCriticalSection(&g_process_lock);
    DeleteCriticalSection(&g_state_lock);
    SetServiceStatusValue(SERVICE_STOPPED);
}

} // namespace

extern "C" unsigned long RpcStopService(void) {
    if (g_stop_event) {
        SetEvent(g_stop_event);
    }
    return ERROR_SUCCESS;
}

extern "C" unsigned long RpcGetAuthenticatedUser(WitcherUserInfo* info) {
    if (!info) {
        return witcher::kRpcErrInvalidArgument;
    }
    ZeroMemory(info, sizeof(*info));
    EnterCriticalSection(&g_state_lock);
    info->is_authenticated = g_auth_state.authenticated ? 1 : 0;
    if (g_auth_state.authenticated) {
        wcsncpy_s(info->username, g_auth_state.username.c_str(), _TRUNCATE);
    }
    LeaveCriticalSection(&g_state_lock);
    return ERROR_SUCCESS;
}

extern "C" unsigned long RpcRegisterUser(wchar_t* username, wchar_t* password) {
    if (!username || !password || wcslen(username) == 0 || wcslen(password) == 0) {
        return witcher::kRpcErrInvalidArgument;
    }
    EnterCriticalSection(&g_state_lock);
    if (FindUserLocked(username) != nullptr) {
        LeaveCriticalSection(&g_state_lock);
        return witcher::kRpcErrInvalidArgument;
    }
    g_users.push_back({username, password});
    LeaveCriticalSection(&g_state_lock);
    return ERROR_SUCCESS;
}

extern "C" unsigned long RpcLogin(wchar_t* username, wchar_t* password) {
    if (!username || !password || wcslen(username) == 0 || wcslen(password) == 0) {
        return witcher::kRpcErrInvalidArgument;
    }
    EnterCriticalSection(&g_state_lock);
    const UserRecord* found = FindUserLocked(username);
    if (!found || wcscmp(found->password.c_str(), password) != 0) {
        LeaveCriticalSection(&g_state_lock);
        return witcher::kRpcErrUnauthorized;
    }
    SetDemoTokensLocked(username);
    LeaveCriticalSection(&g_state_lock);
    return ERROR_SUCCESS;
}

extern "C" unsigned long RpcLogout(void) {
    EnterCriticalSection(&g_state_lock);
    ClearAuthAndLicenseLocked();
    LeaveCriticalSection(&g_state_lock);
    return ERROR_SUCCESS;
}

extern "C" unsigned long RpcGetLicenseInfo(WitcherLicenseInfo* info) {
    if (!info) {
        return witcher::kRpcErrInvalidArgument;
    }
    ZeroMemory(info, sizeof(*info));
    EnterCriticalSection(&g_state_lock);
    if (!g_auth_state.authenticated) {
        LeaveCriticalSection(&g_state_lock);
        return witcher::kRpcErrUnauthorized;
    }
    info->has_license = g_license_state.has_license ? 1 : 0;
    if (g_license_state.has_license) {
        wcsncpy_s(info->expires_at, g_license_state.expires_at.c_str(), _TRUNCATE);
    }
    LeaveCriticalSection(&g_state_lock);
    return ERROR_SUCCESS;
}

extern "C" unsigned long RpcActivateProduct(wchar_t* activation_code) {
    if (!activation_code || wcslen(activation_code) == 0) {
        return witcher::kRpcErrInvalidArgument;
    }
    EnterCriticalSection(&g_state_lock);
    if (!g_auth_state.authenticated) {
        LeaveCriticalSection(&g_state_lock);
        return witcher::kRpcErrUnauthorized;
    }
    if (_wcsicmp(activation_code, witcher::kValidActivationCode) != 0) {
        LeaveCriticalSection(&g_state_lock);
        return witcher::kRpcErrNoLicense;
    }
    g_license_state.has_license = true;
    g_license_state.expiry_ms = NowMs() + 30ull * 24ull * 60ull * 60ull * 1000ull;
    g_license_state.expires_at = L"30 days from now";
    LeaveCriticalSection(&g_state_lock);
    return ERROR_SUCCESS;
}

extern "C" unsigned long RpcGetAntivirusStatus(long* is_available) {
    if (!is_available) {
        return witcher::kRpcErrInvalidArgument;
    }
    EnterCriticalSection(&g_state_lock);
    const bool available = g_auth_state.authenticated && g_license_state.has_license;
    *is_available = available ? 1 : 0;
    LeaveCriticalSection(&g_state_lock);
    if (!available) {
        return witcher::kRpcErrNoLicense;
    }
    return ERROR_SUCCESS;
}

extern "C" void* __RPC_USER midl_user_allocate(size_t size) {
    return HeapAlloc(GetProcessHeap(), 0, size);
}

extern "C" void __RPC_USER midl_user_free(void* pointer) {
    HeapFree(GetProcessHeap(), 0, pointer);
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc > 1) {
        const std::wstring arg = argv[1];
        if (arg == L"--install") {
            return InstallService() ? 0 : 1;
        }
        if (arg == L"--uninstall") {
            return UninstallService() ? 0 : 1;
        }
    }

    SERVICE_TABLE_ENTRYW service_table[] = {
        {const_cast<LPWSTR>(witcher::kServiceName), ServiceMain},
        {nullptr, nullptr}};
    return StartServiceCtrlDispatcherW(service_table) ? 0 : 1;
}
