#include "host_console.h"

#include <windows.h>
#include <commctrl.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "util.h"

namespace kcp {

namespace {

std::mutex g_mutex;
ConsoleModel g_model;
bool g_modelDirty = false;
std::vector<std::string> g_commands;

std::thread g_thread;
std::atomic<HWND> g_wnd{nullptr};
std::atomic<bool> g_visible{false};
std::atomic<bool> g_quit{false};

HWND g_status = nullptr, g_list = nullptr, g_log = nullptr, g_input = nullptr, g_send = nullptr;
HFONT g_font = nullptr, g_fontBold = nullptr;
uint64_t g_logSeq = 0;
bool g_firstFill = true;
constexpr UINT kTimerId = 1;
constexpr UINT kMsgShow = WM_APP + 1;
constexpr int kIdSend = 100;

std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}
std::string N(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

void Layout(HWND wnd) {
    RECT r;
    GetClientRect(wnd, &r);
    const int w = r.right, h = r.bottom, pad = 8, statusH = 22, listH = 150, inputH = 26, sendW = 90;
    MoveWindow(g_status, pad, pad, w - 2 * pad, statusH, TRUE);
    MoveWindow(g_list, pad, pad + statusH + 4, w - 2 * pad, listH, TRUE);
    const int logTop = pad + statusH + 4 + listH + 6;
    const int logH = h - logTop - inputH - 2 * pad;
    MoveWindow(g_log, pad, logTop, w - 2 * pad, logH > 50 ? logH : 50, TRUE);
    MoveWindow(g_input, pad, h - pad - inputH, w - 3 * pad - sendW, inputH, TRUE);
    MoveWindow(g_send, w - pad - sendW, h - pad - inputH, sendW, inputH, TRUE);
}

void AddColumn(int i, const wchar_t* title, int width) {
    LVCOLUMNW c{};
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    c.pszText = const_cast<wchar_t*>(title);
    c.cx = width;
    c.iSubItem = i;
    SendMessageW(g_list, LVM_INSERTCOLUMNW, WPARAM(i), LPARAM(&c));
}

void SetCell(int row, int col, const std::wstring& text) {
    LVITEMW it{};
    it.iSubItem = col;
    it.pszText = const_cast<wchar_t*>(text.c_str());
    if (col == 0) {
        it.mask = LVIF_TEXT;
        it.iItem = row;
        SendMessageW(g_list, LVM_INSERTITEMW, 0, LPARAM(&it));
    } else {
        SendMessageW(g_list, LVM_SETITEMTEXTW, WPARAM(row), LPARAM(&it));
    }
}

void RefreshModel() {
    ConsoleModel m;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (!g_modelDirty) return;
        g_modelDirty = false;
        m = g_model;
    }
    SetWindowTextW(g_status, W(m.status).c_str());
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_list, LVM_DELETEALLITEMS, 0, 0);
    for (size_t i = 0; i < m.players.size(); ++i) {
        const auto& p = m.players[i];
        const int row = int(i);
        SetCell(row, 0, std::to_wstring(p.id));
        SetCell(row, 1, W(p.name));
        SetCell(row, 2, p.pingMs ? std::to_wstring(p.pingMs) + L" ms" : L"-");
        SetCell(row, 3, std::to_wstring(p.characters));
        SetCell(row, 4, W(p.state));
        SetCell(row, 5, (p.warn ? L"⚠ " : L"") + W(p.sync));
    }
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
}

void AppendLog() {
    uint64_t seq = 0;
    RecentLog(0, &seq);
    if (seq == g_logSeq && !g_firstFill) return;
    const size_t want = g_firstFill ? 500 : size_t(std::min<uint64_t>(500, seq - g_logSeq));
    g_firstFill = false;
    const auto lines = RecentLog(want, &seq);
    g_logSeq = seq;
    std::wstring text;
    for (const auto& l : lines) text += W(l) + L"\r\n";
    if (text.empty()) return;
    // keep the control light: past ~400k characters, the oldest half goes
    const int len = GetWindowTextLengthW(g_log);
    if (len > 400000) {
        SendMessageW(g_log, EM_SETSEL, 0, len / 2);
        SendMessageW(g_log, EM_REPLACESEL, FALSE, LPARAM(L""));
    }
    const int end = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, WPARAM(end), LPARAM(end));
    SendMessageW(g_log, EM_REPLACESEL, FALSE, LPARAM(text.c_str()));
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
}

void SendCommand() {
    const int n = GetWindowTextLengthW(g_input);
    if (n <= 0) return;
    std::wstring w(size_t(n) + 1, L'\0');
    GetWindowTextW(g_input, w.data(), n + 1);
    w.resize(size_t(n));
    SetWindowTextW(g_input, L"");
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_commands.size() < 64) g_commands.push_back(N(w));
}

LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        g_fontBold = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 0, 0, wnd, nullptr, nullptr, nullptr);
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER, 0, 0, 0, 0,
                                 wnd, nullptr, nullptr, nullptr);
        SendMessageW(g_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        AddColumn(0, L"#", 36);
        AddColumn(1, L"Joueur", 160);
        AddColumn(2, L"Ping", 70);
        AddColumn(3, L"Persos", 60);
        AddColumn(4, L"État", 170);
        AddColumn(5, L"Synchro", 420);
        g_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL, 0, 0,
                                0, 0, wnd, nullptr, nullptr, nullptr);
        SendMessageW(g_log, EM_SETLIMITTEXT, 0, 0);
        g_input = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, wnd, nullptr, nullptr, nullptr);
        g_send = CreateWindowExW(0, L"BUTTON", L"Envoyer", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, wnd, HMENU(INT_PTR(kIdSend)), nullptr,
                                 nullptr);
        for (HWND c : {g_log, g_input}) SendMessageW(c, WM_SETFONT, WPARAM(g_font), TRUE);
        for (HWND c : {g_status, g_list, g_send}) SendMessageW(c, WM_SETFONT, WPARAM(g_fontBold), TRUE);
        SendMessageW(g_input, EM_SETCUEBANNER, TRUE, LPARAM(L"commande (help pour la liste) puis Entrée"));
        SetTimer(wnd, kTimerId, 250, nullptr);
        Layout(wnd);
        return 0;
    }
    case WM_SIZE: Layout(wnd); return 0;
    case WM_TIMER:
        RefreshModel();
        AppendLog();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == kIdSend) SendCommand();
        return 0;
    case kMsgShow:
        ShowWindow(wnd, wp ? SW_SHOWNOACTIVATE : SW_HIDE);
        return 0;
    case WM_CLOSE:   // closing hides it: Ctrl+Shift+W (or the button in the Multijoueur window) brings it back
        ShowWindow(wnd, SW_HIDE);
        g_visible = false;
        return 0;
    case WM_DESTROY:
        KillTimer(wnd, kTimerId);
        PostQuitMessage(0);
        return 0;
    default: return DefWindowProcW(wnd, msg, wp, lp);
    }
}

void ThreadMain() {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = HBRUSH(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"KenshiCoopHostConsole";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);
    HWND wnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"KenshiCoop — console de l'hôte", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                               CW_USEDEFAULT, 1000, 640, nullptr, nullptr, wc.hInstance, nullptr);
    if (!wnd) { Log("host console: cannot create its window (%lu)", GetLastError()); return; }
    g_wnd = wnd;
    ShowWindow(wnd, SW_SHOWNOACTIVATE);   // never steals the focus from the game
    MSG msg;
    while (!g_quit && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.hwnd == g_input && msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            SendCommand();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_wnd = nullptr;
}

} // namespace

void HostConsoleShow(bool show) {
    if (show && !g_thread.joinable()) {
        g_quit = false;
        g_visible = true;
        g_thread = std::thread(ThreadMain);
        Log("host console window opened");
        return;
    }
    g_visible = show;
    if (HWND w = g_wnd.load()) PostMessageW(w, kMsgShow, show ? 1 : 0, 0);
}

bool HostConsoleVisible() { return g_visible.load(); }

void HostConsolePublish(ConsoleModel model) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_model = std::move(model);
    g_modelDirty = true;
}

std::vector<std::string> HostConsoleTakeCommands() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return std::exchange(g_commands, {});
}

void HostConsoleShutdown() {
    g_quit = true;
    if (HWND w = g_wnd.load()) PostMessageW(w, WM_CLOSE, 0, 0), PostMessageW(w, WM_QUIT, 0, 0);
    if (g_thread.joinable()) g_thread.detach();   // the process is going away: never block the game's exit
}

} // namespace kcp
