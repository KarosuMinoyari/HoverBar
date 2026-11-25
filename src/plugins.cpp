// Optionale Erweiterungen. WizTree wird nur auf Wunsch (Einstellungen -> Vorgeschlagen) installiert:
// aktuelle portable Version von der offiziellen Seite suchen, Teil-Prüfsumme der Seite vergleichen,
// entpacken (tar.exe von Windows) und die digitale Signatur des Herstellers prüfen.
#define NOMINMAX
#include "plugins.h"
#include <shlobj.h>
#include <urlmon.h>
#include <bcrypt.h>
#include <softpub.h>
#include <wintrust.h>
#include <vector>
#include <cwctype>
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "version.lib")

static CRITICAL_SECTION g_cs;
static bool g_csInit = false;
static PlugStatus g_st;
static bool g_busy = false;

static void InitCs() {
    if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = true; }
}

static std::wstring Dir() {
    PWSTR p = nullptr;
    std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) d = p;
    CoTaskMemFree(p);
    return d + L"\\HoverBar\\plugins\\WizTree";
}

std::wstring WizTree_Exe() {
    std::wstring e = Dir() + L"\\WizTree64.exe";
    return GetFileAttributesW(e.c_str()) != INVALID_FILE_ATTRIBUTES ? e : L"";
}

std::wstring WizTree_Version() {
    std::wstring exe = WizTree_Exe();
    if (exe.empty()) return L"";
    DWORD dummy = 0, sz = GetFileVersionInfoSizeW(exe.c_str(), &dummy);
    if (!sz) return L"";
    std::vector<BYTE> buf(sz);
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoW(exe.c_str(), 0, sz, buf.data()) || !VerQueryValueW(buf.data(), L"\\", (void**)&fi, &len) || !fi) return L"";
    return std::to_wstring(HIWORD(fi->dwFileVersionMS)) + L"." + (LOWORD(fi->dwFileVersionMS) < 10 ? L"0" : L"") +
           std::to_wstring(LOWORD(fi->dwFileVersionMS));
}

PlugStatus WizTree_Status() {
    InitCs();
    EnterCriticalSection(&g_cs);
    PlugStatus s = g_st;
    LeaveCriticalSection(&g_cs);
    return s;
}

static void Set(HWND n, int state, const std::wstring& msg, int pct = -1) {
    EnterCriticalSection(&g_cs);
    g_st.state = state;
    if (!msg.empty() || state != PLUG_DOWNLOADING) g_st.msg = msg;
    g_st.percent = pct;
    LeaveCriticalSection(&g_cs);
    PostMessageW(n, WM_APP_PLUGIN, 0, 0);
}

static bool ReadAll(const std::wstring& f, std::vector<BYTE>& d) {
    HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    d.resize((size_t)sz.QuadPart);
    DWORD rd = 0;
    BOOL ok = d.empty() || ReadFile(h, d.data(), (DWORD)d.size(), &rd, nullptr);
    CloseHandle(h);
    return ok && rd == d.size();
}

static std::string Sha256Hex(const std::wstring& file) {
    std::vector<BYTE> d;
    if (!ReadAll(file, d)) return "";
    BYTE h[32];
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, d.data(), (ULONG)d.size(), h, 32) != 0) return "";
    static const char* hx = "0123456789abcdef";
    std::string s;
    for (BYTE b : h) { s += hx[b >> 4]; s += hx[b & 15]; }
    return s;
}

// Digitale Signatur gültig und vom Hersteller "Antibody Software"?
static bool SignedByAntibody(const std::wstring& file) {
    WINTRUST_FILE_INFO fi = {sizeof(fi)};
    fi.pcwszFilePath = file.c_str();
    WINTRUST_DATA wd = {sizeof(wd)};
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG r = WinVerifyTrust(nullptr, &action, &wd);
    bool ok = false;
    if (r == ERROR_SUCCESS) {
        CRYPT_PROVIDER_DATA* pd = WTHelperProvDataFromStateData(wd.hWVTStateData);
        CRYPT_PROVIDER_SGNR* sg = pd ? WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0) : nullptr;
        CRYPT_PROVIDER_CERT* cert = sg ? WTHelperGetProvCertFromChain(sg, 0) : nullptr;
        if (cert && cert->pCert) {
            wchar_t name[256] = {};
            CertGetNameStringW(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256);
            std::wstring n = name;
            for (auto& c : n) c = (wchar_t)towlower(c);
            ok = n.find(L"antibody") != std::wstring::npos;
        }
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &wd);
    return ok;
}

struct Progress : public IBindStatusCallback {
    HWND n;
    explicit Progress(HWND h) : n(h) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** p) override {
        if (r == IID_IUnknown || r == IID_IBindStatusCallback) { *p = this; return S_OK; }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE OnStartBinding(DWORD, IBinding*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPriority(LONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnLowResource(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnProgress(ULONG done, ULONG total, ULONG, LPCWSTR) override {
        if (total > 0) Set(n, PLUG_DOWNLOADING, L"", (int)((unsigned long long)done * 100 / total));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnStopBinding(HRESULT, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBindInfo(DWORD* f, BINDINFO*) override { *f = BINDF_GETNEWESTVERSION | BINDF_NOWRITECACHE; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDataAvailable(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnObjectAvailable(REFIID, IUnknown*) override { return S_OK; }
};

static bool RunWait(const std::wstring& cmd) {
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> b(cmd.begin(), cmd.end());
    b.push_back(0);
    if (!CreateProcessW(nullptr, b.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}

static bool DeleteTree(const std::wstring& dir) {
    std::wstring from = dir + L'\0';  // doppelt nullterminiert
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NO_UI;
    return SHFileOperationW(&op) == 0;
}

static DWORD WINAPI InstallThread(LPVOID p) {
    HWND n = (HWND)p;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wstring dir = Dir(), base = dir.substr(0, dir.find_last_of(L'\\'));
    SHCreateDirectoryExW(nullptr, base.c_str(), nullptr);
    std::wstring stamp = L"?t=" + std::to_wstring(GetTickCount64());
    std::wstring page = base + L"\\wiztree_download.html", zip = base + L"\\wiztree_portable.zip", tmp = base + L"\\WizTree.neu";
    std::wstring err;
    do {
        // 1. aktuellen Link zur portablen Version auf der offiziellen Download-Seite suchen
        Set(n, PLUG_WORKING, L"Suche die aktuelle Version …");
        if (FAILED(URLDownloadToFileW(nullptr, (L"https://diskanalyzer.com/download" + stamp).c_str(), page.c_str(), 0, nullptr))) {
            err = L"diskanalyzer.com nicht erreichbar.";
            break;
        }
        std::vector<BYTE> html;
        ReadAll(page, html);
        DeleteFileW(page.c_str());
        std::string h(html.begin(), html.end());
        size_t k = h.find("_portable.zip");
        size_t a = k == std::string::npos ? k : h.rfind("files/wiztree_", k);
        if (a == std::string::npos || k - a > 40) {
            err = L"Download-Link nicht gefunden.";
            break;
        }
        std::string file = h.substr(a, k + 13 - a);  // "files/wiztree_4_34_portable.zip"
        // Teil-Prüfsumme neben dem Link: "SHA256: 6CA08AF1…922DAAB2" (Anfang und Ende)
        std::string pre, post;
        size_t s = h.find("SHA256:", k);
        if (s != std::string::npos && s - k < 600) {
            size_t i = s + 7;
            while (i < h.size() && h[i] == ' ') i++;
            while (i < h.size() && isxdigit((unsigned char)h[i])) pre += (char)tolower(h[i++]);
            while (i < h.size() && !isxdigit((unsigned char)h[i]) && i - s < 40) i++;
            while (i < h.size() && isxdigit((unsigned char)h[i])) post += (char)tolower(h[i++]);
        }
        // 2. herunterladen
        Set(n, PLUG_DOWNLOADING, L"", 0);
        Progress cb(n);
        std::wstring url = L"https://diskanalyzer.com/" + std::wstring(file.begin(), file.end());
        if (FAILED(URLDownloadToFileW(nullptr, (url + stamp).c_str(), zip.c_str(), 0, &cb))) {
            err = L"Herunterladen fehlgeschlagen.";
            break;
        }
        // 3. Prüfsumme (soweit auf der Seite angegeben)
        Set(n, PLUG_WORKING, L"Prüfe die Datei …");
        std::string sha = Sha256Hex(zip);
        if (pre.size() >= 6 && (sha.compare(0, pre.size(), pre) != 0 || (post.size() >= 6 && sha.compare(sha.size() - post.size(), post.size(), post) != 0))) {
            err = L"Prüfsumme passt nicht – Datei verworfen.";
            break;
        }
        // 4. entpacken (in einen neuen Ordner) und Signatur prüfen
        DeleteTree(tmp);
        SHCreateDirectoryExW(nullptr, tmp.c_str(), nullptr);
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        if (!RunWait(L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + zip + L"\" -C \"" + tmp + L"\"")) {
            err = L"Entpacken fehlgeschlagen.";
            break;
        }
        if (!SignedByAntibody(tmp + L"\\WizTree64.exe")) {
            err = L"Digitale Signatur fehlt oder stimmt nicht – nicht installiert.";
            break;
        }
        // 5. alten Stand ersetzen
        DeleteTree(dir);
        if (!MoveFileExW(tmp.c_str(), dir.c_str(), 0)) {
            err = L"Konnte nicht installieren (läuft WizTree noch?).";
            break;
        }
    } while (false);
    DeleteFileW(zip.c_str());
    DeleteTree(tmp);
    if (err.empty()) Set(n, PLUG_DONE, L"Installiert und geprüft.");
    else Set(n, PLUG_ERROR, err);
    EnterCriticalSection(&g_cs);
    g_busy = false;
    LeaveCriticalSection(&g_cs);
    CoUninitialize();
    return 0;
}

void WizTree_Install(HWND notify) {
    InitCs();
    EnterCriticalSection(&g_cs);
    bool start = !g_busy;
    g_busy = true;
    LeaveCriticalSection(&g_cs);
    if (!start) return;
    HANDLE t = CreateThread(nullptr, 0, InstallThread, notify, 0, nullptr);
    if (t) CloseHandle(t);
    else { EnterCriticalSection(&g_cs); g_busy = false; LeaveCriticalSection(&g_cs); }
}

bool WizTree_Remove() {
    InitCs();
    bool ok = DeleteTree(Dir());
    EnterCriticalSection(&g_cs);
    g_st = PlugStatus();
    LeaveCriticalSection(&g_cs);
    return ok && WizTree_Exe().empty();
}

bool WizTree_Open(const std::wstring& folder) {
    std::wstring exe = WizTree_Exe();
    if (exe.empty()) return false;
    std::wstring f = folder;
    if (!f.empty() && f.back() == L'\\') f += L'\\';  // "C:\" -> "C:\\", sonst wird das Anführungszeichen geschluckt
    std::wstring arg = L"\"" + f + L"\"";
    // WizTree fragt selbst nach Administratorrechten (schnelles Lesen der Laufwerkstabelle) – das ist Windows' eigene Abfrage
    return (INT_PTR)ShellExecuteW(nullptr, L"open", exe.c_str(), arg.c_str(), nullptr, SW_SHOWNORMAL) > 32;
}
