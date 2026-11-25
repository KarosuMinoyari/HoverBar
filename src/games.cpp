// Spiele-Statistiken: Steam (Spielzeit, zuletzt gespielt, Größe), Epic (Größe), eigene Messung der Spielzeit.
#define NOMINMAX
#include "games.h"
#include <shlobj.h>
#include <psapi.h>
#include <map>
#include <set>
#include <ctime>
#include <algorithm>
#include <cwctype>

static HWND g_notify = nullptr;
static std::wstring g_ini;
static CRITICAL_SECTION g_cs;
static bool g_csInit = false;
static std::set<std::wstring> g_sizePending;  // Ordnergrößen, die gerade berechnet werden

static std::wstring Low(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

static bool ReadBytes(const std::wstring& f, std::string& out) {
    HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    if (sz.QuadPart > 64 * 1024 * 1024) { CloseHandle(h); return false; }
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0;
    BOOL ok = out.empty() || ReadFile(h, &out[0], (DWORD)out.size(), &rd, nullptr);
    CloseHandle(h);
    return ok && rd == out.size();
}

static std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// ---------------------------------------------------------------- Steam (VDF-Dateien)

struct SteamApp {
    std::wstring dir;  // Installationsordner (klein, mit "\")
    long long size = -1;
    int minutes = -1;
    long long last = 0;
};
static std::map<std::wstring, SteamApp> g_steam;

struct EpicApp {
    std::wstring appName, dir;  // dir klein geschrieben, mit "\" am Ende
    long long size = -1;
};
static std::vector<EpicApp> g_epic;
static ULONGLONG g_loadedAt = 0;

// einfacher VDF-Leser: ruft cb(Pfad der Abschnitte, Schlüssel, Wert) für jedes Paar auf
template <class F>
static void ParseVdf(const std::string& txt, F cb) {
    std::vector<std::wstring> stack;
    std::wstring key;
    bool haveKey = false;
    size_t i = 0, n = txt.size();
    while (i < n) {
        char c = txt[i];
        if (c == '"') {
            std::string s;
            for (i++; i < n && txt[i] != '"'; i++) {
                if (txt[i] == '\\' && i + 1 < n) i++;
                s += txt[i];
            }
            i++;
            std::wstring w = FromUtf8(s);
            if (!haveKey) { key = w; haveKey = true; }
            else { cb(stack, key, w); haveKey = false; }
        } else if (c == '{') {
            stack.push_back(haveKey ? key : L"");
            haveKey = false;
            i++;
        } else if (c == '}') {
            if (!stack.empty()) stack.pop_back();
            haveKey = false;
            i++;
        } else {
            i++;
        }
    }
}

static std::wstring JsonString(const std::string& j, const char* field) {
    std::string k = std::string("\"") + field + "\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return L"";
    p = j.find(':', p + k.size());
    if (p == std::string::npos) return L"";
    p = j.find('"', p);
    if (p == std::string::npos) return L"";
    std::string s;
    for (p++; p < j.size() && j[p] != '"'; p++) {
        if (j[p] == '\\' && p + 1 < j.size()) p++;
        s += j[p];
    }
    return FromUtf8(s);
}

static long long JsonNumber(const std::string& j, const char* field) {
    std::string k = std::string("\"") + field + "\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return -1;
    p = j.find(':', p + k.size());
    if (p == std::string::npos) return -1;
    while (p < j.size() && (j[p] == ':' || j[p] == ' ' || j[p] == '"')) p++;
    return _atoi64(j.c_str() + p);
}

static void LoadEpicSessions();
static void LoadLaunchers() {
    if (g_loadedAt && GetTickCount64() - g_loadedAt < 30000) return;  // höchstens alle 30 s neu lesen
    g_loadedAt = GetTickCount64();
    g_steam.clear();
    g_epic.clear();

    wchar_t sp[MAX_PATH] = {};
    DWORD cb = sizeof(sp);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, sp, &cb) == ERROR_SUCCESS) {
        std::wstring steam = sp;
        std::replace(steam.begin(), steam.end(), L'/', L'\\');
        std::string txt;
        if (ReadBytes(steam + L"\\steamapps\\libraryfolders.vdf", txt)) {
            std::map<std::wstring, std::wstring> libPath;  // Bibliotheks-Nr. -> Pfad
            std::map<std::wstring, std::wstring> appLib;   // Spiel -> Bibliotheks-Nr.
            ParseVdf(txt, [&](const std::vector<std::wstring>& st, const std::wstring& k, const std::wstring& v) {
                if (!st.empty() && Low(st.back()) == L"apps") {
                    g_steam[k].size = _wtoi64(v.c_str());
                    if (st.size() >= 2) appLib[k] = st[st.size() - 2];
                } else if (st.size() == 2 && Low(k) == L"path") {
                    libPath[st[1]] = v;
                }
            });
            // Installationsordner je Spiel (steamapps\common\<installdir>), um laufende Spiele zu erkennen
            for (auto& a : appLib) {
                std::wstring lib = libPath[a.second];
                std::replace(lib.begin(), lib.end(), L'/', L'\\');
                std::string acf;
                if (lib.empty() || !ReadBytes(lib + L"\\steamapps\\appmanifest_" + a.first + L".acf", acf)) continue;
                std::wstring inst;
                ParseVdf(acf, [&](const std::vector<std::wstring>&, const std::wstring& k, const std::wstring& v) {
                    if (Low(k) == L"installdir") inst = v;
                });
                if (!inst.empty()) g_steam[a.first].dir = Low(lib + L"\\steamapps\\common\\" + inst + L"\\");
            }
        }
        WIN32_FIND_DATAW fd;
        HANDLE fh = FindFirstFileW((steam + L"\\userdata\\*").c_str(), &fd);
        if (fh != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
                if (!ReadBytes(steam + L"\\userdata\\" + fd.cFileName + L"\\config\\localconfig.vdf", txt)) continue;
                ParseVdf(txt, [](const std::vector<std::wstring>& st, const std::wstring& k, const std::wstring& v) {
                    size_t n = st.size();
                    if (n < 3 || Low(st[n - 2]) != L"apps" || Low(st[n - 3]) != L"steam") return;
                    std::wstring lk = Low(k);
                    SteamApp& a = g_steam[st[n - 1]];
                    if (lk == L"playtime") a.minutes = std::max(a.minutes, _wtoi(v.c_str()));
                    else if (lk == L"lastplayed") a.last = std::max(a.last, _wtoi64(v.c_str()));
                });
            } while (FindNextFileW(fh, &fd));
            FindClose(fh);
        }
    }

    if (Games_SourceOn(1)) LoadEpicSessions();
    // Epic: ein Manifest (*.item, JSON) je installiertem Spiel
    std::wstring man = L"C:\\ProgramData\\Epic\\EpicGamesLauncher\\Data\\Manifests\\";
    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileW((man + L"*.item").c_str(), &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            std::string j;
            if (!ReadBytes(man + fd.cFileName, j)) continue;
            EpicApp e;
            e.appName = JsonString(j, "AppName");
            e.dir = Low(JsonString(j, "InstallLocation"));
            std::replace(e.dir.begin(), e.dir.end(), L'/', L'\\');
            if (!e.dir.empty() && e.dir.back() != L'\\') e.dir += L'\\';
            e.size = JsonNumber(j, "InstallSize");
            if (!e.appName.empty()) g_epic.push_back(e);
        } while (FindNextFileW(fh, &fd));
        FindClose(fh);
    }
}

// Epic: Die Gesamt-Spielzeit liegt nur online. Der Launcher protokolliert aber jede Sitzung
// ("Started/Stopped tracking playtime for <ns>:<id>:<AppName>"). Abgeschlossene Sitzungen werden
// einmalig aufaddiert ([EpicSpielzeit] in der ini), so wächst die Zahl mit jeder Sitzung weiter.
static long long LogTime(const std::string& s) {  // "2026.11.22-02.01.31" (UTC) -> Unix-Zeit
    tm t = {};
    if (sscanf_s(s.c_str(), "%d.%d.%d-%d.%d.%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) != 6) return 0;
    t.tm_year -= 1900;
    t.tm_mon -= 1;
    return (long long)_mkgmtime(&t);
}

static void LoadEpicSessions() {
    wchar_t la[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) return;
    std::wstring dir = std::wstring(la) + L"\\EpicGamesLauncher\\Saved\\Logs\\";
    struct Sess { std::string start; std::string stop; };
    std::map<std::string, std::map<std::string, Sess>> byApp;  // AppName -> Start -> Sitzung
    std::map<std::string, std::string> open;                     // AppName -> Start der laufenden Sitzung
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"EpicGamesLauncher*.log").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do files.push_back(fd.cFileName); while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(files.begin(), files.end());
    static const char key[] = "LogPortalPublishTime: ";
    for (auto& f : files) {
        std::string txt;
        if (!ReadBytes(dir + f, txt)) continue;
        for (size_t p = 0; (p = txt.find(key, p)) != std::string::npos; p += sizeof(key)) {
            size_t ls = txt.rfind('\n', p);
            ls = ls == std::string::npos ? 0 : ls + 1;
            if (txt.compare(ls, 1, "[") != 0) continue;
            std::string ts = txt.substr(ls + 1, 19);
            size_t le = txt.find_first_of("\r\n", p);
            std::string msg = txt.substr(p + sizeof(key) - 1, (le == std::string::npos ? txt.size() : le) - p - sizeof(key) + 1);
            bool started = msg.rfind("Started tracking playtime for ", 0) == 0;
            bool stopped = msg.rfind("Stopped tracking playtime for ", 0) == 0;
            if (!started && !stopped) continue;
            std::string app = msg.substr(msg.find_last_of(':') + 1);
            if (started) open[app] = ts;
            else if (open.count(app)) {
                byApp[app][open[app]] = {open[app], ts};
                open.erase(app);
            }
        }
    }
    for (auto& a : byApp) {
        std::wstring app = FromUtf8(a.first);
        wchar_t last[32] = {};
        GetPrivateProfileStringW(L"EpicGezaehlt", app.c_str(), L"", last, 32, g_ini.c_str());
        std::string lastS;
        for (wchar_t* c = last; *c; c++) lastS += (char)*c;
        int add = 0;
        std::string newest = lastS;
        long long lastStop = 0;
        for (auto& s : a.second) {
            lastStop = std::max(lastStop, LogTime(s.second.stop));
            if (!lastS.empty() && s.first <= lastS) continue;  // schon gezählt
            long long d = LogTime(s.second.stop) - LogTime(s.second.start);
            if (d > 0 && d < 24 * 3600) add += (int)((d + 30) / 60);
            newest = std::max(newest, s.first);
        }
        if (add > 0) {
            int m = GetPrivateProfileIntW(L"EpicSpielzeit", app.c_str(), 0, g_ini.c_str()) + add;
            WritePrivateProfileStringW(L"EpicSpielzeit", app.c_str(), std::to_wstring(m).c_str(), g_ini.c_str());
        }
        if (newest != lastS) WritePrivateProfileStringW(L"EpicGezaehlt", app.c_str(), FromUtf8(newest).c_str(), g_ini.c_str());
        wchar_t lp[32] = {};
        GetPrivateProfileStringW(L"EpicZuletzt", app.c_str(), L"0", lp, 32, g_ini.c_str());
        long long prev = _wtoi64(lp);
        if (lastStop > prev) WritePrivateProfileStringW(L"EpicZuletzt", app.c_str(), std::to_wstring(lastStop).c_str(), g_ini.c_str());
    }
}

// Epic-Spielname aus einer Verknüpfung: com.epicgames.launcher://apps/<ns>%3A<id>%3A<AppName>?action=launch
static std::wstring EpicAppName(const std::wstring& id, const std::wstring& file) {
    std::wstring url;
    if (Low(id).find(L"com.epicgames.launcher://") == 0) url = id;
    std::wstring f = !file.empty() ? file : id;
    if (url.empty() && Low(f).size() > 4 && Low(f).substr(Low(f).size() - 4) == L".url") {
        std::string txt;
        if (ReadBytes(f, txt)) {
            size_t p = txt.find("URL=");
            if (p != std::string::npos) url = FromUtf8(txt.substr(p + 4, txt.find_first_of("\r\n", p) - p - 4));
        }
    }
    size_t a = Low(url).find(L"/apps/");
    if (url.find(L"com.epicgames.launcher://") != 0 || a == std::wstring::npos) return L"";
    std::wstring s = url.substr(a + 6, url.find(L'?', a) == std::wstring::npos ? std::wstring::npos : url.find(L'?', a) - a - 6);
    for (size_t k; (k = Low(s).find(L"%3a")) != std::wstring::npos;) s.replace(k, 3, L":");
    size_t c = s.find_last_of(L':');
    return c == std::wstring::npos ? s : s.substr(c + 1);
}

static const EpicApp* FindEpic(const std::wstring& id, const std::wstring& file) {
    std::wstring an = EpicAppName(id, file);
    if (an.empty()) return nullptr;
    for (auto& e : g_epic)
        if (!_wcsicmp(e.appName.c_str(), an.c_str())) return &e;
    return nullptr;
}

// Ordner einer .exe-Kennung ("D:\Spiel\x.exe" oder "{KnownFolder-GUID}\Pfad\x.exe")
static std::wstring ExeDir(const std::wstring& id) {
    std::wstring path;
    if (id.size() > 40 && id[0] == L'{' && id[38] == L'}' && id[39] == L'\\') {
        GUID g;
        PWSTR kf = nullptr;
        if (SUCCEEDED(CLSIDFromString(id.substr(0, 39).c_str(), &g)) && SUCCEEDED(SHGetKnownFolderPath(g, 0, nullptr, &kf))) {
            path = std::wstring(kf) + id.substr(39);
        }
        if (kf) CoTaskMemFree(kf);
    } else if (id.size() > 3 && id[1] == L':' && id[2] == L'\\') {
        path = id;
    }
    std::wstring low = Low(path);
    if (low.size() < 5 || low.substr(low.size() - 4) != L".exe") return L"";
    size_t s = low.find_last_of(L'\\');
    return s == std::wstring::npos || s < 3 ? L"" : low.substr(0, s + 1);  // nicht ganze Laufwerke
}

// Riot (VALORANT, League of Legends, TFT): Installationsordner aus den Produkt-Einstellungen, passend zum App-Namen
static std::wstring RiotDir(const std::wstring& name) {
    std::wstring ln = Low(name);
    std::wstring base = L"C:\\ProgramData\\Riot Games\\Metadata\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + L"*.live").c_str(), &fd);
    std::wstring dir;
    if (h == INVALID_HANDLE_VALUE) return dir;
    do {
        std::wstring prod = Low(fd.cFileName);
        prod = prod.substr(0, prod.find(L'.'));
        std::replace(prod.begin(), prod.end(), L'_', L' ');
        if (prod.empty() || ln.find(prod) == std::wstring::npos) continue;
        std::string y;
        if (!ReadBytes(base + fd.cFileName + L"\\" + fd.cFileName + L".product_settings.yaml", y)) continue;
        size_t p = y.find("product_install_full_path:");
        if (p == std::string::npos) continue;
        size_t a = y.find('"', p), b = a == std::string::npos ? a : y.find('"', a + 1);
        if (b == std::string::npos) continue;
        dir = Low(FromUtf8(y.substr(a + 1, b - a - 1)));
        std::replace(dir.begin(), dir.end(), L'/', L'\\');
        if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
        break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return dir;
}

// Installationsordner für Spiele ohne Launcher-Daten (.exe, Riot, Velopack-Apps wie osu!)
static std::wstring GameDir(const GameRef& g) {
    std::wstring d = ExeDir(g.id);
    if (!d.empty()) return d;
    std::wstring lid = Low(g.id);
    if (lid.find(L"velopack.") == 0) {  // z. B. velopack.osulazer -> %LOCALAPPDATA%\osulazer
        wchar_t la[MAX_PATH];
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) {
            std::wstring p = Low(std::wstring(la) + L"\\" + g.id.substr(9) + L"\\");
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
        }
    }
    return RiotDir(g.name);
}

static std::wstring IniGet(const wchar_t* sec, const std::wstring& key) {
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(sec, key.c_str(), L"", buf, 64, g_ini.c_str());
    return buf;
}

// ---------------------------------------------------------------- Ordnergröße im Hintergrund

static long long DirSize(const std::wstring& dir, int depth) {
    if (depth > 40) return 0;
    long long total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2]))) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += DirSize(dir + fd.cFileName + L"\\", depth + 1);
        else total += ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

// Ordnergrößen nacheinander in einem einzigen Hintergrund-Thread messen (nicht viele gleichzeitig)
struct SizeJob {
    std::wstring id, dir;
};
static std::vector<SizeJob> g_sizeQ;
static bool g_sizeWorker = false;
static DWORD WINAPI SizeWorker(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    for (;;) {
        SizeJob j;
        EnterCriticalSection(&g_cs);
        if (g_sizeQ.empty()) {
            g_sizeWorker = false;
            LeaveCriticalSection(&g_cs);
            return 0;
        }
        j = g_sizeQ.front();
        g_sizeQ.erase(g_sizeQ.begin());
        LeaveCriticalSection(&g_cs);
        long long sz = DirSize(j.dir, 0);
        std::wstring v = std::to_wstring(sz) + L"|" + std::to_wstring((long long)time(nullptr));
        WritePrivateProfileStringW(L"SpielGroesse", j.id.c_str(), v.c_str(), g_ini.c_str());
        EnterCriticalSection(&g_cs);
        g_sizePending.erase(j.id);
        LeaveCriticalSection(&g_cs);
        PostMessageW(g_notify, WM_APP_GAMES, 0, 0);
    }
}

static std::wstring IniGet(const wchar_t* sec, const std::wstring& key);
static long long g_sizeForceAfter = 0;  // ältere Messungen gelten nach "Aktualisieren" als veraltet
// Gespeicherte Ordnergröße (einmal am Tag neu gemessen); -1 = wird gerade gemessen
static long long CachedDirSize(const std::wstring& id, const std::wstring& dir) {
    std::wstring c = IniGet(L"SpielGroesse", id);
    long long size = -1, when = 0;
    if (!c.empty()) {
        size = _wtoi64(c.c_str());
        size_t bar = c.find(L'|');
        if (bar != std::wstring::npos) when = _wtoi64(c.c_str() + bar + 1);
    }
    if (c.empty() || time(nullptr) - when > 24 * 3600 || when < g_sizeForceAfter) {  // täglich oder nach "Aktualisieren"
        EnterCriticalSection(&g_cs);
        if (g_sizePending.insert(id).second) g_sizeQ.push_back({id, dir});
        bool start = !g_sizeWorker && !g_sizeQ.empty();
        if (start) g_sizeWorker = true;
        LeaveCriticalSection(&g_cs);
        if (start) {
            HANDLE t = CreateThread(nullptr, 0, SizeWorker, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
            else { EnterCriticalSection(&g_cs); g_sizeWorker = false; LeaveCriticalSection(&g_cs); }
        }
    }
    return size;
}

// ---------------------------------------------------------------- öffentlich

static const wchar_t* kSrcKey[4] = {L"SpieleSteam", L"SpieleEpic", L"SpieleRiot", L"SpieleMessen"};
bool Games_SourceOn(int src) {
    return src < 0 || src > 3 || GetPrivateProfileIntW(L"Funktionen", kSrcKey[src], 1, g_ini.c_str()) != 0;
}
void Games_SetSourceOn(int src, bool on) {
    if (src < 0 || src > 3) return;
    WritePrivateProfileStringW(L"Funktionen", kSrcKey[src], on ? L"1" : L"0", g_ini.c_str());
    g_loadedAt = 0;  // neu einlesen
}
int Games_ManualMinutes(const std::wstring& id) {
    return GetPrivateProfileIntW(L"SpielzeitManuell", id.c_str(), -1, g_ini.c_str());
}
void Games_SetManualMinutes(const std::wstring& id, int minutes) {
    WritePrivateProfileStringW(L"SpielzeitManuell", id.c_str(), minutes < 0 ? nullptr : std::to_wstring(minutes).c_str(), g_ini.c_str());
}
bool Games_IsSteam(const std::wstring& id) { return Low(id).find(L"steam://") == 0; }

void Games_Init(HWND notify, const std::wstring& ini) {
    g_notify = notify;
    g_ini = ini;
    if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = true; }
}

// Ordner eines Spiels ohne Launcher-Daten, unter Beachtung der Schalter (Riot bzw. andere Spiele)
static std::wstring WatchDir(const GameRef& g, bool* riot = nullptr) {
    if (riot) *riot = false;
    std::wstring d = ExeDir(g.id);
    std::wstring lid = Low(g.id);
    if (d.empty() && lid.find(L"velopack.") == 0) d = GameDir(g);
    if (!d.empty()) return Games_SourceOn(3) ? d : L"";
    d = RiotDir(g.name);
    if (!d.empty() && riot) *riot = true;
    return Games_SourceOn(2) ? d : L"";
}

GameStats Games_Get(const GameRef& g) {
    const std::wstring& id = g.id;
    const std::wstring& file = g.file;
    GameStats st;
    LoadLaunchers();
    if (Games_IsSteam(id)) {
        if (!Games_SourceOn(0)) return st;
        auto it = g_steam.find(id.substr(18));
        if (it != g_steam.end()) {
            st.size = it->second.size;
            st.minutes = std::max(0, it->second.minutes);
            st.lastPlayed = it->second.last;
        } else {
            st.minutes = 0;
        }
        return st;
    }
    int manual = Games_ManualMinutes(id);
    auto addManual = [&]() {
        if (manual >= 0) st.minutes = std::max(0, st.minutes) + manual;
    };
    if (const EpicApp* e = FindEpic(id, file)) {
        if (Games_SourceOn(1)) {
            st.size = e->size;
            // Spielzeit aus den Launcher-Protokollen (seit dem ältesten noch vorhandenen Protokoll)
            st.minutes = GetPrivateProfileIntW(L"EpicSpielzeit", e->appName.c_str(), 0, g_ini.c_str());
            wchar_t lp[32] = {};
            GetPrivateProfileStringW(L"EpicZuletzt", e->appName.c_str(), L"0", lp, 32, g_ini.c_str());
            st.lastPlayed = _wtoi64(lp);
        }
        addManual();
        return st;
    }
    std::wstring dir = WatchDir(g);
    if (!dir.empty()) {
        st.size = CachedDirSize(id, dir);  // einmal am Tag neu messen
        st.measured = true;  // selbst gemessen
        st.minutes = GetPrivateProfileIntW(L"Spielzeit", id.c_str(), 0, g_ini.c_str());
        st.lastPlayed = _wtoi64(IniGet(L"ZuletztGespielt", id).c_str());
    }
    addManual();
    if (manual > 0) st.measured = false;  // "misst ab jetzt" nicht mehr nötig
    return st;
}
static std::vector<std::pair<std::wstring, std::wstring>> g_watch;  // id, Ordner (klein, mit "\")

void Games_SetWatched(const std::vector<GameRef>& games) {
    LoadLaunchers();
    g_watch.clear();
    for (auto& p : games) {
        if (Low(p.id).find(L"steam://") == 0) continue;  // Steam zählt selbst
        if (FindEpic(p.id, p.file)) continue;  // Epic: Spielzeit kommt aus den Launcher-Protokollen
        std::wstring dir = WatchDir(p);
        if (dir.size() > 3) g_watch.push_back({p.id, dir});
    }
}

void Games_Tick() {
    if (g_watch.empty()) return;
    DWORD pids[2048], got = 0;
    if (!EnumProcesses(pids, sizeof(pids), &got)) return;
    std::set<std::wstring> running;
    for (DWORD k = 0; k < got / sizeof(DWORD); k++) {
        HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[k]);
        if (!ph) continue;
        wchar_t path[MAX_PATH];
        DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(ph, 0, path, &len)) {
            std::wstring low = Low(path);
            for (auto& w : g_watch)
                if (low.compare(0, w.second.size(), w.second) == 0) running.insert(w.first);
        }
        CloseHandle(ph);
    }
    std::wstring now = std::to_wstring((long long)time(nullptr));
    for (auto& id : running) {
        int m = GetPrivateProfileIntW(L"Spielzeit", id.c_str(), 0, g_ini.c_str()) + 1;
        WritePrivateProfileStringW(L"Spielzeit", id.c_str(), std::to_wstring(m).c_str(), g_ini.c_str());
        WritePrivateProfileStringW(L"ZuletztGespielt", id.c_str(), now.c_str(), g_ini.c_str());
    }
}

// ---------------------------------------------------------------- Größe beliebiger Apps (Liste "Alle Apps")

struct UninstEntry {
    std::wstring name, dir;  // klein; dir mit "\" am Ende
    long long bytes;
};
static std::vector<UninstEntry> g_uninst;
static ULONGLONG g_uninstAt = 0;

static std::wstring RegStr(HKEY k, const wchar_t* v) {
    wchar_t buf[1024];
    DWORD cb = sizeof(buf);
    if (RegGetValueW(k, nullptr, v, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buf, &cb) != ERROR_SUCCESS) return L"";
    return buf;
}

// Windows-Liste der installierten Programme (wie in den Einstellungen): Name, Ordner, geschätzte Größe
static void LoadUninstall() {
    if (g_uninstAt && GetTickCount64() - g_uninstAt < 60000) return;
    g_uninstAt = GetTickCount64();
    g_uninst.clear();
    struct Root { HKEY h; REGSAM flag; } roots[] = {{HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY}, {HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY}, {HKEY_CURRENT_USER, 0}};
    for (auto& r : roots) {
        HKEY base;
        if (RegOpenKeyExW(r.h, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_READ | r.flag, &base) != ERROR_SUCCESS) continue;
        wchar_t sub[256];
        for (DWORD i = 0;; i++) {
            DWORD n = 256;
            if (RegEnumKeyExW(base, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            HKEY k;
            if (RegOpenKeyExW(base, sub, 0, KEY_READ | r.flag, &k) != ERROR_SUCCESS) continue;
            UninstEntry e;
            e.name = Low(RegStr(k, L"DisplayName"));
            e.dir = Low(RegStr(k, L"InstallLocation"));
            if (e.dir.empty()) {  // sonst aus dem Symbol-Pfad ableiten: "C:\...\app.exe",0
                std::wstring ic = Low(RegStr(k, L"DisplayIcon"));
                size_t comma = ic.rfind(L',');
                if (comma != std::wstring::npos && comma > 3) ic = ic.substr(0, comma);
                ic.erase(std::remove(ic.begin(), ic.end(), L'"'), ic.end());
                size_t s = ic.find_last_of(L'\\');
                if (s != std::wstring::npos && ic.size() > 4 && ic.substr(ic.size() - 4) == L".exe") e.dir = ic.substr(0, s + 1);
            }
            e.dir.erase(std::remove(e.dir.begin(), e.dir.end(), L'"'), e.dir.end());
            if (!e.dir.empty() && e.dir.back() != L'\\') e.dir += L'\\';
            DWORD kb = 0, cb = sizeof(kb);
            e.bytes = RegGetValueW(k, nullptr, L"EstimatedSize", RRF_RT_REG_DWORD, nullptr, &kb, &cb) == ERROR_SUCCESS ? (long long)kb * 1024 : -1;
            RegCloseKey(k);
            if (!e.name.empty()) g_uninst.push_back(e);
        }
        RegCloseKey(base);
    }
}

long long Apps_Size(const GameRef& g, const std::wstring& target) {
    std::wstring lid = Low(g.id);
    LoadLaunchers();
    // Spiele-Launcher kennen die Größe genau
    if (Games_IsSteam(g.id) || FindEpic(g.id, g.file) || lid.find(L"velopack.") == 0) {
        GameStats s = Games_Get(g);
        if (s.size >= 0) return s.size;
    }
    std::wstring dir = ExeDir(g.id);
    if (dir.empty() && !target.empty()) dir = ExeDir(target);
    LoadUninstall();
    // 1. Installationsordner passt (der genaueste Eintrag gewinnt), 2. gleicher Name
    const UninstEntry* best = nullptr;
    if (!dir.empty())
        for (auto& e : g_uninst) {
            if (e.dir.size() < 4 || std::count(e.dir.begin(), e.dir.end(), L'\\') < 3) continue;  // nicht "C:\Program Files\"
            if (dir.compare(0, e.dir.size(), e.dir) == 0 && (!best || e.dir.size() > best->dir.size())) best = &e;
        }
    if (!best) {
        std::wstring ln = Low(g.name);
        for (auto& e : g_uninst)
            if (ln.size() >= 4 && (e.name == ln || e.name.rfind(ln + L" ", 0) == 0)) { best = &e; break; }
    }
    if (best && best->bytes > 0) return best->bytes;
    // 3. Ordner selbst messen (im Hintergrund, nicht für Windows-eigene Programme)
    if (dir.empty()) dir = RiotDir(g.name);
    if (best && !best->dir.empty() && best->dir.size() > 3) dir = best->dir;
    if (!dir.empty() && dir.find(L"c:\\windows\\") != 0 && std::count(dir.begin(), dir.end(), L'\\') >= 3)
        return CachedDirSize(g.id, dir);
    return -1;
}
// ---------------------------------------------------------------- Laufende Apps erkennen und beenden

// Woran ein laufender Prozess als diese App erkannt wird
AppMatcher Apps_Matcher(const GameRef& g, const std::wstring& target) {
    AppMatcher m;
    LoadLaunchers();
    std::wstring lid = Low(g.id);
    auto exePath = [](const std::wstring& id) -> std::wstring {
        std::wstring d = ExeDir(id);
        if (d.empty()) return L"";
        std::wstring low = Low(id);
        if (low.size() > 40 && low[0] == L'{') {  // {KnownFolder}\pfad\x.exe -> voller Pfad
            return d + low.substr(low.find_last_of(L'\\') + 1);
        }
        return low;
    };
    if (Games_IsSteam(g.id)) {
        auto it = g_steam.find(g.id.substr(18));
        if (it != g_steam.end()) m.dir = it->second.dir;
    } else if (const EpicApp* e = FindEpic(g.id, g.file)) {
        m.dir = e->dir;
    } else if (lid.find(L"velopack.") == 0) {
        m.dir = GameDir(g);
    } else if (g.id.find(L'!') != std::wstring::npos && g.id.find(L'\\') == std::wstring::npos) {
        // Store-App: PaketFamilie!App -> Prozesse unter ...\WindowsApps\<Name>_<Version>_..._<Hash> (Ordner)
        std::wstring fam = lid.substr(0, lid.find(L'!'));
        size_t us = fam.rfind(L'_');
        if (us != std::wstring::npos) { m.uwpName = fam.substr(0, us); m.uwpHash = fam.substr(us + 1); }
    } else {
        m.exe = exePath(g.id);
        if (m.exe.empty() && !target.empty()) m.exe = exePath(target);
        if (m.exe.empty()) m.dir = RiotDir(g.name);
    }
    // Windows-eigene Programme (Explorer, Rechner-Hilfsprogramme …) nie anbieten zu beenden
    if (m.exe.find(L"c:\\windows\\") == 0) m.system = true;
    if (m.dir.size() < 6 || std::count(m.dir.begin(), m.dir.end(), L'\\') < 3) m.dir.clear();  // keine ganzen Laufwerke/Programme-Ordner
    return m;
}

bool Apps_MatchProcess(const AppMatcher& m, const std::wstring& lowPath) {
    if (m.system || lowPath.empty()) return false;
    if (!m.exe.empty()) return lowPath == m.exe;
    if (!m.dir.empty()) return lowPath.compare(0, m.dir.size(), m.dir) == 0;
    if (!m.uwpName.empty()) {
        size_t w = lowPath.find(L"\\windowsapps\\");
        if (w == std::wstring::npos) return false;
        std::wstring pkg = lowPath.substr(w + 13, lowPath.find(L'\\', w + 13) - w - 13);
        return pkg.rfind(m.uwpName + L"_", 0) == 0 && pkg.size() >= m.uwpHash.size() &&
               pkg.compare(pkg.size() - m.uwpHash.size(), m.uwpHash.size(), m.uwpHash) == 0;
    }
    return false;
}

// Alle Prozesse, die HoverBar sehen darf (eigene des Benutzers), mit Pfad und Arbeitsspeicher
std::vector<RunProc> Apps_Processes() {
    std::vector<RunProc> out;
    DWORD pids[4096], got = 0;
    if (!EnumProcesses(pids, sizeof(pids), &got)) return out;
    DWORD self = GetCurrentProcessId();
    for (DWORD k = 0; k < got / sizeof(DWORD); k++) {
        if (pids[k] == self || pids[k] <= 4) continue;
        HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[k]);
        if (!ph) continue;
        wchar_t path[MAX_PATH];
        DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(ph, 0, path, &len)) {
            PROCESS_MEMORY_COUNTERS pmc = {sizeof(pmc)};
            SIZE_T ram = GetProcessMemoryInfo(ph, &pmc, sizeof(pmc)) ? pmc.WorkingSetSize : 0;
            out.push_back({pids[k], Low(path), ram});
        }
        CloseHandle(ph);
    }
    return out;
}

// Erst höflich schließen (wie das X im Fenster), nach timeoutMs: true, wenn alle beendet sind
static BOOL CALLBACK CloseWnd(HWND w, LPARAM lp) {
    auto* pids = (std::vector<DWORD>*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (std::find(pids->begin(), pids->end(), pid) != pids->end() && IsWindowVisible(w) && !GetWindow(w, GW_OWNER))
        PostMessageW(w, WM_CLOSE, 0, 0);
    return TRUE;
}
bool Apps_CloseGracefully(const std::vector<DWORD>& pids, int timeoutMs) {
    std::vector<DWORD> copy = pids;
    EnumWindows(CloseWnd, (LPARAM)&copy);
    std::vector<HANDLE> hs;
    for (DWORD p : pids)
        if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, p)) hs.push_back(h);
    bool all = true;
    if (!hs.empty()) all = WaitForMultipleObjects((DWORD)hs.size(), hs.data(), TRUE, timeoutMs) != WAIT_TIMEOUT;
    for (HANDLE h : hs) CloseHandle(h);
    return all;
}

// Erzwingen; false, wenn ein Prozess nicht beendet werden durfte (z. B. läuft mit Administratorrechten)
bool Apps_Kill(const std::vector<DWORD>& pids) {
    bool ok = true;
    for (DWORD p : pids) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, p);
        if (!h) {
            if (GetLastError() != ERROR_INVALID_PARAMETER) ok = false;  // ungültig = schon beendet
            continue;
        }
        if (!TerminateProcess(h, 1)) ok = false;
        WaitForSingleObject(h, 2000);
        CloseHandle(h);
    }
    return ok;
}
// "Aktualisieren": Launcher-Daten, Programmliste und Ordnergrößen neu einlesen bzw. neu messen
void Apps_RefreshSizes() {
    g_sizeForceAfter = (long long)time(nullptr);
    g_loadedAt = 0;
    g_uninstAt = 0;
}