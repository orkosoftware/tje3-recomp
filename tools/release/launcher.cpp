// TJE3.exe: starts the game, or sets it up the first time.
//
// Setup asks for the player's own disc image or extracted folder, any
// downloadable content folders and an install folder, and runs setup/setup.py
// with the Python in the release. Nothing from the game is in the release.
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <string>
#include <vector>

namespace {

enum { ID_ISO = 101, ID_FOLDER, ID_ADD_DLC, ID_CLEAR_DLC, ID_BROWSE, ID_INSTALL, ID_TERMS, WM_LINE = WM_APP + 1, WM_FINISHED };
enum Task { TASK_NONE, TASK_SOURCE, TASK_DLC, TASK_INSTALL };

const wchar_t kTitle[] = L"ToeJam & Earl III setup";
const wchar_t kFolderName[] = L"ToeJam & Earl III";

HWND g_win, g_source, g_source_status, g_dlc, g_dlc_status, g_dest, g_install, g_bar, g_status, g_log;
HWND g_iso_btn, g_folder_btn, g_add_btn, g_clear_btn, g_browse_btn, g_shortcut, g_android;
std::wstring g_dir, g_game, g_source_path;
std::vector<std::wstring> g_dlc_paths;
Task g_task = TASK_NONE;
bool g_source_ok = false, g_dlc_ok = true, g_ready = false;
HFONT g_font;

std::wstring exe_dir() {
    wchar_t p[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, p, MAX_PATH * 2);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L'\\'));
}

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring location_file() { return g_dir + L"\\install_location.txt"; }

// Where setup put the game: remembered beside this program.
void load_location() {
    g_game = g_dir + L"\\game";
    HANDLE f = CreateFileW(location_file().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    wchar_t buf[MAX_PATH * 2];
    DWORD n = 0;
    if (ReadFile(f, buf, sizeof buf, &n, nullptr) && n >= sizeof(wchar_t)) g_game.assign(buf, n / sizeof(wchar_t));
    CloseHandle(f);
}

void save_location() {
    HANDLE f = CreateFileW(location_file().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n;
    WriteFile(f, g_game.data(), (DWORD)(g_game.size() * sizeof(wchar_t)), &n, nullptr);
    CloseHandle(f);
}

// A desktop shortcut straight to the game, with the icon setup made from the disc.
void create_shortcut() {
    wchar_t desktop[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop))) return;
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&link))) return;
    link->SetPath((g_game + L"\\tje3.exe").c_str());
    link->SetWorkingDirectory(g_game.c_str());
    link->SetDescription(kFolderName);
    if (exists(g_game + L"\\tje3.ico")) link->SetIconLocation((g_game + L"\\tje3.ico").c_str(), 0);
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&file))) {
        file->Save((std::wstring(desktop) + L"\\" + kFolderName + L".lnk").c_str(), TRUE);
        file->Release();
    }
    link->Release();
}

std::wstring quoted(std::wstring p) {
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    if (!p.empty() && p.back() == L'\\') p += L'\\';    // "E:\\" so the quote stays a quote
    return L"\"" + p + L"\"";
}

bool play(const wchar_t* args) {
    std::wstring exe = g_game + L"\\tje3.exe", cmd = quoted(exe) + L" " + args;
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, g_game.c_str(), &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void update_controls() {
    bool idle = g_task == TASK_NONE;
    EnableWindow(g_install, g_ready || (idle && g_source_ok && g_dlc_ok));
    for (HWND h : {g_iso_btn, g_folder_btn, g_add_btn, g_clear_btn, g_browse_btn}) EnableWindow(h, idle && !g_ready);
    SetWindowTextW(g_install, g_ready ? L"Play" : L"Install");
}

std::wstring pick_folder(const wchar_t* title) {
    BROWSEINFOW bi{};
    bi.hwndOwner = g_win;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    wchar_t path[MAX_PATH * 2] = L"";
    if (PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi)) {
        SHGetPathFromIDListW(id, path);
        CoTaskMemFree(id);
    }
    return path;
}

std::wstring pick_iso() {
    wchar_t path[MAX_PATH * 2] = L"";
    OPENFILENAMEW o{sizeof o};
    o.hwndOwner = g_win;
    o.lpstrFilter = L"Xbox disc image (*.iso)\0*.iso\0All files\0*.*\0";
    o.lpstrFile = path;
    o.nMaxFile = MAX_PATH * 2;
    o.lpstrTitle = L"Choose your disc image";
    o.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    GetOpenFileNameW(&o);
    return path;
}

void log_line(const std::wstring& line) {
    std::wstring t = line + L"\r\n";
    int n = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, n, n);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)t.c_str());
}

void on_line(const std::wstring& line) {
    if (line.rfind(L"@P ", 0) == 0) {
        wchar_t* end = nullptr;
        long percent = wcstol(line.c_str() + 3, &end, 10);
        SendMessageW(g_bar, PBM_SETPOS, percent, 0);
        if (end && *end) SetWindowTextW(g_status, end + 1);
    } else if (line.rfind(L"@R ", 0) == 0) {
        bool ok = line.size() > 3 && line[3] == L'1';
        const wchar_t* text = line.size() > 5 ? line.c_str() + 5 : L"";
        if (g_task == TASK_SOURCE) { g_source_ok = ok; SetWindowTextW(g_source_status, text); }
        if (g_task == TASK_DLC) { g_dlc_ok = ok; SetWindowTextW(g_dlc_status, text); }
    } else {
        log_line(line);
    }
}

// Runs setup.py and sends its output to the window, one line per message.
DWORD WINAPI worker(void* param) {
    std::wstring* cmd = (std::wstring*)param;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd, wr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring dir = g_dir + L"\\setup";
    BOOL ok = CreateProcessW(nullptr, cmd->data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi);
    delete cmd;
    CloseHandle(wr);
    DWORD code = 1;
    if (ok) {
        std::string buf;
        char chunk[4096];
        DWORD n;
        while (ReadFile(rd, chunk, sizeof chunk, &n, nullptr) && n) {
            buf.append(chunk, n);
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                int w = MultiByteToWideChar(CP_UTF8, 0, line.data(), (int)line.size(), nullptr, 0);
                auto* text = new std::wstring(w, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, line.data(), (int)line.size(), text->data(), w);
                PostMessageW(g_win, WM_LINE, 0, (LPARAM)text);
            }
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(rd);
    PostMessageW(g_win, WM_FINISHED, code, ok);
    return 0;
}

void run_setup(Task task, const std::wstring& args) {
    auto* cmd = new std::wstring(quoted(g_dir + L"\\setup\\python\\python.exe") + L" -X utf8 -u " +
                                 quoted(g_dir + L"\\setup\\setup.py") + L" " + args);
    g_task = task;
    update_controls();
    CreateThread(nullptr, 0, worker, cmd, 0, nullptr);
}

void check_source(const std::wstring& path) {
    g_source_path = path;
    g_source_ok = false;
    SetWindowTextW(g_source, path.c_str());
    SetWindowTextW(g_source_status, L"Checking...");
    run_setup(TASK_SOURCE, L"--check-source " + quoted(path));
}

void check_dlc() {
    SendMessageW(g_dlc, LB_RESETCONTENT, 0, 0);
    for (auto& p : g_dlc_paths) SendMessageW(g_dlc, LB_ADDSTRING, 0, (LPARAM)p.c_str());
    g_dlc_ok = true;
    if (g_dlc_paths.empty()) {
        SetWindowTextW(g_dlc_status, L"");
        update_controls();
        return;
    }
    std::wstring args;
    for (auto& d : g_dlc_paths) args += L"--check-dlc " + quoted(d) + L" ";
    SetWindowTextW(g_dlc_status, L"Checking...");
    run_setup(TASK_DLC, args);
}

void start_install() {
    std::wstring args = L"--source " + quoted(g_source_path) + L" --dest " + quoted(g_game);
    for (auto& d : g_dlc_paths) args += L" --dlc " + quoted(d);
    if (SendMessageW(g_android, BM_GETCHECK, 0, 0) == BST_CHECKED) args += L" --android";
    SendMessageW(g_bar, PBM_SETPOS, 0, 0);
    SetWindowTextW(g_log, L"");
    run_setup(TASK_INSTALL, args);
}

// The game gets a folder of its own inside the one chosen, so that an install
// can never mix with whatever else is there.
void choose_destination() {
    std::wstring p = pick_folder(L"Choose where to install the game");
    if (p.empty()) return;
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    size_t slash = p.find_last_of(L'\\');
    std::wstring last = slash == std::wstring::npos ? p : p.substr(slash + 1);
    bool ours = last == kFolderName || exists(p + L"\\tje3.exe");
    if (!ours) p += (p.back() == L'\\' ? L"" : L"\\") + std::wstring(kFolderName);
    g_game = p;
    SetWindowTextW(g_dest, p.c_str());
}

HWND add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_win, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

void build_ui() {
    const DWORD tab = WS_TABSTOP;
    add(L"STATIC", L"This release contains no part of the game. It builds the game on this PC from your own copy.",
        0, 16, 14, 610, 20);
    add(L"STATIC", L"Your copy of the game (USA Xbox disc):", 0, 16, 44, 400, 20);
    g_source = add(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL, 16, 66, 610, 24, 0, WS_EX_CLIENTEDGE);
    g_iso_btn = add(L"BUTTON", L"Choose disc image (.iso)...", tab, 16, 98, 200, 30, ID_ISO);
    g_folder_btn = add(L"BUTTON", L"Choose extracted folder...", tab, 226, 98, 200, 30, ID_FOLDER);
    g_source_status = add(L"STATIC", L"", 0, 16, 134, 610, 20);

    add(L"STATIC", L"Downloadable content folders (optional):", 0, 16, 164, 400, 20);
    g_dlc = add(L"LISTBOX", L"", WS_VSCROLL | LBS_NOSEL, 16, 186, 610, 62, 0, WS_EX_CLIENTEDGE);
    g_add_btn = add(L"BUTTON", L"Add DLC folder...", tab, 16, 254, 200, 30, ID_ADD_DLC);
    g_clear_btn = add(L"BUTTON", L"Clear list", tab, 226, 254, 120, 30, ID_CLEAR_DLC);
    g_dlc_status = add(L"STATIC", L"", 0, 16, 290, 610, 36);

    add(L"STATIC", L"Install to:", 0, 16, 332, 400, 20);
    g_dest = add(L"EDIT", g_game.c_str(), ES_READONLY | ES_AUTOHSCROLL, 16, 354, 480, 24, 0, WS_EX_CLIENTEDGE);
    g_browse_btn = add(L"BUTTON", L"Browse...", tab, 506, 351, 120, 30, ID_BROWSE);

    g_status = add(L"STATIC", L"", 0, 16, 392, 610, 20);
    g_bar = add(PROGRESS_CLASSW, L"", PBS_SMOOTH, 16, 414, 610, 20);
    SendMessageW(g_bar, PBM_SETRANGE32, 0, 100);
    g_log = add(L"EDIT", L"", ES_READONLY | ES_MULTILINE | WS_VSCROLL | ES_AUTOVSCROLL, 16, 442, 610, 84, 0, WS_EX_CLIENTEDGE);
    g_shortcut = add(L"BUTTON", L"Create a desktop shortcut", tab | BS_AUTOCHECKBOX, 16, 542, 300, 22);
    SendMessageW(g_shortcut, BM_SETCHECK, BST_CHECKED, 0);
    g_android = add(L"BUTTON", L"Also build an Android app for my own device. Downloads Google's Android NDK (about 750 MB). I accept Google's terms.",
                    tab | BS_AUTOCHECKBOX | BS_MULTILINE, 16, 578, 470, 40);
    add(L"BUTTON", L"View Google's terms", tab, 506, 584, 120, 30, ID_TERMS);
    g_install = add(L"BUTTON", L"Install", tab | BS_DEFPUSHBUTTON, 506, 536, 120, 34, ID_INSTALL);
    update_controls();
}

LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_ISO: {
            std::wstring p = pick_iso();
            if (!p.empty()) check_source(p);
            break;
        }
        case ID_FOLDER: {
            std::wstring p = pick_folder(L"Choose the folder that holds default.xbe");
            if (!p.empty()) check_source(p);
            break;
        }
        case ID_ADD_DLC: {
            std::wstring p = pick_folder(L"Choose a DLC folder, or the folder that holds several");
            if (!p.empty()) { g_dlc_paths.push_back(p); check_dlc(); }
            break;
        }
        case ID_CLEAR_DLC: g_dlc_paths.clear(); check_dlc(); break;
        case ID_BROWSE: choose_destination(); break;
        case ID_TERMS:
            ShellExecuteW(h, L"open", L"https://developer.android.com/studio/terms", nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case ID_INSTALL:
            if (g_ready) { if (play(L"")) DestroyWindow(h); }
            else start_install();
            break;
        }
        return 0;
    case WM_CTLCOLORSTATIC: {
        HWND c = (HWND)lp;
        if (c != g_source_status && c != g_dlc_status) break;
        wchar_t text[16] = L"";
        GetWindowTextW(c, text, 16);
        bool ok = c == g_source_status ? g_source_ok : g_dlc_ok;
        bool checking = !wcsncmp(text, L"Checking", 8);
        SetTextColor((HDC)wp, checking ? RGB(90, 90, 90) : ok ? RGB(0, 120, 0) : RGB(190, 0, 0));
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    case WM_LINE: {
        auto* text = (std::wstring*)lp;
        on_line(*text);
        delete text;
        return 0;
    }
    case WM_FINISHED: {
        Task done = g_task;
        g_task = TASK_NONE;
        if (done == TASK_INSTALL) {
            g_ready = wp == 0 && lp && exists(g_game + L"\\tje3.exe");
            if (g_ready) {
                save_location();
                if (SendMessageW(g_shortcut, BM_GETCHECK, 0, 0) == BST_CHECKED) create_shortcut();
            }
            SetWindowTextW(g_status, g_ready ? L"Ready to play." : L"Setup did not finish. The reason is below.");
        } else if (!lp) {
            SetWindowTextW(done == TASK_SOURCE ? g_source_status : g_dlc_status, L"The check could not run.");
        }
        InvalidateRect(g_source_status, nullptr, TRUE);
        InvalidateRect(g_dlc_status, nullptr, TRUE);
        update_controls();
        return 0;
    }
    case WM_CLOSE:
        if (g_task == TASK_INSTALL && MessageBoxW(h, L"Setup is still running. Close anyway?", kTitle,
                                                  MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

#include "launcher_home.inc"

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR args, int show) {
    g_dir = exe_dir();
    load_location();
    // The game is compiled on this PC, so a newer release has to compile it again. The game
    // files from the first setup are still there: no disc is needed.
    auto small_file = [](const std::wstring& path) {
        std::string text;
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return text;
        char buf[64];
        DWORD n = 0;
        if (ReadFile(f, buf, sizeof buf, &n, nullptr)) text.assign(buf, n);
        CloseHandle(f);
        return text;
    };
    const std::string kit = small_file(g_dir + L"\\setup\\version.txt");
    const bool update = !kit.empty() && exists(g_game + L"\\data\\default.xbe") && !wcsstr(args, L"--setup") &&
                        small_file(g_game + L"\\kit_version.txt") != kit;
    if (!update && exists(g_game + L"\\tje3.exe") && !wcsstr(args, L"--setup")) {
        // Straight to the game if the player asked for that; otherwise Play / Settings first.
        const bool straight = exists(skip_path()) && !(GetKeyState(VK_SHIFT) & 0x8000) && !wcsstr(args, L"--settings");
        if (!straight && !run_home(inst, show)) return 0;
        if (play(straight ? args : L"")) return 0;
    }
    if (!exists(g_dir + L"\\setup\\setup.py")) {
        MessageBoxW(nullptr, L"The setup folder is missing. Unpack the whole release into one folder.", kTitle, MB_ICONERROR);
        return 1;
    }
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX cc{sizeof cc, ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&cc);
    NONCLIENTMETRICSW nm{sizeof nm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof nm, &nm, 0);
    g_font = CreateFontIndirectW(&nm.lfMessageFont);

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"tje3setup";
    RegisterClassW(&wc);
    RECT r{0, 0, 642, 630};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    g_win = CreateWindowExW(0, wc.lpszClassName, kTitle, style, CW_USEDEFAULT, CW_USEDEFAULT,
                            r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    build_ui();
    if (update) {                    // a newer release over an existing install: no questions needed
        SetWindowTextW(g_source, L"Using the game files already installed");
        SetWindowTextW(g_status, L"Updating the game to this version...");
        run_setup(TASK_INSTALL, L"--rebuild --dest " + quoted(g_game));
    }
    ShowWindow(g_win, show);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(g_win, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}
