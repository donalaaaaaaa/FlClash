#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <ras.h>
#include <shellapi.h>
#include <winhttp.h>
#include <wininet.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kOpenCommand = 1001;
constexpr UINT kProxyCommand = 1002;
constexpr UINT kExitCommand = 1003;
constexpr wchar_t kWindowClass[] = L"FlClashTrayWindow";
constexpr wchar_t kMutexName[] = L"Local\\FlClashSystemTray";
constexpr wchar_t kRuntimeKey[] =
    L"SYSTEM\\CurrentControlSet\\Services\\FlClashHelperService\\Runtime";
constexpr DWORD kDefaultHelperPort = 47890;

struct State {
  uint16_t port = 0;
  bool desired_proxy = false;
  std::vector<std::wstring> bypass;
};

struct App {
  std::wstring flclash_path;
  std::wstring state_path;
  HWND window = nullptr;
  NOTIFYICONDATAW notify = {};
  HICON icon = nullptr;
};

std::string TrimAscii(std::string value) {
  while (!value.empty() && (value.back() == '\r' || value.back() == '\n' ||
                            value.back() == ' ' || value.back() == '\t')) {
    value.pop_back();
  }
  size_t first = 0;
  while (first < value.size() && (value[first] == ' ' || value[first] == '\t')) {
    ++first;
  }
  return value.substr(first);
}

bool ParseUnsigned(std::string text, uint32_t* value) {
  text = TrimAscii(std::move(text));
  if (text.empty()) return false;
  uint32_t parsed = 0;
  const char* begin = text.data();
  const char* end = begin + text.size();
  auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc() || result.ptr != end) return false;
  *value = parsed;
  return true;
}

bool Utf8ToWide(const std::string& input, std::wstring* output) {
  if (input.empty()) {
    output->clear();
    return true;
  }
  int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                  static_cast<int>(input.size()), nullptr, 0);
  if (length <= 0) return false;
  output->resize(length);
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                             static_cast<int>(input.size()), output->data(),
                             length) == length;
}

bool ReadState(const std::wstring& path, State* state) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(file, line)) lines.push_back(line);
  if (lines.size() < 3) return false;

  uint32_t port = 0;
  uint32_t desired = 0;
  uint32_t count = 0;
  if (!ParseUnsigned(lines[0], &port) ||
      port == 0 || port > 65535 ||
      !ParseUnsigned(lines[1], &desired) ||
      desired > 1 ||
      !ParseUnsigned(lines[2], &count) ||
      count > lines.size() - 3) {
    return false;
  }

  State parsed;
  parsed.port = static_cast<uint16_t>(port);
  parsed.desired_proxy = desired == 1;
  parsed.bypass.reserve(count + 1);
  for (uint32_t i = 0; i < count; ++i) {
    std::wstring domain;
    if (!Utf8ToWide(lines[3 + i], &domain)) return false;
    parsed.bypass.push_back(domain);
  }
  *state = std::move(parsed);
  return true;
}

bool WriteState(const std::wstring& path, const State& state) {
  const std::wstring temporary_path = path + L".tray.tmp";
  std::ofstream file(temporary_path, std::ios::binary | std::ios::trunc);
  if (!file) return false;
  file << state.port << '\n' << (state.desired_proxy ? '1' : '0') << '\n'
       << state.bypass.size() << '\n';
  for (const auto& domain : state.bypass) {
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, domain.data(),
                                     static_cast<int>(domain.size()), nullptr, 0,
                                     nullptr, nullptr);
    if (length < 0 || (length == 0 && !domain.empty())) return false;
    std::string encoded(static_cast<size_t>(length), '\0');
    if (length > 0 &&
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, domain.data(),
                            static_cast<int>(domain.size()), encoded.data(),
                            length, nullptr, nullptr) != length) {
      return false;
    }
    file.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    file.put('\n');
  }
  file.flush();
  if (!file) return false;
  file.close();
  return MoveFileExW(temporary_path.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

bool CurrentProxyPort(LPWSTR connection, uint16_t* port) {
  INTERNET_PER_CONN_OPTIONW options[3] = {};
  options[0].dwOption = INTERNET_PER_CONN_FLAGS;
  options[1].dwOption = INTERNET_PER_CONN_PROXY_SERVER;
  options[2].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
  INTERNET_PER_CONN_OPTION_LISTW list = {};
  list.dwSize = sizeof(list);
  list.pszConnection = connection;
  list.dwOptionCount = 3;
  list.pOptions = options;
  DWORD size = sizeof(list);
  if (!InternetQueryOptionW(nullptr, INTERNET_OPTION_PER_CONNECTION_OPTION, &list,
                            &size) ||
      !(options[0].Value.dwValue & PROXY_TYPE_PROXY) ||
      options[1].Value.pszValue == nullptr) {
    if (options[1].Value.pszValue) GlobalFree(options[1].Value.pszValue);
    if (options[2].Value.pszValue) GlobalFree(options[2].Value.pszValue);
    return false;
  }
  std::wstring proxy(options[1].Value.pszValue);
  GlobalFree(options[1].Value.pszValue);
  if (options[2].Value.pszValue) GlobalFree(options[2].Value.pszValue);
  size_t colon = proxy.rfind(L':');
  if (colon == std::wstring::npos || proxy.substr(0, colon) != L"127.0.0.1")
    return false;
  uint32_t parsed = 0;
  if (colon + 1 == proxy.size()) return false;
  for (size_t i = colon + 1; i < proxy.size(); ++i) {
    if (proxy[i] < L'0' || proxy[i] > L'9') return false;
    parsed = parsed * 10 + static_cast<uint32_t>(proxy[i] - L'0');
    if (parsed > 65535) return false;
  }
  if (parsed == 0) return false;
  *port = static_cast<uint16_t>(parsed);
  return true;
}

bool CurrentProxyPort(uint16_t* port) {
  return CurrentProxyPort(nullptr, port);
}

bool ForEachConnection(const std::function<bool(LPWSTR)>& visit) {
  bool success = visit(nullptr);
  DWORD size = 0;
  DWORD count = 0;
  auto result = RasEnumEntriesW(nullptr, nullptr, nullptr, &size, &count);
  if (result == ERROR_BUFFER_TOO_SMALL && count > 0) {
    std::vector<RASENTRYNAMEW> entries(count);
    for (auto& entry : entries) entry.dwSize = sizeof(RASENTRYNAMEW);
    result = RasEnumEntriesW(nullptr, nullptr, entries.data(), &size, &count);
    if (result == ERROR_SUCCESS) {
      for (DWORD i = 0; i < count; ++i) {
        success = visit(entries[i].szEntryName) && success;
      }
    } else {
      success = false;
    }
  } else if (result != ERROR_SUCCESS) {
    success = false;
  }
  return success;
}

bool SetProxy(const State& state, bool enabled) {
  const bool applied = ForEachConnection([&](LPWSTR connection) {
    INTERNET_PER_CONN_OPTIONW options[3] = {};
    INTERNET_PER_CONN_OPTION_LISTW list = {};
    list.dwSize = sizeof(list);
    list.pszConnection = connection;
    list.pOptions = options;
    if (!enabled) {
      uint16_t active_port = 0;
      if (!CurrentProxyPort(connection, &active_port) || active_port != state.port) {
        return true;
      }
      options[0].dwOption = INTERNET_PER_CONN_FLAGS;
      options[0].Value.dwValue = PROXY_TYPE_DIRECT;
      list.dwOptionCount = 1;
    } else {
      std::wstring bypass;
      for (const auto& item : state.bypass) {
        if (!bypass.empty()) bypass += L';';
        bypass += item;
      }
      if (!bypass.empty()) bypass += L';';
      bypass += L"<local>";
      std::wstring proxy = L"127.0.0.1:" + std::to_wstring(state.port);
      options[0].dwOption = INTERNET_PER_CONN_FLAGS;
      options[0].Value.dwValue = PROXY_TYPE_DIRECT | PROXY_TYPE_PROXY;
      options[1].dwOption = INTERNET_PER_CONN_PROXY_SERVER;
      options[1].Value.pszValue = proxy.data();
      options[2].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
      options[2].Value.pszValue = bypass.data();
      list.dwOptionCount = 3;
    }
    return InternetSetOptionW(nullptr, INTERNET_OPTION_PER_CONNECTION_OPTION,
                               &list, sizeof(list)) != FALSE;
  });
  if (!applied) return false;
  const bool changed =
      InternetSetOptionW(nullptr, INTERNET_OPTION_SETTINGS_CHANGED, nullptr, 0) != FALSE;
  const bool refreshed =
      InternetSetOptionW(nullptr, INTERNET_OPTION_REFRESH, nullptr, 0) != FALSE;
  return changed && refreshed;
}

bool HasProxyOnAnyConnection(uint16_t target_port) {
  bool found = false;
  ForEachConnection([&](LPWSTR connection) {
    uint16_t active_port = 0;
    if (CurrentProxyPort(connection, &active_port) && active_port == target_port) {
      found = true;
    }
    return true;
  });
  return found;
}

void OpenFlClash(const App& app) {
  ShellExecuteW(nullptr, L"open", app.flclash_path.c_str(), nullptr, nullptr,
                SW_SHOWNORMAL);
}

struct WindowSearch {
  const std::wstring* executable_path;
  HWND window = nullptr;
};

BOOL CALLBACK FindFlClashWindow(HWND window, LPARAM parameter) {
  auto* search = reinterpret_cast<WindowSearch*>(parameter);
  DWORD process_id = 0;
  GetWindowThreadProcessId(window, &process_id);
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
  if (!process) return TRUE;

  wchar_t image_path[MAX_PATH] = {};
  DWORD image_path_length = ARRAYSIZE(image_path);
  const bool queried = QueryFullProcessImageNameW(
      process, 0, image_path, &image_path_length) != FALSE;
  CloseHandle(process);
  if (queried && CompareStringOrdinal(
                     image_path, static_cast<int>(image_path_length),
                     search->executable_path->c_str(),
                     static_cast<int>(search->executable_path->size()), TRUE) ==
                     CSTR_EQUAL) {
    search->window = window;
    return FALSE;
  }
  return TRUE;
}

void CloseFlClashUi(const App& app) {
  WindowSearch search{&app.flclash_path};
  EnumWindows(FindFlClashWindow, reinterpret_cast<LPARAM>(&search));
  if (search.window) {
    DWORD_PTR result = 0;
    SendMessageTimeoutW(search.window, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 2000,
                        &result);
    Sleep(300);
  }
}

bool StopCurrentHelper() {
  HKEY key = nullptr;
  LSTATUS open_status =
      RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRuntimeKey, 0, KEY_READ, &key);
  if (open_status == ERROR_FILE_NOT_FOUND) return true;
  if (open_status != ERROR_SUCCESS && open_status != ERROR_ACCESS_DENIED) {
    return false;
  }
  DWORD port = kDefaultHelperPort;
  if (open_status == ERROR_SUCCESS) {
    DWORD published_port = 0;
    DWORD size = sizeof(published_port);
    DWORD type = 0;
    LSTATUS port_status = RegQueryValueExW(
        key, L"Port", nullptr, &type, reinterpret_cast<BYTE*>(&published_port),
        &size);
    RegCloseKey(key);
    if (port_status == ERROR_SUCCESS && type == REG_DWORD && published_port > 0 &&
        published_port <= 65535) {
      port = published_port;
    }
  }

  bool stopped = false;
  HINTERNET session = WinHttpOpen(
      L"FlClashTray/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return false;
  WinHttpSetTimeouts(session, 3000, 3000, 3000, 3000);
  HINTERNET connection =
      WinHttpConnect(session, L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0);
  if (connection) {
    HINTERNET request = WinHttpOpenRequest(
        connection, L"POST", L"/stop-current", nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (request) {
      if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
          WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0;
        DWORD status_size = sizeof(status);
        stopped = WinHttpQueryHeaders(
                      request,
                      WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                      WINHTTP_NO_HEADER_INDEX) &&
                  status == 200;
      }
      WinHttpCloseHandle(request);
    }
    WinHttpCloseHandle(connection);
  }
  WinHttpCloseHandle(session);
  return stopped;
}

void ShowMenu(App* app) {
  State state;
  const bool state_valid = ReadState(app->state_path, &state);
  HMENU menu = CreatePopupMenu();
  if (!menu) return;
  AppendMenuW(menu, MF_STRING, kOpenCommand, L"Open FlClash");
  bool checked = state_valid && HasProxyOnAnyConnection(state.port);
  AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : MF_UNCHECKED),
              kProxyCommand, L"System Proxy");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kExitCommand, L"Exit");
  POINT point;
  GetCursorPos(&point);
  SetForegroundWindow(app->window);
  UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                point.x, point.y, 0, app->window, nullptr);
  DestroyMenu(menu);
  if (command == kOpenCommand) {
    OpenFlClash(*app);
  } else if (command == kProxyCommand && state_valid) {
    // Re-read after the menu interaction so a concurrently updated state file
    // cannot cause the tray to apply stale port or bypass settings.
    State fresh;
    if (ReadState(app->state_path, &fresh)) {
      bool active = HasProxyOnAnyConnection(fresh.port);
      fresh.desired_proxy = !active;
      if (SetProxy(fresh, fresh.desired_proxy)) {
        if (!WriteState(app->state_path, fresh)) {
          MessageBoxW(app->window,
                      L"The proxy changed, but FlClash could not save the setting.",
                      L"FlClash", MB_OK | MB_ICONWARNING);
        }
      } else {
        MessageBoxW(app->window, L"FlClash could not change the system proxy.",
                    L"FlClash", MB_OK | MB_ICONWARNING);
      }
    }
  } else if (command == kExitCommand) {
    CloseFlClashUi(*app);
    if (!StopCurrentHelper()) {
      MessageBoxW(app->window,
                  L"FlClash could not stop the Core service. The tray remains running.",
                  L"FlClash", MB_OK | MB_ICONWARNING);
      return;
    }
    State fresh;
    if (ReadState(app->state_path, &fresh)) {
      if (HasProxyOnAnyConnection(fresh.port)) SetProxy(fresh, false);
    }
    PostQuitMessage(0);
  }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    return TRUE;
  }
  if (app && message == kTrayMessage &&
      (lparam == WM_RBUTTONUP || lparam == WM_LBUTTONUP)) {
    ShowMenu(app);
    return 0;
  }
  if (message == WM_DESTROY) PostQuitMessage(0);
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv || argc != 3) {
    if (argv) LocalFree(argv);
    return ERROR_INVALID_PARAMETER;
  }
  App app{argv[1], argv[2]};
  LocalFree(argv);

  SetLastError(ERROR_SUCCESS);
  HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
  DWORD mutex_error = GetLastError();
  if (!mutex || mutex_error == ERROR_ALREADY_EXISTS) {
    if (mutex) CloseHandle(mutex);
    return 0;
  }

  WNDCLASSW window_class = {};
  window_class.hInstance = instance;
  window_class.lpfnWndProc = WindowProc;
  window_class.lpszClassName = kWindowClass;
  RegisterClassW(&window_class);
  app.window = CreateWindowExW(0, kWindowClass, L"FlClash Tray", WS_OVERLAPPED,
                              0, 0, 0, 0, nullptr, nullptr, instance, &app);
  if (!app.window) {
    CloseHandle(mutex);
    return static_cast<int>(GetLastError());
  }

  app.icon = ExtractIconW(instance, app.flclash_path.c_str(), 0);
  if (!app.icon) app.icon = LoadIconW(nullptr, IDI_APPLICATION);
  app.notify.cbSize = sizeof(app.notify);
  app.notify.hWnd = app.window;
  app.notify.uID = 1;
  app.notify.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  app.notify.uCallbackMessage = kTrayMessage;
  app.notify.hIcon = app.icon;
  lstrcpynW(app.notify.szTip, L"FlClash", ARRAYSIZE(app.notify.szTip));
  Shell_NotifyIconW(NIM_ADD, &app.notify);

  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  Shell_NotifyIconW(NIM_DELETE, &app.notify);
  if (app.icon) DestroyIcon(app.icon);
  DestroyWindow(app.window);
  UnregisterClassW(kWindowClass, instance);
  CloseHandle(mutex);
  return 0;
}
