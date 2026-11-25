#define NOMINMAX
#include "update.h"
#include <urlmon.h>
#include <bcrypt.h>
#include <algorithm>
#include <mutex>
#include <vector>

static std::mutex g_mu;
static UpdateStatus g_st;
static std::wstring g_url, g_hashUrl, g_hash;  // gefundene Download-Adresse und Prüfsumme
static bool g_busy = false;

static std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }  // Adressen/Versionen sind ASCII
static std::wstring ExePath() {
    wchar_t p[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, p, MAX_PATH * 2);
    return p;
}
static void Set(int state, const std::wstring& msg, int percent = -1) {
    std::lock_guard<std::mutex> l(g_mu);
    g_st.state = state;
    g_st.msg = msg;
    g_st.percent = percent;
}
static void Notify(HWND h) { PostMessageW(h, WM_APP_UPDATE, 0, 0); }

UpdateStatus Update_Status() {
    std::lock_guard<std::mutex> l(g_mu);
    return g_st;
}

static bool ReadFileText(const std::wstring& f, std::string& out) {
    HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(h, nullptr), got = 0;
    out.resize(sz);
    bool ok = sz < 4 * 1024 * 1024 && ReadFile(h, &out[0], sz, &got, nullptr);
    CloseHandle(h);
    out.resize(got);
    return ok;
}
static std::wstring TempFile(const wchar_t* name) {
    wchar_t t[MAX_PATH];
    GetTempPathW(MAX_PATH, t);
    return std::wstring(t) + name;
}
static std::wstring Stamp() { return L"?t=" + std::to_wstring(GetTickCount64()); }  // am Zwischenspeicher vorbei

// "v1.2.3" / "1.2" -> {1,2,3}
static std::vector<int> ParseVersion(const std::wstring& s) {
    std::vector<int> v;
    size_t i = 0;
    while (i < s.size() && !iswdigit(s[i])) i++;
    while (i < s.size()) {
        int n = 0;
        bool any = false;
        while (i < s.size() && iswdigit(s[i])) { n = n * 10 + (s[i] - L'0'); i++; any = true; }
        if (any) v.push_back(n);
        if (i < s.size() && s[i] == L'.') i++;
        else break;
    }
    v.resize(std::max<size_t>(3, v.size()));
    return v;
}
static bool Newer(const std::wstring& latest) { return ParseVersion(latest) > ParseVersion(HB_VERSION); }

// Wert nach "key":" in json ab Position from
static std::string JsonStr(const std::string& j, const std::string& key, size_t from, size_t* at = nullptr) {
    std::string pat = "\"" + key + "\":\"";
    size_t p = j.find(pat, from);
    if (p == std::string::npos) return "";
    p += pat.size();
    size_t e = j.find('"', p);
    if (e == std::string::npos) return "";
    if (at) *at = p;
    return j.substr(p, e - p);
}

// Release-Angaben holen: Version, Download-Adresse der HoverBar.exe, Prüfsumme
static bool QueryLatest(std::wstring& err) {
    std::wstring api = Widen(std::string("https://api.github.com/repos/") + HB_REPO + "/releases/latest") + Stamp();
    std::wstring tmp = TempFile(L"hoverbar_release.json");
    DeleteFileW(tmp.c_str());
    if (FAILED(URLDownloadToFileW(nullptr, api.c_str(), tmp.c_str(), 0, nullptr))) {
        err = L"Keine Verbindung zu GitHub (oder noch keine Version veröffentlicht).";
        return false;
    }
    std::string j;
    bool ok = ReadFileText(tmp, j);
    DeleteFileW(tmp.c_str());
    if (!ok || j.empty()) { err = L"Antwort von GitHub nicht lesbar."; return false; }
    std::string tag = JsonStr(j, "tag_name", 0);
    if (tag.empty()) { err = L"Keine veröffentlichte Version gefunden."; return false; }
    size_t p = j.find("\"name\":\"HoverBar.exe\"");
    if (p == std::string::npos) { err = L"Die Version enthält keine HoverBar.exe."; return false; }
    std::string url = JsonStr(j, "browser_download_url", p);
    std::string digest = JsonStr(j, "digest", p);
    size_t q = j.find("\"name\":\"HoverBar.exe.sha256\"");
    std::string hurl = q == std::string::npos ? "" : JsonStr(j, "browser_download_url", q);
    // nur Downloads von GitHub selbst
    if (url.rfind("https://github.com/", 0) != 0) { err = L"Unerwartete Download-Adresse."; return false; }
    std::string hash;
    if (digest.rfind("sha256:", 0) == 0) hash = digest.substr(7);
    std::lock_guard<std::mutex> l(g_mu);
    g_st.latest = Widen(tag);
    g_url = Widen(url);
    g_hashUrl = hurl.rfind("https://github.com/", 0) == 0 ? Widen(hurl) : L"";
    g_hash = Widen(hash);
    return true;
}

static std::wstring Sha256Of(const std::wstring& file) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hh = nullptr;
    std::wstring out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 && BCryptCreateHash(alg, &hh, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<BYTE> buf(1 << 16);
        DWORD got = 0;
        while (ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr) && got) BCryptHashData(hh, buf.data(), got, 0);
        BYTE d[32] = {};
        if (BCryptFinishHash(hh, d, 32, 0) == 0) {
            static const wchar_t* hx = L"0123456789abcdef";
            for (BYTE b : d) { out += hx[b >> 4]; out += hx[b & 15]; }
        }
    }
    if (hh) BCryptDestroyHash(hh);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(h);
    return out;
}

struct ProgressCb : IBindStatusCallback {
    HWND hwnd;
    explicit ProgressCb(HWND h) : hwnd(h) {}
    STDMETHODIMP QueryInterface(REFIID r, void** o) override {
        if (r == IID_IUnknown || r == IID_IBindStatusCallback) { *o = this; return S_OK; }
        *o = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP OnStartBinding(DWORD, IBinding*) override { return S_OK; }
    STDMETHODIMP GetPriority(LONG*) override { return E_NOTIMPL; }
    STDMETHODIMP OnLowResource(DWORD) override { return S_OK; }
    STDMETHODIMP OnProgress(ULONG now, ULONG total, ULONG, LPCWSTR) override {
        if (total > 0) {
            Set(UPD_DOWNLOADING, L"Lädt herunter …", (int)((unsigned long long)now * 100 / total));
            Notify(hwnd);
        }
        return S_OK;
    }
    STDMETHODIMP OnStopBinding(HRESULT, LPCWSTR) override { return S_OK; }
    STDMETHODIMP GetBindInfo(DWORD*, BINDINFO*) override { return S_OK; }
    STDMETHODIMP OnDataAvailable(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override { return S_OK; }
    STDMETHODIMP OnObjectAvailable(REFIID, IUnknown*) override { return S_OK; }
};

static bool Begin() {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_busy) return false;
    g_busy = true;
    return true;
}
static void End() {
    std::lock_guard<std::mutex> l(g_mu);
    g_busy = false;
}

void Update_Check(HWND notify) {
    if (!Begin()) return;
    Set(UPD_CHECKING, L"Suche nach Updates …");
    Notify(notify);
    HANDLE t = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        HWND h = (HWND)p;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::wstring err;
        if (!QueryLatest(err)) Set(UPD_ERROR, err);
        else {
            std::wstring latest = Update_Status().latest;
            if (Newer(latest)) Set(UPD_AVAILABLE, L"Neu: " + latest);
            else Set(UPD_UPTODATE, L"Du hast die neueste Version (" + std::wstring(HB_VERSION) + L").");
        }
        CoUninitialize();
        End();
        Notify(h);
        return 0;
    }, notify, 0, nullptr);
    if (t) CloseHandle(t);
    else { End(); Set(UPD_ERROR, L"Konnte nicht starten."); }
}

void Update_Install(HWND notify) {
    if (!Begin()) return;
    Set(UPD_DOWNLOADING, L"Lädt herunter …", 0);
    Notify(notify);
    HANDLE t = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        HWND h = (HWND)p;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::wstring exe = ExePath(), url, hashUrl, hash;
        {
            std::lock_guard<std::mutex> l(g_mu);
            url = g_url; hashUrl = g_hashUrl; hash = g_hash;
        }
        std::wstring dir = exe.substr(0, exe.find_last_of(L'\\')), tmp = exe + L".update";
        auto fail = [&](const std::wstring& m) {
            DeleteFileW(tmp.c_str());
            Set(UPD_ERROR, m);
        };
        do {
            if (url.empty()) { fail(L"Keine Download-Adresse."); break; }
            DeleteFileW(tmp.c_str());
            ProgressCb cb(h);
            if (FAILED(URLDownloadToFileW(nullptr, url.c_str(), tmp.c_str(), 0, &cb))) { fail(L"Download fehlgeschlagen."); break; }
            Set(UPD_VERIFYING, L"Prüfe Download …");
            Notify(h);
            if (hash.empty() && !hashUrl.empty()) {  // Prüfsumme aus der .sha256-Datei der Version
                std::wstring ht = TempFile(L"hoverbar_exe.sha256");
                std::string txt;
                if (SUCCEEDED(URLDownloadToFileW(nullptr, hashUrl.c_str(), ht.c_str(), 0, nullptr)) && ReadFileText(ht, txt) && txt.size() >= 64)
                    hash = Widen(txt.substr(0, 64));
                DeleteFileW(ht.c_str());
            }
            if (hash.size() != 64) { fail(L"Keine Prüfsumme zur Version – Update abgebrochen."); break; }
            std::wstring got = Sha256Of(tmp);
            std::wstring want = hash;
            for (auto& c : want) c = towlower(c);
            if (got != want) { fail(L"Prüfsumme stimmt nicht – Download verworfen."); break; }
            // muss eine ausführbare Datei sein
            unsigned char mz[2] = {};
            DWORD n = 0;
            HANDLE f = CreateFileW(tmp.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE) { ReadFile(f, mz, 2, &n, nullptr); CloseHandle(f); }
            if (n != 2 || mz[0] != 'M' || mz[1] != 'Z') { fail(L"Download ist keine Programmdatei."); break; }
            // laufende Datei lässt sich umbenennen, nicht überschreiben: alte zur Seite, neue an ihre Stelle
            std::wstring old = exe.substr(0, exe.size() - 4) + L".alt.exe";
            DeleteFileW(old.c_str());
            if (!MoveFileExW(exe.c_str(), old.c_str(), 0)) { fail(L"Kein Schreibrecht im Programmordner (" + dir + L")."); break; }
            if (!MoveFileExW(tmp.c_str(), exe.c_str(), 0)) {
                MoveFileExW(old.c_str(), exe.c_str(), 0);  // zurück
                fail(L"Konnte die neue Version nicht einsetzen.");
                break;
            }
            Set(UPD_READY, L"Fertig – zum Starten der neuen Version neu starten.");
        } while (false);
        CoUninitialize();
        End();
        Notify(h);
        return 0;
    }, notify, 0, nullptr);
    if (t) CloseHandle(t);
    else { End(); Set(UPD_ERROR, L"Konnte nicht starten."); }
}

void Update_Restart() {
    std::wstring exe = ExePath();
    std::wstring cmd = L"\"" + exe + L"\" --warte-auf " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

void Update_Cleanup() {
    std::wstring exe = ExePath();
    DeleteFileW((exe.substr(0, exe.size() - 4) + L".alt.exe").c_str());
    DeleteFileW((exe + L".update").c_str());
}
