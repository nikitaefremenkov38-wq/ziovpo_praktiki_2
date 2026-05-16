#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <rpc.h>
#include <sddl.h>

#include <algorithm>
#include <string>

#include "../resources/resource.h"
#include "common.h"
#include "WitcherControl_h.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "rpcrt4.lib")

handle_t WitcherControl_IfHandle = nullptr;

namespace {

    constexpr wchar_t kWindowClassName[] = L"TourismServiceTrayWindowClass";
    constexpr wchar_t kWindowTitle[] = L"Tourism Service Tray App";
    constexpr wchar_t kMutexName[] = L"Local\\TourismServiceTrayApp.SingleInstance";

    constexpr UINT kTrayCallbackMessage = WM_APP + 1;
    constexpr UINT_PTR kTrayIconId = 1;

    constexpr UINT kMenuOpen = 1001;
    constexpr UINT kMenuExit = 1002;

    HINSTANCE g_instance = nullptr;
    HWND g_main_window = nullptr;
    NOTIFYICONDATAW g_tray_icon{};
    UINT g_taskbar_created_message = 0;
    HANDLE g_single_instance_mutex = nullptr;
    bool g_is_exiting = false;

    std::wstring ToLower(std::wstring text) {
        std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(towlower(ch));
            });

        return text;
    }

    bool HasArgument(const wchar_t* expected) {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

        if (!argv) {
            return false;
        }

        bool found = false;

        for (int i = 1; i < argc; ++i) {
            if (_wcsicmp(argv[i], expected) == 0) {
                found = true;
                break;
            }
        }

        LocalFree(argv);
        return found;
    }

    int RunSecureStopConfirmation() {
        /*
            Secure-stop confirmation helper.

            This helper is started by the service in the active user's session:
                TrayWin32App.exe --secure-stop-confirm

            It creates a separate restricted private desktop, switches the user
            to that desktop, shows a blocking confirmation dialog, then switches
            back to the original input desktop.

            This is not the system Winlogon/UAC secure desktop, because ordinary
            user-mode applications cannot display UI on the real Winlogon desktop.
            For this assignment this is the closest practical implementation:
            a separate private desktop with a restricted DACL.
        */

        HDESK original_desktop = OpenInputDesktop(
            0,
            FALSE,
            DESKTOP_SWITCHDESKTOP |
            DESKTOP_READOBJECTS |
            DESKTOP_WRITEOBJECTS |
            DESKTOP_CREATEWINDOW
        );

        if (!original_desktop) {
            return 1;
        }

        wchar_t desktop_name[128]{};

        wsprintfW(
            desktop_name,
            L"WitcherStopConfirmationDesktop-%lu",
            GetCurrentProcessId()
        );

        PSECURITY_DESCRIPTOR security_descriptor = nullptr;

        /*
            Desktop DACL:
            - SY = LocalSystem full access
            - OW = object owner full access

            The helper process is launched with the active user's token, so the
            object owner is the active user.
        */
        constexpr wchar_t kDesktopSecurityDescriptor[] =
            L"D:P"
            L"(A;;GA;;;SY)"
            L"(A;;GA;;;OW)";

        SECURITY_ATTRIBUTES security_attributes{};
        security_attributes.nLength = sizeof(security_attributes);
        security_attributes.bInheritHandle = FALSE;
        security_attributes.lpSecurityDescriptor = nullptr;

        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            kDesktopSecurityDescriptor,
            SDDL_REVISION_1,
            &security_descriptor,
            nullptr
        )) {
            security_attributes.lpSecurityDescriptor = security_descriptor;
        }

        HDESK confirmation_desktop = CreateDesktopW(
            desktop_name,
            nullptr,
            nullptr,
            0,
            GENERIC_ALL,
            security_attributes.lpSecurityDescriptor ? &security_attributes : nullptr
        );

        if (security_descriptor) {
            LocalFree(security_descriptor);
            security_descriptor = nullptr;
        }

        if (!confirmation_desktop) {
            CloseDesktop(original_desktop);
            return 1;
        }

        const BOOL switched_to_confirmation = SwitchDesktop(confirmation_desktop);
        const BOOL thread_desktop_set = SetThreadDesktop(confirmation_desktop);

        int result = IDNO;

        if (switched_to_confirmation && thread_desktop_set) {
            result = MessageBoxW(
                nullptr,
                L"Stop WitcherTrayService and close all tray applications?",
                L"WitcherTrayService",
                MB_YESNO |
                MB_ICONWARNING |
                MB_TOPMOST |
                MB_SETFOREGROUND |
                MB_SYSTEMMODAL
            );
        }

        SwitchDesktop(original_desktop);
        SetThreadDesktop(original_desktop);

        CloseDesktop(confirmation_desktop);
        CloseDesktop(original_desktop);

        if (!switched_to_confirmation || !thread_desktop_set) {
            return 1;
        }

        return result == IDYES ? 0 : 1;
    }

    bool HasHiddenStartupArgument() {
        return HasArgument(L"--hidden") ||
            HasArgument(L"--no-window") ||
            HasArgument(L"/hidden");
    }

    bool QueryServiceState(DWORD* state) {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);

        if (!scm) {
            return false;
        }

        SC_HANDLE service = OpenServiceW(
            scm,
            witcher::kServiceName,
            SERVICE_QUERY_STATUS | SERVICE_START
        );

        if (!service) {
            CloseServiceHandle(scm);
            return false;
        }

        SERVICE_STATUS_PROCESS status{};
        DWORD bytes_needed = 0;

        const BOOL ok = QueryServiceStatusEx(
            service,
            SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&status),
            sizeof(status),
            &bytes_needed
        );

        if (ok && state) {
            *state = status.dwCurrentState;
        }

        CloseServiceHandle(service);
        CloseServiceHandle(scm);

        return ok != FALSE;
    }

    bool StartServiceAndWaitRunning() {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);

        if (!scm) {
            return false;
        }

        SC_HANDLE service = OpenServiceW(
            scm,
            witcher::kServiceName,
            SERVICE_QUERY_STATUS | SERVICE_START
        );

        if (!service) {
            CloseServiceHandle(scm);
            return false;
        }

        SERVICE_STATUS_PROCESS status{};
        DWORD bytes_needed = 0;

        if (!QueryServiceStatusEx(
            service,
            SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&status),
            sizeof(status),
            &bytes_needed
        )) {
            CloseServiceHandle(service);
            CloseServiceHandle(scm);
            return false;
        }

        if (status.dwCurrentState == SERVICE_STOPPED) {
            StartServiceW(service, 0, nullptr);
        }

        bool running = false;

        for (int i = 0; i < 50; ++i) {
            if (!QueryServiceStatusEx(
                service,
                SC_STATUS_PROCESS_INFO,
                reinterpret_cast<LPBYTE>(&status),
                sizeof(status),
                &bytes_needed
            )) {
                break;
            }

            if (status.dwCurrentState == SERVICE_RUNNING) {
                running = true;
                break;
            }

            Sleep(200);
        }

        CloseServiceHandle(service);
        CloseServiceHandle(scm);

        return running;
    }

    bool IsServiceStopped() {
        DWORD state = 0;
        return QueryServiceState(&state) && state == SERVICE_STOPPED;
    }

    DWORD GetParentProcessId() {
        const DWORD current_pid = GetCurrentProcessId();

        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

        if (snapshot == INVALID_HANDLE_VALUE) {
            return 0;
        }

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);

        DWORD parent_pid = 0;

        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (entry.th32ProcessID == current_pid) {
                    parent_pid = entry.th32ParentProcessID;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }

        CloseHandle(snapshot);

        return parent_pid;
    }

    std::wstring GetProcessImageBaseName(DWORD pid) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

        if (snapshot == INVALID_HANDLE_VALUE) {
            return L"";
        }

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);

        std::wstring name;

        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (entry.th32ProcessID == pid) {
                    name = entry.szExeFile;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }

        CloseHandle(snapshot);

        return name;
    }

    bool IsParentServiceProcess() {
        const DWORD parent_pid = GetParentProcessId();

        if (parent_pid == 0) {
            return false;
        }

        const std::wstring parent_name = ToLower(GetProcessImageBaseName(parent_pid));
        return parent_name == ToLower(witcher::kServiceExeName);
    }

    void RequestServiceStop() {
        RPC_WSTR string_binding = nullptr;

        RPC_STATUS status = RpcStringBindingComposeW(
            nullptr,
            reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(L"ncalrpc")),
            nullptr,
            reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(witcher::kRpcEndpoint)),
            nullptr,
            &string_binding
        );

        if (status != RPC_S_OK) {
            return;
        }

        status = RpcBindingFromStringBindingW(
            string_binding,
            &WitcherControl_IfHandle
        );

        RpcStringFreeW(&string_binding);

        if (status != RPC_S_OK) {
            return;
        }

        RpcTryExcept
        {
            RpcStopService();
        }
            RpcExcept(1)
        {
        }
        RpcEndExcept

            RpcBindingFree(&WitcherControl_IfHandle);
        WitcherControl_IfHandle = nullptr;
    }

    void ShowMainWindow() {
        if (!g_main_window) {
            return;
        }

        ShowWindow(g_main_window, SW_SHOWNORMAL);
        SetForegroundWindow(g_main_window);
    }

    void RemoveTrayIcon() {
        if (g_tray_icon.cbSize != 0) {
            Shell_NotifyIconW(NIM_DELETE, &g_tray_icon);
            g_tray_icon = {};
        }
    }

    void AddTrayIcon(HWND hwnd) {
        g_tray_icon = {};
        g_tray_icon.cbSize = sizeof(g_tray_icon);
        g_tray_icon.hWnd = hwnd;
        g_tray_icon.uID = kTrayIconId;
        g_tray_icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        g_tray_icon.uCallbackMessage = kTrayCallbackMessage;
        g_tray_icon.hIcon = LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_TRAY_ICON));

        wcscpy_s(g_tray_icon.szTip, L"Tourism Service Tray App");

        Shell_NotifyIconW(NIM_ADD, &g_tray_icon);

        g_tray_icon.uVersion = NOTIFYICON_VERSION_4;

        Shell_NotifyIconW(NIM_SETVERSION, &g_tray_icon);
    }

    void ExitApplication() {
        g_is_exiting = true;
        RemoveTrayIcon();
        PostQuitMessage(0);
    }

    void StopServiceAndExit() {
        RequestServiceStop();

        /*
            Do not close the GUI immediately.

            If the user confirms the service stop on the private desktop,
            the service will terminate all launched TrayWin32App.exe processes.

            If the user clicks No or the confirmation cannot be shown,
            the tray app remains running.
        */
    }

    void ShowTrayMenu(HWND hwnd) {
        POINT cursor_position{};
        GetCursorPos(&cursor_position);

        HMENU menu = CreatePopupMenu();

        if (!menu) {
            return;
        }

        AppendMenuW(menu, MF_STRING, kMenuOpen, L"\u041E\u0442\u043A\u0440\u044B\u0442\u044C");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuExit, L"\u0412\u044B\u0445\u043E\u0434");

        SetForegroundWindow(hwnd);

        const UINT command = TrackPopupMenu(
            menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
            cursor_position.x,
            cursor_position.y,
            0,
            hwnd,
            nullptr
        );

        DestroyMenu(menu);

        switch (command) {
        case kMenuOpen:
            ShowMainWindow();
            break;

        case kMenuExit:
            StopServiceAndExit();
            break;

        default:
            break;
        }
    }

    HMENU CreateMainMenu() {
        HMENU menu_bar = CreateMenu();
        HMENU file_menu = CreatePopupMenu();

        AppendMenuW(file_menu, MF_STRING, kMenuExit, L"\u0412\u044B\u0445\u043E\u0434");
        AppendMenuW(menu_bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file_menu), L"\u0424\u0430\u0439\u043B");

        return menu_bar;
    }

    void PaintMainWindow(HWND hwnd) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rect{};
        GetClientRect(hwnd, &rect);

        HBRUSH background = CreateSolidBrush(RGB(45, 45, 45));
        FillRect(hdc, &rect, background);
        DeleteObject(background);

        HPEN shadow_pen = CreatePen(PS_SOLID, 16, RGB(20, 20, 20));
        HGDIOBJ old_pen = SelectObject(hdc, shadow_pen);

        MoveToEx(hdc, 190, 85, nullptr);
        LineTo(hdc, 110, 305);

        MoveToEx(hdc, 310, 85, nullptr);
        LineTo(hdc, 230, 305);

        MoveToEx(hdc, 430, 85, nullptr);
        LineTo(hdc, 350, 305);

        SelectObject(hdc, old_pen);
        DeleteObject(shadow_pen);

        HPEN red_pen = CreatePen(PS_SOLID, 10, RGB(170, 0, 0));
        old_pen = SelectObject(hdc, red_pen);

        MoveToEx(hdc, 180, 80, nullptr);
        LineTo(hdc, 100, 300);

        MoveToEx(hdc, 300, 80, nullptr);
        LineTo(hdc, 220, 300);

        MoveToEx(hdc, 420, 80, nullptr);
        LineTo(hdc, 340, 300);

        SelectObject(hdc, old_pen);
        DeleteObject(red_pen);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(230, 230, 230));

        RECT text_rect = rect;
        text_rect.top = rect.bottom - 70;

        DrawTextW(
            hdc,
            L"Tourism Service Tray App",
            -1,
            &text_rect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE
        );

        EndPaint(hwnd, &ps);
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
        if (message == g_taskbar_created_message) {
            AddTrayIcon(hwnd);
            return 0;
        }

        switch (message) {
        case WM_CREATE:
            AddTrayIcon(hwnd);
            return 0;

        case kTrayCallbackMessage:
            switch (LOWORD(l_param)) {
            case WM_LBUTTONUP:
            case NIN_SELECT:
            case NIN_KEYSELECT:
                ShowMainWindow();
                return 0;

            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:
                ShowTrayMenu(hwnd);
                return 0;

            default:
                return 0;
            }

        case WM_COMMAND:
            switch (LOWORD(w_param)) {
            case kMenuExit:
                StopServiceAndExit();
                return 0;

            default:
                break;
            }

            break;

        case WM_PAINT:
            PaintMainWindow(hwnd);
            return 0;

        case WM_CLOSE:
            if (!g_is_exiting) {
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }

            break;

        case WM_DESTROY:
            RemoveTrayIcon();
            PostQuitMessage(0);
            return 0;

        default:
            break;
        }

        return DefWindowProcW(hwnd, message, w_param, l_param);
    }

    bool RegisterMainWindowClass() {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = WindowProc;
        window_class.hInstance = g_instance;
        window_class.hIcon = LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_TRAY_ICON));
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = kWindowClassName;
        window_class.hIconSm = LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_TRAY_ICON));

        return RegisterClassExW(&window_class) != 0;
    }

    HWND CreateMainWindow() {
        return CreateWindowExW(
            0,
            kWindowClassName,
            kWindowTitle,
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            640,
            420,
            nullptr,
            CreateMainMenu(),
            g_instance,
            nullptr
        );
    }

}

extern "C" void* __RPC_USER midl_user_allocate(size_t size) {
    return HeapAlloc(GetProcessHeap(), 0, size);
}

extern "C" void __RPC_USER midl_user_free(void* pointer) {
    HeapFree(GetProcessHeap(), 0, pointer);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    g_instance = instance;

    if (HasArgument(L"--secure-stop-confirm")) {
        return RunSecureStopConfirmation();
    }

    if (IsServiceStopped()) {
        StartServiceAndWaitRunning();
        return 0;
    }

    if (!IsParentServiceProcess()) {
        return 0;
    }

    g_single_instance_mutex = CreateMutexW(nullptr, TRUE, kMutexName);

    if (!g_single_instance_mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_single_instance_mutex) {
            CloseHandle(g_single_instance_mutex);
        }

        return 0;
    }

    g_taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");

    if (!RegisterMainWindowClass()) {
        CloseHandle(g_single_instance_mutex);
        return 1;
    }

    g_main_window = CreateMainWindow();

    if (!g_main_window) {
        CloseHandle(g_single_instance_mutex);
        return 1;
    }

    if (!HasHiddenStartupArgument()) {
        ShowWindow(g_main_window, show_command);
        UpdateWindow(g_main_window);
    }

    MSG message{};

    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (g_single_instance_mutex) {
        ReleaseMutex(g_single_instance_mutex);
        CloseHandle(g_single_instance_mutex);
    }

    return static_cast<int>(message.wParam);
}