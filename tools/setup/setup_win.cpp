// Windows front end: one window with the three choices, Browse buttons, a
// progress bar and an Install button. Also works from a console with the
// options of setup_common.h.
#include "setup_common.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

namespace fs = std::filesystem;
using namespace rs::setup;

std::wstring Wide(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), nullptr, 0);
  std::wstring out(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), out.data(), n);
  return out;
}
std::string Narrow(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), int(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), int(wide.size()), out.data(), n, nullptr, nullptr);
  return out;
}

enum Id {
  kGameEdit = 101, kGameBrowse, kUpdateEdit, kUpdateBrowse, kAddonsEdit, kAddonsBrowse,
  kScanButton, kInstallButton, kCloseButton, kProgress, kStatus, kLog
};
constexpr UINT kMsgProgress = WM_APP + 1;  // wParam = percent, lParam = std::wstring* (owned)
constexpr UINT kMsgLog = WM_APP + 2;       // lParam = std::wstring* (owned)
constexpr UINT kMsgDone = WM_APP + 3;      // wParam = ok, lParam = std::wstring* error (owned)

struct Window {
  HWND hwnd = nullptr;
  HWND game = nullptr, update = nullptr, addons = nullptr, install = nullptr, progress = nullptr,
       status = nullptr, log = nullptr, scan = nullptr, close = nullptr;
  HWND browse[3] = {};
  HFONT font = nullptr;
  bool busy = false;
  Args args;
  std::thread worker;
};

std::wstring Text(HWND edit) {
  const int n = GetWindowTextLengthW(edit);
  std::wstring out(size_t(n), L'\0');
  GetWindowTextW(edit, out.data(), n + 1);
  return out;
}

std::wstring PickPath(HWND owner, const wchar_t* title, bool folder) {
  std::wstring result;
  IFileOpenDialog* dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
    return result;
  }
  DWORD options = 0;
  dialog->GetOptions(&options);
  options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST;
  if (folder) options |= FOS_PICKFOLDERS;
  dialog->SetOptions(options);
  dialog->SetTitle(title);
  if (!folder) {
    const COMDLG_FILTERSPEC filters[] = {{L"All files (Xbox 360 packages have no extension)", L"*"}};
    dialog->SetFileTypes(1, filters);
  }
  if (SUCCEEDED(dialog->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        result = path;
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dialog->Release();
  return result;
}

void AppendLog(Window& w, const std::wstring& line) {
  const int len = GetWindowTextLengthW(w.log);
  SendMessageW(w.log, EM_SETSEL, len, len);
  const std::wstring text = line + L"\r\n";
  SendMessageW(w.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
}

void SetBusy(Window& w, bool busy) {
  w.busy = busy;
  for (HWND h : {w.game, w.update, w.addons, w.install, w.scan, w.browse[0], w.browse[1], w.browse[2]}) {
    EnableWindow(h, !busy);
  }
  SetWindowTextW(w.close, busy ? L"Cancel" : L"Close");
}

void StartInstall(Window& w) {
  Plan plan;
  plan.game = fs::path(Text(w.game));
  plan.update = fs::path(Text(w.update));
  const std::wstring addons = Text(w.addons);
  if (!addons.empty()) plan.addons = AddonsIn(fs::path(addons));
  plan.dest = w.args.dest.empty() ? ProgramDir() : w.args.dest;
  plan.user_root = w.args.user_root;
  if (plan.game.empty()) {
    MessageBoxW(w.hwnd, L"Choose the game package first (Browse... next to \"Game package\").",
                L"ReCompiled Steel setup", MB_OK | MB_ICONINFORMATION);
    return;
  }
  SetBusy(w, true);
  SetWindowTextW(w.log, L"");
  SendMessageW(w.progress, PBM_SETPOS, 0, 0);
  SetWindowTextW(w.status, L"Starting...");
  HWND hwnd = w.hwnd;
  if (w.worker.joinable()) w.worker.join();
  w.worker = std::thread([hwnd, plan] {
    std::string error;
    const bool ok = Run(
        plan,
        [hwnd](const std::string& line) {
          PostMessageW(hwnd, kMsgLog, 0, reinterpret_cast<LPARAM>(new std::wstring(Wide(line))));
        },
        [hwnd](double fraction, const std::string& what) {
          PostMessageW(hwnd, kMsgProgress, WPARAM(fraction * 100.0),
                       reinterpret_cast<LPARAM>(new std::wstring(Wide(what))));
        },
        &error);
    PostMessageW(hwnd, kMsgDone, ok ? 1 : 0, reinterpret_cast<LPARAM>(new std::wstring(Wide(error))));
  });
}

void ScanInto(Window& w, const std::wstring& folder) {
  SetWindowTextW(w.status, L"Looking through the folder...");
  UpdateWindow(w.status);
  const Scan found = ScanFolder(fs::path(folder));
  if (!found.game.empty()) SetWindowTextW(w.game, found.game.c_str());
  if (!found.update.empty()) SetWindowTextW(w.update, found.update.c_str());
  if (!found.addons.empty()) SetWindowTextW(w.addons, found.addons.front().parent_path().c_str());
  wchar_t text[256];
  std::swprintf(text, 256, L"Found: %s, %s, %zu add-on package(s)%s.",
                found.game.empty() ? L"no game package" : L"the game",
                found.update.empty() ? L"no title update" : L"the title update", found.addons.size(),
                found.other_titles ? L" (packages of other games ignored)" : L"");
  SetWindowTextW(w.status, text);
  if (found.game.empty()) {
    MessageBoxW(w.hwnd,
                L"No Real Steel game package in that folder.\n\nOn the Xbox 360 it is the file under "
                L"Content\\0000000000000000\\584111E0\\000D0000\\ (about 200 MB). Copy that folder "
                L"structure from the console's storage to this PC and pick it here.",
                L"ReCompiled Steel setup", MB_OK | MB_ICONINFORMATION);
  }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  Window* w = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_COMMAND: {
      if (w == nullptr) break;
      const int id = LOWORD(wp);
      if (id == kGameBrowse) {
        const std::wstring p = PickPath(hwnd, L"The game package (Content\\...\\584111E0\\000D0000\\...)", false);
        if (!p.empty()) SetWindowTextW(w->game, p.c_str());
      } else if (id == kUpdateBrowse) {
        const std::wstring p = PickPath(hwnd, L"The title update package (Content\\...\\584111E0\\000B0000\\...)", false);
        if (!p.empty()) SetWindowTextW(w->update, p.c_str());
      } else if (id == kAddonsBrowse) {
        const std::wstring p = PickPath(hwnd, L"The folder with the add-on packages (Content\\...\\584111E0\\00000002)", true);
        if (!p.empty()) SetWindowTextW(w->addons, p.c_str());
      } else if (id == kScanButton) {
        const std::wstring p = PickPath(hwnd, L"Folder holding your Real Steel files (everything in it is found)", true);
        if (!p.empty()) ScanInto(*w, p);
      } else if (id == kInstallButton) {
        if (!w->busy) StartInstall(*w);
      } else if (id == kCloseButton) {
        if (!w->busy) DestroyWindow(hwnd);
      }
      return 0;
    }
    case kMsgProgress: {
      auto* text = reinterpret_cast<std::wstring*>(lp);
      if (w != nullptr) {
        SendMessageW(w->progress, PBM_SETPOS, wp, 0);
        SetWindowTextW(w->status, (*text + L" (" + std::to_wstring(int(wp)) + L"%)").c_str());
      }
      delete text;
      return 0;
    }
    case kMsgLog: {
      auto* text = reinterpret_cast<std::wstring*>(lp);
      if (w != nullptr) AppendLog(*w, *text);
      delete text;
      return 0;
    }
    case kMsgDone: {
      auto* error = reinterpret_cast<std::wstring*>(lp);
      if (w != nullptr) {
        SetBusy(*w, false);
        if (wp != 0) {
          SendMessageW(w->progress, PBM_SETPOS, 100, 0);
          SetWindowTextW(w->status, L"Done. You can close this window and start realsteel.exe.");
          MessageBoxW(hwnd, L"Your game files are installed.\n\nStart the game with realsteel.exe in this folder.",
                      L"ReCompiled Steel setup", MB_OK | MB_ICONINFORMATION);
        } else {
          SetWindowTextW(w->status, L"Setup failed.");
          AppendLog(*w, L"FAILED: " + *error);
          MessageBoxW(hwnd, (L"Setup failed:\n\n" + *error).c_str(), L"ReCompiled Steel setup", MB_OK | MB_ICONERROR);
        }
      }
      delete error;
      return 0;
    }
    case WM_CLOSE:
      if (w != nullptr && w->busy) return 0;
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND Make(Window& w, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int cx, int cy, int id,
          DWORD ex = 0) {
  HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, cx, cy, w.hwnd,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
  SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(w.font), TRUE);
  return h;
}

int RunWindow(const Args& args) {
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  WNDCLASSW wc{};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  wc.lpszClassName = L"RealSteelSetup";
  wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  RegisterClassW(&wc);

  Window w;
  w.args = args;
  NONCLIENTMETRICSW ncm{sizeof(ncm)};
  SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
  w.font = CreateFontIndirectW(&ncm.lfMessageFont);

  const int width = 640, height = 470;
  RECT r{0, 0, width, height};
  AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
  w.hwnd = CreateWindowExW(0, wc.lpszClassName, L"ReCompiled Steel setup",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
  SetWindowLongPtrW(w.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&w));

  const int m = 16;
  int y = m;
  Make(w, L"STATIC",
       L"This copies your own Real Steel files from your Xbox 360 into this folder. On the console "
       L"they are under Content\\0000000000000000\\584111E0\\ (000D0000 = the game, 000B0000 = the "
       L"title update, 00000002 = add-ons). Copy that folder to this PC, then either pick it with "
       L"\"Find in a folder...\" or choose the files one by one.",
       0, m, y, width - 2 * m, 64, 0);
  y += 72;
  w.scan = Make(w, L"BUTTON", L"Find in a folder...", BS_PUSHBUTTON | WS_TABSTOP, m, y, 160, 26, kScanButton);
  y += 40;
  const struct { const wchar_t* label; HWND* edit; HWND* browse; int edit_id, browse_id; } rows[] = {
      {L"Game package:", &w.game, &w.browse[0], kGameEdit, kGameBrowse},
      {L"Title update:", &w.update, &w.browse[1], kUpdateEdit, kUpdateBrowse},
      {L"Add-on folder:", &w.addons, &w.browse[2], kAddonsEdit, kAddonsBrowse},
  };
  for (const auto& row : rows) {
    Make(w, L"STATIC", row.label, 0, m, y + 4, 100, 20, 0);
    *row.edit = Make(w, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER, m + 104, y, width - 2 * m - 104 - 96, 24,
                     row.edit_id);
    *row.browse = Make(w, L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, width - m - 88, y - 1, 88, 26, row.browse_id);
    y += 34;
  }
  Make(w, L"STATIC",
       (L"Add-ons go to " + DefaultUserRoot().wstring() + L" (where the game keeps saves). The title update and add-ons are optional.")
           .c_str(),
       0, m, y, width - 2 * m, 34, 0);
  y += 40;
  w.progress = Make(w, PROGRESS_CLASSW, L"", 0, m, y, width - 2 * m, 18, kProgress);
  SendMessageW(w.progress, PBM_SETRANGE32, 0, 100);
  y += 24;
  w.status = Make(w, L"STATIC", L"", 0, m, y, width - 2 * m, 20, kStatus);
  y += 24;
  w.log = Make(w, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, m, y,
               width - 2 * m, 90, kLog);
  y += 100;
  w.install = Make(w, L"BUTTON", L"Install", BS_DEFPUSHBUTTON | WS_TABSTOP, width - m - 200, y, 96, 28, kInstallButton);
  w.close = Make(w, L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, width - m - 96, y, 96, 28, kCloseButton);

  if (!args.game.empty()) SetWindowTextW(w.game, args.game.c_str());
  if (!args.update.empty()) SetWindowTextW(w.update, args.update.c_str());
  if (!args.addons.empty()) SetWindowTextW(w.addons, args.addons.c_str());
  if (!args.scan.empty()) ScanInto(w, args.scan.wstring());

  ShowWindow(w.hwnd, SW_SHOW);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(w.hwnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  if (w.worker.joinable()) w.worker.join();
  CoUninitialize();
  return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::string> args_text;
  for (int i = 0; i < argc; ++i) args_text.push_back(Narrow(argv[i]));
  LocalFree(argv);
  const Args args = ParseArgs(args_text);

  // From a console: the options drive it, output goes there.
  const bool console_mode = args.help || !args.error.empty() || args.cli || !args.game.empty() || !args.scan.empty();
  if (console_mode && AttachConsole(ATTACH_PARENT_PROCESS)) {
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONIN$", "r", stdin);
    std::printf("\n");
  }
  if (args.help || !args.error.empty()) {
    if (!args.error.empty()) std::printf("%s\n\n", args.error.c_str());
    std::printf("%s", Usage().c_str());
    return args.help ? 0 : 2;
  }
  if (args.cli && args.game.empty() && args.scan.empty()) {
    Args asked = args;
    if (!AskOnTerminal(&asked)) return 1;
    Plan plan;
    std::string error;
    if (!PlanFromArgs(asked, &plan, &error)) {
      std::printf("%s\n", error.c_str());
      return 1;
    }
    return RunConsole(plan);
  }
  if (!args.game.empty() || !args.scan.empty()) {
    if (!args.yes && !args.cli) return RunWindow(args);  // choices filled in, the window confirms
    Plan plan;
    std::string error;
    if (!PlanFromArgs(args, &plan, &error)) {
      std::printf("%s\n", error.c_str());
      return 1;
    }
    return RunConsole(plan);
  }
  return RunWindow(args);
}
