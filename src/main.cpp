#include <windows.h>
#include <shellapi.h>
#include <strsafe.h>
#include "../resources/resource.h"

namespace {

    constexpr wchar_t kWindowClassName[] = L"TrayWin32AppWindowClass";
    constexpr wchar_t kAppTitle[] = L"Tray Win32 App";
    constexpr wchar_t kMutexName[] = L"Local\\TrayWin32App_SingleInstanceMutex";

    constexpr UINT kTrayIconId = 1;
    constexpr UINT kWmTrayIcon = WM_APP + 1;
    constexpr UINT kMenuOpen = 1001;
    constexpr UINT kMenuExit = 1002;
    constexpr UINT kMenuFileExit = 2001;

    HINSTANCE g_instance = nullptr;
    HWND g_main_window = nullptr;
    HANDLE g_single_instance_mutex = nullptr;
    UINT g_taskbar_created_message = 0;
    bool g_tray_icon_added = false;

    bool IsHiddenLaunch(int argc, wchar_t** argv) {
        for (int i = 1; i < argc; ++i) {
            if (_wcsicmp(argv[i], L"--hidden") == 0 ||
                _wcsicmp(argv[i], L"/hidden") == 0 ||
                _wcsicmp(argv[i], L"--no-window") == 0 ||
                _wcsicmp(argv[i], L"/no-window") == 0) {
                return true;
            }
        }
        return false;
    }

    void ShowMainWindow() {
        if (!g_main_window) return;
        ShowWindow(g_main_window, SW_SHOWNORMAL);
        SetForegroundWindow(g_main_window);
    }

    void RemoveTrayIcon() {
        if (!g_tray_icon_added || !g_main_window) return;

        NOTIFYICONDATA nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = g_main_window;
        nid.uID = kTrayIconId;
        Shell_NotifyIcon(NIM_DELETE, &nid);
        g_tray_icon_added = false;
    }

    bool AddTrayIcon() {
        if (!g_main_window) return false;

        NOTIFYICONDATA nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = g_main_window;
        nid.uID = kTrayIconId;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = kWmTrayIcon;
        nid.hIcon = LoadIcon(g_instance, MAKEINTRESOURCE(IDI_TRAY_ICON));
        StringCchCopy(nid.szTip, ARRAYSIZE(nid.szTip), kAppTitle);

        const BOOL result = Shell_NotifyIcon(NIM_ADD, &nid);
        if (result) {
            nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIcon(NIM_SETVERSION, &nid);
            g_tray_icon_added = true;
        }

        return result == TRUE;
    }

    void RecreateTrayIcon() {
        g_tray_icon_added = false;
        AddTrayIcon();
    }

    void ExitApplication() {
        RemoveTrayIcon();
        DestroyWindow(g_main_window);
        PostQuitMessage(0);
    }

    void ShowTrayMenu(HWND hwnd) {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;

        AppendMenu(menu, MF_STRING, kMenuOpen, L"\u041E\u0442\u043A\u0440\u044B\u0442\u044C");
        AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenu(menu, MF_STRING, kMenuExit, L"\u0412\u044B\u0445\u043E\u0434");

        POINT cursor{};
        GetCursorPos(&cursor);

        SetForegroundWindow(hwnd);

        TrackPopupMenu(
            menu,
            TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
            cursor.x,
            cursor.y,
            0,
            hwnd,
            nullptr
        );

        DestroyMenu(menu);
    }

    HMENU CreateMainMenu() {
        HMENU menu_bar = CreateMenu();
        HMENU file_menu = CreatePopupMenu();

        AppendMenu(file_menu, MF_STRING, kMenuFileExit, L"\u0412\u044B\u0445\u043E\u0434");
        AppendMenu(menu_bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file_menu), L"\u0424\u0430\u0439\u043B");

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
            L"Tray Win32 App",
            -1,
            &text_rect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE
        );

        EndPaint(hwnd, &ps);
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
        if (message == g_taskbar_created_message) {
            RecreateTrayIcon();
            return 0;
        }

        switch (message) {
        case WM_COMMAND: {
            const UINT command = LOWORD(w_param);
            if (command == kMenuOpen) {
                ShowMainWindow();
                return 0;
            }
            if (command == kMenuExit || command == kMenuFileExit) {
                ExitApplication();
                return 0;
            }
            break;
        }

        case kWmTrayIcon:
            if (LOWORD(l_param) == WM_LBUTTONUP) {
                ShowMainWindow();
                return 0;
            }
            if (LOWORD(l_param) == WM_RBUTTONUP || LOWORD(l_param) == WM_CONTEXTMENU) {
                ShowTrayMenu(hwnd);
                return 0;
            }
            break;

        case WM_PAINT:
            PaintMainWindow(hwnd);
            return 0;

        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;

        case WM_DESTROY:
            RemoveTrayIcon();
            return 0;

        default:
            break;
        }

        return DefWindowProc(hwnd, message, w_param, l_param);
    }

    bool RegisterMainWindowClass() {
        WNDCLASSEX wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = g_instance;
        wc.lpszClassName = kWindowClassName;
        wc.hIcon = LoadIcon(g_instance, MAKEINTRESOURCE(IDI_TRAY_ICON));
        wc.hIconSm = LoadIcon(g_instance, MAKEINTRESOURCE(IDI_TRAY_ICON));
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

        return RegisterClassEx(&wc) != 0;
    }

    HWND CreateMainWindow() {
        HWND hwnd = CreateWindowEx(
            0,
            kWindowClassName,
            kAppTitle,
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

        return hwnd;
    }

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    g_instance = instance;

    g_single_instance_mutex = CreateMutex(nullptr, TRUE, kMutexName);
    if (!g_single_instance_mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_single_instance_mutex) {
            CloseHandle(g_single_instance_mutex);
        }
        return 0;
    }

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const bool hidden_launch = argv ? IsHiddenLaunch(argc, argv) : false;
    if (argv) {
        LocalFree(argv);
    }

    g_taskbar_created_message = RegisterWindowMessage(L"TaskbarCreated");

    if (!RegisterMainWindowClass()) {
        CloseHandle(g_single_instance_mutex);
        return 1;
    }

    g_main_window = CreateMainWindow();
    if (!g_main_window) {
        CloseHandle(g_single_instance_mutex);
        return 1;
    }

    if (!AddTrayIcon()) {
        DestroyWindow(g_main_window);
        CloseHandle(g_single_instance_mutex);
        return 1;
    }

    if (!hidden_launch) {
        ShowWindow(g_main_window, show_command);
        UpdateWindow(g_main_window);
    }

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_single_instance_mutex) {
        ReleaseMutex(g_single_instance_mutex);
        CloseHandle(g_single_instance_mutex);
        g_single_instance_mutex = nullptr;
    }

    return static_cast<int>(msg.wParam);
}