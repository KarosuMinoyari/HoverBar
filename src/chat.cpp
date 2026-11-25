// Claude-Chat: eigenes (nicht durchsichtiges) Fenster mit WebView2, über dem Inhaltsbereich der Leiste.
#define NOMINMAX
#include "chat.h"
#include <shlobj.h>
#include <dwmapi.h>
#include <wrl.h>
#include "WebView2.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

static HWND g_wnd = nullptr;
static ComPtr<ICoreWebView2Environment> g_env;
static ComPtr<ICoreWebView2Controller> g_ctl;
static ComPtr<ICoreWebView2> g_web;
static bool g_creating = false;
static std::wstring g_status;
static std::wstring g_pending = L"https://claude.ai/new";

static void Fit() {
    if (!g_ctl || !g_wnd) return;
    RECT r;
    GetClientRect(g_wnd, &r);
    g_ctl->put_Bounds(r);
}

static LRESULT CALLBACK ChatProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_SIZE:
        Fit();
        return 0;
    case WM_ERASEBKGND: {  // dunkler Grund, solange die Seite lädt
        RECT r;
        GetClientRect(h, &r);
        HBRUSH b = CreateSolidBrush(RGB(38, 38, 36));
        FillRect((HDC)wp, &r, b);
        DeleteObject(b);
        return 1;
    }
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static std::wstring ProfileDir() {
    PWSTR p = nullptr;
    std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) d = p;
    CoTaskMemFree(p);
    return d + L"\\HoverBar\\webview";
}

static void CreateWebView() {
    if (g_creating || g_ctl) return;
    g_creating = true;
    g_status = L"Lädt …";
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, ProfileDir().c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([](HRESULT res, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(res) || !env || !g_wnd) {
                g_creating = false;
                g_status = L"Webansicht konnte nicht gestartet werden (WebView2-Laufzeit fehlt?).";
                return S_OK;
            }
            g_env = env;
            env->CreateCoreWebView2Controller(
                g_wnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([](HRESULT r2, ICoreWebView2Controller* ctl) -> HRESULT {
                           g_creating = false;
                           if (FAILED(r2) || !ctl || !g_wnd) {
                               g_status = L"Webansicht konnte nicht erstellt werden.";
                               return S_OK;
                           }
                           g_ctl = ctl;
                           ctl->get_CoreWebView2(&g_web);
                           // dunkler Hintergrund passend zur Leiste
                           ComPtr<ICoreWebView2Controller2> c2;
                           if (SUCCEEDED(ctl->QueryInterface(IID_PPV_ARGS(&c2)))) c2->put_DefaultBackgroundColor({255, 38, 38, 36});
                           ComPtr<ICoreWebView2Settings> s;
                           if (g_web && SUCCEEDED(g_web->get_Settings(&s))) {
                               s->put_AreDevToolsEnabled(FALSE);
                               s->put_IsStatusBarEnabled(FALSE);
                           }
                           // Links zu anderen Seiten im normalen Browser öffnen, claude.ai bleibt hier
                           if (g_web) {
                               EventRegistrationToken tok;
                               g_web->add_NewWindowRequested(
                                   Callback<ICoreWebView2NewWindowRequestedEventHandler>([](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a) -> HRESULT {
                                       LPWSTR uri = nullptr;
                                       if (SUCCEEDED(a->get_Uri(&uri)) && uri) {
                                           std::wstring u = uri;
                                           CoTaskMemFree(uri);
                                           // Anmelde-Fenster (Google, Apple, claude.ai selbst) im eigenen Popup lassen,
                                           // sonst kommt die Anmeldung nie im Chat an
                                           size_t hs = u.find(L"://");
                                           std::wstring host = hs == std::wstring::npos ? L"" : u.substr(hs + 3, u.find(L'/', hs + 3) - hs - 3);
                                           for (auto& c : host) c = towlower(c);
                                           auto ends = [&](const wchar_t* d) {
                                               size_t n = wcslen(d);
                                               return host == d + 1 || (host.size() > n && host.compare(host.size() - n, n, d) == 0);
                                           };
                                           if (ends(L".google.com") || ends(L".apple.com") || ends(L".claude.ai") || ends(L".anthropic.com"))
                                               return S_OK;  // WebView2 öffnet ein eigenes Fenster (mit Verbindung zur Seite)
                                           ShellExecuteW(nullptr, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                                       }
                                       a->put_Handled(TRUE);
                                       return S_OK;
                                   }).Get(),
                                   &tok);
                               g_web->Navigate(g_pending.c_str());
                           }
                           Fit();
                           ctl->put_IsVisible(TRUE);
                           ctl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                           g_status.clear();
                           return S_OK;
                       }).Get());
            return S_OK;
        }).Get());
    if (FAILED(hr)) {
        g_creating = false;
        g_status = L"WebView2-Laufzeit nicht gefunden.";
    }
}

void Chat_Show(HWND owner, const RECT& sr, int corner) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = ChatProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"HoverBarChat";
        RegisterClassW(&wc);
        reg = true;
    }
    if (!g_wnd) {
        g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, L"HoverBarChat", L"Claude", WS_POPUP | WS_CLIPCHILDREN,
                                sr.left, sr.top, sr.right - sr.left, sr.bottom - sr.top, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!g_wnd) return;
        SetLayeredWindowAttributes(g_wnd, 0, 178, LWA_ALPHA);  // durchsichtig: die Glasleiste scheint durch
        // runde Ecken wie die Karten der Leiste (Windows 11)
        DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUNDSMALL;
        DwmSetWindowAttribute(g_wnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
        (void)corner;
    }
    // nur bei Änderung verschieben/zeigen – und nie aktivieren (sonst verliert das Eingabefeld bei jedem Neuzeichnen den Fokus)
    static RECT last = {};
    bool wasVisible = IsWindowVisible(g_wnd) != FALSE;
    if (!wasVisible || !EqualRect(&last, &sr)) {
        last = sr;
        SetWindowPos(g_wnd, HWND_TOPMOST, sr.left, sr.top, sr.right - sr.left, sr.bottom - sr.top, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }
    if (!g_ctl) CreateWebView();
    else if (!wasVisible) {
        g_ctl->put_IsVisible(TRUE);
        Fit();
    }
}

void Chat_Hide() {
    if (g_ctl) g_ctl->put_IsVisible(FALSE);
    if (g_wnd) ShowWindow(g_wnd, SW_HIDE);
}

void Chat_Free() {
    if (g_web) {  // Seite merken (laufende Unterhaltung), damit sie beim nächsten Öffnen wieder kommt
        LPWSTR u = nullptr;
        if (SUCCEEDED(g_web->get_Source(&u)) && u) {
            std::wstring s = u;
            if (s.find(L"claude.ai") != std::wstring::npos) g_pending = s;
            CoTaskMemFree(u);
        }
    }
    if (g_ctl) g_ctl->Close();
    g_web.Reset();
    g_ctl.Reset();
    g_env.Reset();
    if (g_wnd) { DestroyWindow(g_wnd); g_wnd = nullptr; }
    g_creating = false;
    g_status.clear();
}

bool Chat_IsOwnWindow(HWND h) {
    return g_wnd && h && (h == g_wnd || IsChild(g_wnd, h) || GetAncestor(h, GA_ROOT) == g_wnd);
}

void Chat_Navigate(const std::wstring& url) {
    g_pending = url;
    if (g_web) g_web->Navigate(url.c_str());
}

std::wstring Chat_Status() { return g_status; }
