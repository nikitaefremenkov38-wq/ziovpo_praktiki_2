#pragma once

#include <windows.h>

namespace witcher {

inline constexpr wchar_t kServiceName[] = L"WitcherTrayService";
inline constexpr wchar_t kServiceDisplayName[] = L"Witcher Tray Service";
inline constexpr wchar_t kTrayAppExeName[] = L"TrayWin32App.exe";
inline constexpr wchar_t kServiceExeName[] = L"WitcherTrayService.exe";
inline constexpr wchar_t kRpcEndpoint[] = L"WitcherTrayServiceRpc";

} // namespace witcher
