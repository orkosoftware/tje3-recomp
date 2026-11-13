// Part of launcher.cpp: the window shown once the game is installed (Play,
// Settings, set up again), in the game's colours rather than plain Windows grey.
// Settings edits the same tje3.ini the game's own Display Settings page writes.

enum { ID_PLAY = 201, ID_SETTINGS, ID_SETUP_AGAIN, ID_SKIP, ID_SAVE, ID_CANCEL, ID_COMBO = 300 };

const COLORREF kBack = RGB(22, 18, 40), kPanel = RGB(52, 44, 92), kInk = RGB(244, 240, 255),
               kDim = RGB(176, 168, 206), kGold = RGB(255, 184, 28), kGoldInk = RGB(40, 24, 0);
const wchar_t kGameName[] = L"ToeJam & Earl III";

struct Pick { int value; const wchar_t* name; };
struct Opt {
    const char* key;
    const wchar_t* title;
    std::vector<Pick> picks;
    int fallback;
};
const Opt kOpts[] = {
    {"display_mode", L"Display", {{0, L"Original 4:3"}, {1, L"Widescreen"}, {2, L"Stretch"}, {3, L"Sharp Pixels"}}, 0},
    {"resolution", L"Resolution", {{0, L"Match Screen"}, {1, L"Original (640 x 480)"}, {2, L"2x"}, {3, L"3x"}, {4, L"4x"}}, 0},
    {"smoothing", L"Smoothing", {{1, L"On"}, {0, L"Off"}}, 1},
    {"vsync", L"V-Sync", {{1, L"On"}, {0, L"Off"}}, 0},
    {"fullscreen", L"Fullscreen", {{1, L"On"}, {0, L"Off"}}, 1},
    {"master_volume", L"Volume", {{100, L"100%"}, {75, L"75%"}, {50, L"50%"}, {25, L"25%"}, {0, L"Off"}}, 100},
    {"invert_camera_x", L"Camera Turn", {{1, L"Modern"}, {0, L"Original"}}, 1},
    {"invert_look_y", L"Look Up/Down", {{1, L"Modern"}, {0, L"Original (flight style)"}}, 1},
    {"shadow_enhance", L"Shadow Overlap", {{0, L"Original"}, {20, L"Light"}, {35, L"Medium"}, {50, L"Strong"}}, 0},
    {"boogeymen_stay_converted", L"Boogeymen", {{0, L"Come back (original)"}, {1, L"Stay converted"}}, 0},
};
const int kOptCount = (int)(sizeof kOpts / sizeof kOpts[0]);

HBRUSH g_back_brush, g_panel_brush;
HFONT g_title_font, g_button_font;
HICON g_game_icon;
HWND g_home, g_settings, g_combos[16];
bool g_start_game = false;

std::wstring ini_path() { return g_game + L"\\tje3.ini"; }
std::wstring skip_path() { return g_dir + L"\\skip_launcher.txt"; }

std::vector<std::string> read_lines(const std::wstring& path) {
    std::vector<std::string> lines;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return lines;
    std::string all;
    char buf[4096];
    DWORD n;
    while (ReadFile(f, buf, sizeof buf, &n, nullptr) && n) all.append(buf, n);
    CloseHandle(f);
    size_t at = 0;
    while (at < all.size()) {
        size_t end = all.find('\n', at);
        if (end == std::string::npos) end = all.size();
        std::string line = all.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        at = end + 1;
    }
    return lines;
}

// "key = value" on a line that isn't a comment; empty key otherwise.
std::string line_key(const std::string& line, std::string* value) {
    size_t a = line.find_first_not_of(" \t");
    if (a == std::string::npos || line[a] == '#' || line[a] == ';') return "";
    size_t eq = line.find('=');
    if (eq == std::string::npos) return "";
    std::string key = line.substr(a, eq - a);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
    size_t v = line.find_first_not_of(" \t", eq + 1);
    *value = v == std::string::npos ? "" : line.substr(v);
    return key;
}

int ini_value(const std::vector<std::string>& lines, const Opt& o) {
    for (const std::string& line : lines) {
        std::string value;
        if (line_key(line, &value) == o.key) return atoi(value.c_str());
    }
    return o.fallback;
}

void save_settings() {
    std::vector<std::string> lines = read_lines(ini_path());
    for (int i = 0; i < kOptCount; i++) {
        const Opt& o = kOpts[i];
        int sel = (int)SendMessageW(g_combos[i], CB_GETCURSEL, 0, 0);
        if (sel < 0 || sel >= (int)o.picks.size()) continue;
        std::string text = std::string(o.key) + " = " + std::to_string(o.picks[sel].value);
        bool found = false;
        for (std::string& line : lines) {
            std::string value;
            if (line_key(line, &value) == o.key) { line = text; found = true; }
        }
        if (!found) lines.push_back(text);
    }
    std::string all;
    for (const std::string& line : lines) all += line + "\n";
    HANDLE f = CreateFileW(ini_path().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        MessageBoxW(g_settings, L"The settings could not be saved.", kGameName, MB_ICONWARNING);
        return;
    }
    DWORD n;
    WriteFile(f, all.data(), (DWORD)all.size(), &n, nullptr);
    CloseHandle(f);
}

void dark_title_bar(HWND h) {
    BOOL on = TRUE;
    DwmSetWindowAttribute(h, 20, &on, sizeof on);        // DWMWA_USE_IMMERSIVE_DARK_MODE
}

HWND control(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT font) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

// Flat buttons: gold for the main action, panel colour for the rest.
void draw_button(const DRAWITEMSTRUCT* d) {
    const bool main = d->CtlID == ID_PLAY || d->CtlID == ID_SAVE, down = d->itemState & ODS_SELECTED;
    COLORREF fill = main ? kGold : kPanel;
    if (down) fill = main ? RGB(220, 150, 10) : RGB(72, 62, 122);
    FillRect(d->hDC, &d->rcItem, g_back_brush);
    HBRUSH b = CreateSolidBrush(fill);
    HGDIOBJ old_brush = SelectObject(d->hDC, b), old_pen = SelectObject(d->hDC, GetStockObject(NULL_PEN));
    RoundRect(d->hDC, d->rcItem.left, d->rcItem.top, d->rcItem.right + 1, d->rcItem.bottom + 1, 12, 12);
    SelectObject(d->hDC, old_brush);
    SelectObject(d->hDC, old_pen);
    DeleteObject(b);
    wchar_t text[64];
    GetWindowTextW(d->hwndItem, text, 64);
    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, main ? kGoldInk : kInk);
    RECT r = d->rcItem;
    DrawTextW(d->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (d->itemState & ODS_FOCUS) {
        InflateRect(&r, -4, -4);
        DrawFocusRect(d->hDC, &r);
    }
}

LRESULT colours(UINT msg, WPARAM wp) {
    SetBkMode((HDC)wp, TRANSPARENT);
    SetTextColor((HDC)wp, kInk);
    SetBkColor((HDC)wp, msg == WM_CTLCOLORLISTBOX || msg == WM_CTLCOLOREDIT ? kPanel : kBack);
    return (LRESULT)(msg == WM_CTLCOLORLISTBOX || msg == WM_CTLCOLOREDIT ? g_panel_brush : g_back_brush);
}

LRESULT CALLBACK settings_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == ID_SAVE) { save_settings(); DestroyWindow(h); }
        if (LOWORD(wp) == ID_CANCEL) DestroyWindow(h);
        return 0;
    case WM_DRAWITEM: draw_button((const DRAWITEMSTRUCT*)lp); return TRUE;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: case WM_CTLCOLORLISTBOX: case WM_CTLCOLOREDIT: return colours(msg, wp);
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY:
        EnableWindow(g_home, TRUE);
        SetForegroundWindow(g_home);
        g_settings = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void open_settings() {
    if (g_settings) return;
    const int row = 38, top = 20, width = 470, height = top + kOptCount * row + 74;
    RECT r{0, 0, width, height};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRect(&r, style, FALSE);
    RECT home;
    GetWindowRect(g_home, &home);
    g_settings = CreateWindowExW(0, L"tje3settings", L"Settings", style, home.left + 30, home.top + 30, r.right - r.left,
                                 r.bottom - r.top, g_home, nullptr, GetModuleHandleW(nullptr), nullptr);
    dark_title_bar(g_settings);
    const std::vector<std::string> lines = read_lines(ini_path());
    for (int i = 0; i < kOptCount; i++) {
        const Opt& o = kOpts[i];
        control(g_settings, L"STATIC", o.title, SS_CENTERIMAGE, 24, top + i * row, 170, 28, 0, g_font);
        HWND combo = control(g_settings, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 200, top + i * row, 246, 300,
                             ID_COMBO + i, g_font);
        SetWindowTheme(combo, L"DarkMode_CFD", nullptr);
        const int now = ini_value(lines, o);
        int sel = 0;
        for (size_t k = 0; k < o.picks.size(); k++) {
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)o.picks[k].name);
            if (o.picks[k].value == now) sel = (int)k;
        }
        SendMessageW(combo, CB_SETCURSEL, sel, 0);
        g_combos[i] = combo;
    }
    const int by = top + kOptCount * row + 16;
    control(g_settings, L"BUTTON", L"Save", BS_OWNERDRAW | WS_TABSTOP, width - 24 - 110, by, 110, 36, ID_SAVE, g_button_font);
    control(g_settings, L"BUTTON", L"Cancel", BS_OWNERDRAW | WS_TABSTOP, width - 24 - 110 - 120, by, 110, 36, ID_CANCEL, g_button_font);
    control(g_settings, L"STATIC", L"Display options can also be changed in the game: Options > Display Settings.",
            0, 24, by - 2, 190, 44, 0, g_font);
    EnableWindow(g_home, FALSE);
    ShowWindow(g_settings, SW_SHOW);
}

LRESULT CALLBACK home_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_PLAY: g_start_game = true; DestroyWindow(h); break;
        case ID_SETTINGS: open_settings(); break;
        case ID_SETUP_AGAIN: {
            wchar_t exe[MAX_PATH * 2];
            GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
            ShellExecuteW(nullptr, L"open", exe, L"--setup", g_dir.c_str(), SW_SHOWNORMAL);
            DestroyWindow(h);
            break;
        }
        case ID_SKIP:
            if (SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                HANDLE f = CreateFileW(skip_path().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
                if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            } else {
                DeleteFileW(skip_path().c_str());
            }
            break;
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (g_game_icon) DrawIconEx(dc, 28, 26, g_game_icon, 96, 96, 0, nullptr, DI_NORMAL);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kInk);
        HGDIOBJ old = SelectObject(dc, g_title_font);
        TextOutW(dc, 146, 36, kGameName, (int)wcslen(kGameName));
        SelectObject(dc, g_font);
        SetTextColor(dc, kDim);
        const wchar_t sub[] = L"Mission to Earth  \x2022  PC recompilation";
        TextOutW(dc, 148, 82, sub, (int)wcslen(sub));
        SelectObject(dc, old);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DRAWITEM: draw_button((const DRAWITEMSTRUCT*)lp); return TRUE;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: return colours(msg, wp);
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Returns true if the player pressed Play.
bool run_home(HINSTANCE inst, int show) {
    SetProcessDPIAware();
    NONCLIENTMETRICSW nm{sizeof nm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof nm, &nm, 0);
    g_font = CreateFontIndirectW(&nm.lfMessageFont);
    LOGFONTW lf = nm.lfMessageFont;
    lf.lfHeight = -30;
    lf.lfWeight = FW_BOLD;
    g_title_font = CreateFontIndirectW(&lf);
    lf.lfHeight = -17;
    lf.lfWeight = FW_SEMIBOLD;
    g_button_font = CreateFontIndirectW(&lf);
    g_back_brush = CreateSolidBrush(kBack);
    g_panel_brush = CreateSolidBrush(kPanel);
    g_game_icon = (HICON)LoadImageW(nullptr, (g_game + L"\\tje3.ico").c_str(), IMAGE_ICON, 96, 96, LR_LOADFROMFILE);

    WNDCLASSW wc{};
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_back_brush;
    wc.hIcon = g_game_icon;
    wc.lpfnWndProc = home_proc;
    wc.lpszClassName = L"tje3home";
    RegisterClassW(&wc);
    wc.lpfnWndProc = settings_proc;
    wc.lpszClassName = L"tje3settings";
    RegisterClassW(&wc);

    const int width = 520, height = 300;
    RECT r{0, 0, width, height};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    g_home = CreateWindowExW(0, L"tje3home", kGameName, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                             r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    dark_title_bar(g_home);
    HWND play_button = control(g_home, L"BUTTON", L"Play", BS_OWNERDRAW | WS_TABSTOP, 28, 150, 220, 56, ID_PLAY, g_button_font);
    control(g_home, L"BUTTON", L"Settings", BS_OWNERDRAW | WS_TABSTOP, 262, 150, 110, 56, ID_SETTINGS, g_button_font);
    control(g_home, L"BUTTON", L"Set up again", BS_OWNERDRAW | WS_TABSTOP, 382, 150, 110, 56, ID_SETUP_AGAIN, g_button_font);
    HWND skip = control(g_home, L"BUTTON", L"Start the game straight away next time (hold Shift to see this window)",
                        BS_AUTOCHECKBOX | WS_TABSTOP, 28, 232, 464, 24, ID_SKIP, g_font);
    SetWindowTheme(skip, L"", L"");              // unthemed, so the text takes our colour
    ShowWindow(g_home, show);
    SetFocus(play_button);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && GetForegroundWindow() == g_home &&
            GetFocus() == play_button) {
            SendMessageW(g_home, WM_COMMAND, ID_PLAY, 0);
            continue;
        }
        HWND top = g_settings ? g_settings : g_home;
        if (!IsDialogMessageW(top, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return g_start_game;
}
