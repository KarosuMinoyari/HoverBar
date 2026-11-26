// Eigene Auswahlkästen und Meldungen im HoverBar-Stil statt der Windows-Menüs und -Dialoge.
// HbMenu zeigt ein gewöhnliches HMENU (Häkchen, Trennlinien, ausgegraut, Untermenüs) und liefert den gewählten Befehl (0 = nichts).
#pragma once
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <algorithm>
#include <string>
#include <vector>

namespace hb {

struct Style { std::wstring font = L"Segoe UI"; UINT dpi = 96; };
inline Style g_style;
inline int Px(int v) { return MulDiv(v, g_style.dpi, 96); }

struct Item { std::wstring text; UINT id = 0; bool separator = false, grayed = false, checked = false; HMENU sub = nullptr; };

struct Popup {
    HWND hwnd = nullptr;
    HFONT font = nullptr, bold = nullptr;
    // Menü
    std::vector<HMENU> stack;  // offene Ebenen (Untermenüs ersetzen den Inhalt, "‹ Zurück" geht eine Ebene hoch)
    std::vector<Item> items;
    int hot = -1, scroll = 0, rowH = 0, sepH = 0, width = 0, height = 0, content = 0;
    POINT anchor = {};
    // Meldung
    bool dialog = false;
    std::wstring title, message;
    std::vector<std::pair<std::wstring, int>> buttons;
    std::vector<RECT> buttonRects;
    int cancel = 0;
    // Ergebnis
    int result = 0;
    bool done = false;
};

inline int ItemHeight(const Popup& p, const Item& i) { return i.separator ? p.sepH : p.rowH; }

inline void Load(Popup& p) {
    p.items.clear();
    HMENU m = p.stack.back();
    if (p.stack.size() > 1) p.items.push_back({L"‹  Zurück", (UINT)-1});
    int n = GetMenuItemCount(m);
    for (int i = 0; i < n; i++) {
        wchar_t text[300] = {};
        MENUITEMINFOW mi = {sizeof(mi)};
        mi.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
        mi.dwTypeData = text;
        mi.cch = 299;
        if (!GetMenuItemInfoW(m, i, TRUE, &mi)) continue;
        Item it;
        for (const wchar_t* c = text; *c && *c != L'\t'; c++) {
            if (*c == L'&' && *(c + 1) != L'&') continue;
            it.text += *c;
            if (*c == L'&') c++;
        }
        it.id = mi.wID;
        it.separator = (mi.fType & MFT_SEPARATOR) != 0;
        it.grayed = (mi.fState & MFS_DISABLED) != 0;
        it.checked = (mi.fState & MFS_CHECKED) != 0;
        it.sub = mi.hSubMenu;
        p.items.push_back(it);
    }
    p.hot = -1;
    p.scroll = 0;
}

// Größe aus dem Inhalt, Lage am Mauszeiger, aber immer ganz auf dem Bildschirm
inline void Place(Popup& p) {
    HDC dc = GetDC(p.hwnd);
    SelectObject(dc, p.font);
    int w = Px(180);
    p.content = 0;
    for (auto& i : p.items) {
        SIZE s = {};
        GetTextExtentPoint32W(dc, i.text.c_str(), (int)i.text.size(), &s);
        w = std::max(w, (int)s.cx + Px(76));
        p.content += ItemHeight(p, i);
    }
    ReleaseDC(p.hwnd, dc);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(p.anchor, MONITOR_DEFAULTTONEAREST), &mi);
    RECT wa = mi.rcWork;
    p.width = std::min(w, Px(420));
    p.height = std::min(p.content + Px(12), (int)(wa.bottom - wa.top) * 7 / 10);
    int x = p.anchor.x + p.width > wa.right ? p.anchor.x - p.width : p.anchor.x;
    int y = p.anchor.y + p.height > wa.bottom ? p.anchor.y - p.height : p.anchor.y;
    SetWindowPos(p.hwnd, HWND_TOPMOST, std::max((int)wa.left, x), std::max((int)wa.top, y), p.width, p.height, SWP_NOACTIVATE);
    InvalidateRect(p.hwnd, nullptr, FALSE);
}

inline void Round(HDC dc, const RECT& r, int rad, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, rad, rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
}

inline int ItemAt(const Popup& p, int y) {
    int top = Px(6) - p.scroll;
    for (int i = 0; i < (int)p.items.size(); i++) {
        int h = ItemHeight(p, p.items[i]);
        if (y >= top && y < top + h) return p.items[i].separator || p.items[i].grayed ? -1 : i;
        top += h;
    }
    return -1;
}

inline void Paint(Popup& p, HDC dc, const RECT& all) {
    HBRUSH bg = CreateSolidBrush(RGB(32, 32, 40));
    FillRect(dc, &all, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    const COLORREF text = RGB(242, 242, 246), dim = RGB(125, 125, 138), accent = RGB(76, 141, 255);
    if (p.dialog) {
        SelectObject(dc, p.bold);
        SetTextColor(dc, text);
        RECT t = {Px(22), Px(18), all.right - Px(22), Px(44)};
        DrawTextW(dc, p.title.c_str(), -1, &t, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, p.font);
        SetTextColor(dc, RGB(205, 205, 214));
        RECT m = {Px(22), Px(52), all.right - Px(22), all.bottom - Px(68)};
        DrawTextW(dc, p.message.c_str(), -1, &m, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        for (int i = 0; i < (int)p.buttons.size(); i++) {
            Round(dc, p.buttonRects[i], Px(16), i == p.hot ? accent : RGB(52, 52, 64));
            SetTextColor(dc, text);
            RECT r = p.buttonRects[i];
            DrawTextW(dc, p.buttons[i].first.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        return;
    }
    SelectObject(dc, p.font);
    int top = Px(6) - p.scroll;
    for (int i = 0; i < (int)p.items.size(); i++) {
        const Item& it = p.items[i];
        int h = ItemHeight(p, it);
        if (it.separator) {
            RECT line = {Px(14), top + h / 2, all.right - Px(14), top + h / 2 + 1};
            HBRUSH b = CreateSolidBrush(RGB(62, 62, 76));
            FillRect(dc, &line, b);
            DeleteObject(b);
        } else {
            if (i == p.hot) {
                RECT r = {Px(6), top, all.right - Px(6), top + h};
                Round(dc, r, Px(12), accent);
            }
            SetTextColor(dc, it.grayed ? dim : text);
            RECT r = {Px(34), top, all.right - Px(30), top + h};
            DrawTextW(dc, it.text.c_str(), -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (it.checked) {
                RECT c = {Px(10), top, Px(32), top + h};
                DrawTextW(dc, L"✓", -1, &c, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            if (it.sub) {
                RECT c = {all.right - Px(28), top, all.right - Px(10), top + h};
                DrawTextW(dc, L"›", -1, &c, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
        top += h;
    }
}

inline void Choose(Popup& p, int i) {
    if (i < 0 || i >= (int)p.items.size()) return;
    Item it = p.items[i];
    if (it.separator || it.grayed) return;
    if (it.id == (UINT)-1 && p.stack.size() > 1) p.stack.pop_back();
    else if (it.sub) p.stack.push_back(it.sub);
    else {
        p.result = (int)it.id;
        p.done = true;
        return;
    }
    Load(p);
    Place(p);
}

inline LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Popup* p = (Popup*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!p) return DefWindowProcW(h, msg, wp, lp);
    POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    auto buttonAt = [&] {
        for (int i = 0; i < (int)p->buttonRects.size(); i++)
            if (PtInRect(&p->buttonRects[i], pt)) return i;
        return -1;
    };
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT all;
        GetClientRect(h, &all);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, all.right, all.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        Paint(*p, mem, all);
        BitBlt(dc, 0, 0, all.right, all.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int hot = p->dialog ? buttonAt() : ItemAt(*p, pt.y);
        if (hot != p->hot && (hot >= 0 || !p->dialog)) { p->hot = hot; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        if (p->dialog) {
            int i = buttonAt();
            if (i >= 0) { p->result = p->buttons[i].second; p->done = true; }
        } else Choose(*p, ItemAt(*p, pt.y));
        return 0;
    case WM_MOUSEWHEEL:
        p->scroll = std::max(0, std::min(p->content + Px(12) - p->height, p->scroll - GET_WHEEL_DELTA_WPARAM(wp) / 120 * p->rowH * 2));
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { p->result = p->cancel; p->done = true; return 0; }
        if (p->dialog) {
            int n = (int)p->buttons.size();
            if (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_TAB) p->hot = (p->hot + (wp == VK_LEFT ? n - 1 : 1)) % n;
            if ((wp == VK_RETURN || wp == VK_SPACE) && p->hot >= 0) { p->result = p->buttons[p->hot].second; p->done = true; }
        } else if (wp == VK_UP || wp == VK_DOWN) {
            int n = (int)p->items.size(), i = p->hot;
            for (int k = 0; k < n; k++) {
                i = (i + (wp == VK_DOWN ? 1 : n - 1) + n) % n;
                if (i < 0) i = 0;
                if (!p->items[i].separator && !p->items[i].grayed) break;
            }
            p->hot = i;
            int top = 0;
            for (int k = 0; k < i; k++) top += ItemHeight(*p, p->items[k]);
            p->scroll = std::max(0, std::min(top, std::max(top + p->rowH + Px(12) - p->height, p->scroll)));
        } else if (wp == VK_RETURN || wp == VK_RIGHT) {
            Choose(*p, p->hot);
        } else if (wp == VK_LEFT && p->stack.size() > 1) {
            Choose(*p, 0);
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_ACTIVATE:  // woanders geklickt: Menü zu (eine Meldung bleibt stehen)
        if (LOWORD(wp) == WA_INACTIVE && !p->dialog) p->done = true;
        return 0;
    case WM_CLOSE:
        p->result = p->cancel;
        p->done = true;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

inline void Create(Popup& p, HWND owner) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"HbPopup";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.style = CS_DROPSHADOW;
        RegisterClassW(&wc);
        registered = true;
    }
    p.font = CreateFontW(-Px(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, g_style.font.c_str());
    p.bold = CreateFontW(-Px(16), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, g_style.font.c_str());
    p.rowH = Px(32);
    p.sepH = Px(9);
    p.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"HbPopup", L"HoverBar", WS_POPUP, 0, 0, 10, 10, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetWindowLongPtrW(p.hwnd, GWLP_USERDATA, (LONG_PTR)&p);
    int round = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(p.hwnd, 33, &round, sizeof(round));
}

inline void Run(Popup& p) {
    ShowWindow(p.hwnd, SW_SHOW);
    SetForegroundWindow(p.hwnd);
    SetFocus(p.hwnd);
    MSG m;
    while (!p.done && IsWindow(p.hwnd)) {
        if (GetMessageW(&m, nullptr, 0, 0) <= 0) {
            PostQuitMessage((int)m.wParam);
            break;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    SetWindowLongPtrW(p.hwnd, GWLP_USERDATA, 0);
    DestroyWindow(p.hwnd);
    DeleteObject(p.font);
    DeleteObject(p.bold);
}

inline int Menu(HMENU menu, HWND owner, POINT at) {
    Popup p;
    p.anchor = at;
    p.stack.push_back(menu);
    Create(p, owner);
    Load(p);
    Place(p);
    Run(p);
    return p.result;
}

// buttons: Beschriftung und Rückgabewert; preselect = vorbelegter Knopf, cancel = Wert für Esc
inline int Dialog(HWND owner, const std::wstring& title, const std::wstring& message, std::vector<std::pair<std::wstring, int>> buttons, int preselect, int cancel) {
    Popup p;
    p.dialog = true;
    p.title = title;
    p.message = message;
    p.buttons = std::move(buttons);
    p.hot = preselect;
    p.cancel = p.result = cancel;
    Create(p, owner);
    int w = Px(440);
    HDC dc = GetDC(p.hwnd);
    SelectObject(dc, p.font);
    RECT m = {0, 0, w - Px(44), 0};
    DrawTextW(dc, message.c_str(), -1, &m, DT_CALCRECT | DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    std::vector<int> widths;
    for (auto& b : p.buttons) {
        SIZE s = {};
        GetTextExtentPoint32W(dc, b.first.c_str(), (int)b.first.size(), &s);
        widths.push_back(std::max(Px(96), (int)s.cx + Px(36)));
    }
    ReleaseDC(p.hwnd, dc);
    int h = Px(52) + m.bottom + Px(72);
    p.buttonRects.resize(p.buttons.size());
    for (int i = (int)p.buttons.size() - 1, x = w - Px(18); i >= 0; i--) {
        p.buttonRects[i] = {x - widths[i], h - Px(52), x, h - Px(18)};
        x -= widths[i] + Px(10);
    }
    POINT c;
    GetCursorPos(&c);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowPos(p.hwnd, HWND_TOPMOST, (mi.rcWork.left + mi.rcWork.right - w) / 2, (mi.rcWork.top + mi.rcWork.bottom - h) / 2, w, h, SWP_NOACTIVATE);
    Run(p);
    return p.result;
}

// Ersatz für MessageBoxW mit denselben Angaben
inline int Box(HWND owner, const wchar_t* text, const wchar_t* title, UINT flags) {
    std::vector<std::pair<std::wstring, int>> b;
    int cancel = IDOK;
    if ((flags & 0xF) == MB_YESNO) { b = {{L"Ja", IDYES}, {L"Nein", IDNO}}; cancel = IDNO; }
    else if ((flags & 0xF) == MB_OKCANCEL) { b = {{L"OK", IDOK}, {L"Abbrechen", IDCANCEL}}; cancel = IDCANCEL; }
    else b = {{L"OK", IDOK}};
    int preselect = (flags & MB_DEFBUTTON2) && b.size() > 1 ? 1 : 0;
    return Dialog(owner, title ? title : L"HoverBar", text ? text : L"", b, preselect, cancel);
}

}  // namespace hb
