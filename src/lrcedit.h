// LRC-Editor: Songtext als Text bearbeiten und beim Mithören je Zeile einen Zeitstempel setzen (Knopf oder F8).
#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <string>
#include "music.h"
#include "parse.h"
#include "popup.h"
#pragma comment(lib, "uxtheme.lib")

namespace lrced {

enum { ID_PLAY = 101, ID_BACK, ID_FWD, ID_STAMP, ID_SAVE, ID_CANCEL, ID_CLOSE };
inline bool IsEditor(HWND h) {
    wchar_t c[16] = {};
    return GetClassNameW(h, c, 16) && !wcscmp(c, L"HbLrcEdit");
}
// feste Maße (bei 96 dpi): links der Text, rechts die Anleitung
constexpr int kW = 940, kH = 600, kSide = 280, kPad = 18, kHead = 58, kFoot = 62;
constexpr COLORREF cBg = RGB(24, 24, 31), cPanel = RGB(34, 34, 44), cBtn = RGB(52, 52, 64), cAcc = RGB(76, 141, 255),
                   cTxt = RGB(242, 242, 246), cDim = RGB(150, 152, 168);
struct State {
    HWND wnd = nullptr, edit = nullptr;
    std::wstring songId, title;
    HFONT font = nullptr, bold = nullptr, tiny = nullptr;
    HBRUSH panel = nullptr;
    bool dirty = false, done = false;
};

inline std::wstring EditText(HWND edit) {
    std::wstring t(GetWindowTextLengthW(edit) + 1, L'\0');
    t.resize(GetWindowTextW(edit, &t[0], (int)t.size()));
    return t;
}

// Zeile mit der Einfügemarke bekommt die aktuelle Zeit, dann geht es zur nächsten
inline void Stamp(State& s) {
    PlayerState ps = Player_State();
    if (!ps.active || ps.songId != s.songId) { MessageBeep(MB_ICONHAND); return; }  // es läuft ein anderer Song
    DWORD at = 0;
    SendMessageW(s.edit, EM_GETSEL, (WPARAM)&at, 0);
    int line = (int)SendMessageW(s.edit, EM_LINEFROMCHAR, at, 0);
    int a = (int)SendMessageW(s.edit, EM_LINEINDEX, line, 0);
    int len = (int)SendMessageW(s.edit, EM_LINELENGTH, a, 0);
    std::wstring all = EditText(s.edit);
    std::wstring neu = StampLine(all.substr(std::min<size_t>(a, all.size()), len), ps.pos);
    SendMessageW(s.edit, EM_SETSEL, a, a + len);
    SendMessageW(s.edit, EM_REPLACESEL, TRUE, (LPARAM)neu.c_str());
    int next = (int)SendMessageW(s.edit, EM_LINEINDEX, line + 1, 0);
    if (next >= 0) SendMessageW(s.edit, EM_SETSEL, next, next);
    SendMessageW(s.edit, EM_SCROLLCARET, 0, 0);
}

inline void Command(State& s, int id) {
    PlayerState ps = Player_State();
    bool mine = ps.active && ps.songId == s.songId;
    if (id == ID_PLAY) {
        if (mine) Player_Toggle();
        else Player_Play({s.songId}, 0);
    } else if ((id == ID_BACK || id == ID_FWD) && mine && ps.dur > 0) {
        Player_Seek((ps.pos + (id == ID_BACK ? -3 : 3)) / ps.dur);
    } else if (id == ID_STAMP) {
        Stamp(s);
    } else if (id == ID_SAVE) {
        if (Music_SaveLyrics(s.songId, EditText(s.edit))) s.done = true;
        else hb::Dialog(s.wnd, L"Songtext", L"Speichern fehlgeschlagen.", {{L"OK", IDOK}}, 0, IDOK);
        return;
    } else if (id == ID_CANCEL) {
        if (!s.dirty || hb::Dialog(s.wnd, L"Änderungen verwerfen?", L"Der Songtext wurde nicht gespeichert.",
                                   {{L"Verwerfen", IDYES}, {L"Weiter bearbeiten", IDNO}}, 1, IDNO) == IDYES)
            s.done = true;
        return;
    }
    SetFocus(s.edit);
}

inline void Layout(State& s) {
    using hb::Px;
    int left = Px(kPad), textR = Px(kW - kSide - kPad), bh = Px(34), by = Px(kH - kFoot + 10);
    MoveWindow(s.edit, left + Px(12), Px(kHead + 12), textR - left - Px(18), Px(kH - kHead - kFoot - 24), TRUE);
    int x = left;
    const int widths[] = {92, 62, 62, 150};
    for (int i = 0; i < 4; i++) {
        MoveWindow(GetDlgItem(s.wnd, ID_PLAY + i), x, by, Px(widths[i]), bh, TRUE);
        x += Px(widths[i] + 8);
    }
    MoveWindow(GetDlgItem(s.wnd, ID_CANCEL), Px(kW - kPad - 108), by, Px(108), bh, TRUE);
    MoveWindow(GetDlgItem(s.wnd, ID_SAVE), Px(kW - kPad - 224), by, Px(108), bh, TRUE);
    MoveWindow(GetDlgItem(s.wnd, ID_CLOSE), Px(kW - 46), Px(12), Px(32), Px(32), TRUE);
}

inline void Text(HDC dc, HFONT f, COLORREF c, const std::wstring& t, RECT r, UINT flags) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, t.c_str(), -1, &r, flags | DT_NOPREFIX);
}

inline void Paint(State& s, HDC dc) {
    using hb::Px;
    RECT all = {0, 0, Px(kW), Px(kH)};
    HBRUSH bg = CreateSolidBrush(cBg);
    FillRect(dc, &all, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    Text(dc, s.bold, cTxt, L"Songtext – " + s.title, {Px(kPad), 0, Px(kW - 60), Px(kHead)}, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    hb::Round(dc, {Px(kPad), Px(kHead), Px(kW - kSide - kPad), Px(kH - kFoot)}, Px(16), cPanel);
    int x = Px(kW - kSide), r = Px(kW - kPad), y = Px(kHead);
    hb::Round(dc, {x, y, r, Px(kH - kFoot)}, Px(16), cPanel);
    x += Px(16);
    r -= Px(14);
    y += Px(14);
    Text(dc, s.bold, cTxt, L"Anleitung", {x, y, r, y + Px(24)}, DT_LEFT | DT_SINGLELINE);
    y += Px(34);
    static const wchar_t* steps[] = {
        L"Songtext einfügen oder tippen: eine Zeile pro Gesangszeile.",
        L"In die erste Zeile klicken und den Song mit ▶ starten.",
        L"Sobald die Zeile gesungen wird: F8 drücken. Die Zeit wird gesetzt, weiter geht es in der nächsten Zeile.",
        L"Verpasst? Mit −3 s zurück, in die Zeile klicken und F8 noch einmal.",
        L"Speichern. Der Text läuft dann bei „Läuft gerade“ mit."};
    for (int i = 0; i < 5; i++) {
        RECT m = {x + Px(30), 0, r, 0};
        SelectObject(dc, s.tiny);
        DrawTextW(dc, steps[i], -1, &m, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        hb::Round(dc, {x, y, x + Px(20), y + Px(20)}, Px(20), cAcc);
        Text(dc, s.tiny, cTxt, std::to_wstring(i + 1), {x, y, x + Px(20), y + Px(20)}, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        Text(dc, s.tiny, cTxt, steps[i], {x + Px(30), y + Px(1), r, y + m.bottom + Px(4)}, DT_LEFT | DT_WORDBREAK);
        y += std::max((int)m.bottom, Px(20)) + Px(14);
    }
    Text(dc, s.tiny, cDim, L"F8  Zeit setzen\nEsc  Schließen\nZeitformat: [Minuten:Sekunden.Hundertstel]\nLeer speichern löscht den Songtext.",
         {x, y + Px(6), r, Px(kH - kFoot - 8)}, DT_LEFT | DT_WORDBREAK);
}

inline void DrawButton(State& s, const DRAWITEMSTRUCT& d) {
    wchar_t label[64] = {};
    GetWindowTextW(d.hwndItem, label, 64);
    RECT r = d.rcItem;
    HBRUSH bg = CreateSolidBrush(cBg);
    FillRect(d.hDC, &r, bg);
    DeleteObject(bg);
    bool down = (d.itemState & ODS_SELECTED) != 0, main = d.CtlID == ID_STAMP || d.CtlID == ID_SAVE;
    hb::Round(d.hDC, {r.left, r.top, r.right - 1, r.bottom - 1}, hb::Px(16), down ? RGB(80, 80, 96) : main ? cAcc : cBtn);
    SetBkMode(d.hDC, TRANSPARENT);
    Text(d.hDC, s.font, cTxt, label, r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

inline LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    State* s = (State*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!s) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        Paint(*s, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_NCHITTEST: {  // an der Kopfzeile verschieben
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &pt);
        return pt.y < hb::Px(kHead) ? HTCAPTION : HTCLIENT;
    }
    case WM_DRAWITEM: DrawButton(*s, *(DRAWITEMSTRUCT*)lp); return TRUE;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, cTxt);
        SetBkColor((HDC)wp, cPanel);
        return (LRESULT)s->panel;
    case WM_COMMAND:
        if ((HWND)lp == s->edit) { if (HIWORD(wp) == EN_CHANGE) s->dirty = true; }
        else if (HIWORD(wp) == BN_CLICKED) Command(*s, LOWORD(wp) == ID_CLOSE ? ID_CANCEL : LOWORD(wp));
        return 0;
    case WM_CLOSE: Command(*s, ID_CANCEL); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Öffnet den Editor und wartet, bis er geschlossen wird; true, wenn gespeichert wurde
inline bool Run(HWND owner, const std::wstring& songId, const std::wstring& title, const std::wstring& text) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"HbLrcEdit";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.style = CS_DROPSHADOW;
        RegisterClassW(&wc);
        registered = true;
    }
    State s;
    s.songId = songId;
    s.title = title;
    int w = hb::Px(kW), h = hb::Px(kH);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    s.wnd = CreateWindowExW(WS_EX_TOPMOST, L"HbLrcEdit", L"Songtext", WS_POPUP | WS_CLIPCHILDREN, (wa.left + wa.right - w) / 2, (wa.top + wa.bottom - h) / 2, w, h,
                            owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s.wnd) return false;
    int round = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(s.wnd, 33, &round, sizeof(round));
    auto mk = [](int px, int weight) {
        return CreateFontW(-hb::Px(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                           hb::g_style.font.c_str());
    };
    s.font = mk(15, FW_NORMAL);
    s.bold = mk(18, FW_SEMIBOLD);
    s.tiny = mk(13, FW_NORMAL);
    s.panel = CreateSolidBrush(cPanel);
    auto child = [&](const wchar_t* cls, const wchar_t* label, DWORD style, int id) {
        HWND c = CreateWindowExW(0, cls, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, s.wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)s.font, TRUE);
        return c;
    };
    s.edit = child(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN | ES_NOHIDESEL | WS_VSCROLL, 0);
    SetWindowTheme(s.edit, L"DarkMode_Explorer", nullptr);  // dunkle Bildlaufleiste
    SendMessageW(s.edit, EM_SETLIMITTEXT, 500000, 0);
    const wchar_t* labels[] = {L"▶ / ❚❚", L"−3 s", L"+3 s", L"Zeit setzen (F8)"};
    for (int i = 0; i < 4; i++) child(L"BUTTON", labels[i], BS_OWNERDRAW, ID_PLAY + i);
    child(L"BUTTON", L"Speichern", BS_OWNERDRAW, ID_SAVE);
    child(L"BUTTON", L"Abbrechen", BS_OWNERDRAW, ID_CANCEL);
    child(L"BUTTON", L"✕", BS_OWNERDRAW, ID_CLOSE);
    std::wstring crlf;  // das Eingabefeld braucht \r\n
    for (wchar_t c : text)
        if (c == L'\n') crlf += L"\r\n";
        else if (c != L'\r') crlf += c;
    SetWindowTextW(s.edit, crlf.c_str());
    SetWindowLongPtrW(s.wnd, GWLP_USERDATA, (LONG_PTR)&s);
    Layout(s);
    ShowWindow(s.wnd, SW_SHOW);
    SetForegroundWindow(s.wnd);
    SetFocus(s.edit);
    SendMessageW(s.edit, EM_SETSEL, 0, 0);
    s.dirty = false;
    MSG m;
    while (!s.done && IsWindow(s.wnd)) {
        if (GetMessageW(&m, nullptr, 0, 0) <= 0) {
            PostQuitMessage((int)m.wParam);
            break;
        }
        bool ours = m.hwnd == s.wnd || IsChild(s.wnd, m.hwnd);
        if (ours && m.message == WM_KEYDOWN && m.wParam == VK_F8) { Command(s, ID_STAMP); continue; }
        if (ours && m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) { Command(s, ID_CANCEL); continue; }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    bool saved = s.done && s.dirty;
    SetWindowLongPtrW(s.wnd, GWLP_USERDATA, 0);
    DestroyWindow(s.wnd);
    for (HGDIOBJ o : {(HGDIOBJ)s.font, (HGDIOBJ)s.bold, (HGDIOBJ)s.tiny, (HGDIOBJ)s.panel}) DeleteObject(o);
    return saved;
}

}  // namespace lrced
