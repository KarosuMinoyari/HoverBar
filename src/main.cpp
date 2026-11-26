// HoverBar – unsichtbarer App-Starter am rechten Bildschirmrand.
// Im Ruhezustand: ein Timer, der alle 120 ms die Mausposition prüft. Sonst nichts.
// Beim Öffnen werden die Apps geladen; nach dem Schließen wird alles wieder freigegeben.

#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <commdlg.h>
#include <shellscalingapi.h>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <timeapi.h>
#include <bluetoothapis.h>
#include <tlhelp32.h>
#include <urlmon.h>
#pragma comment(lib, "urlmon.lib")
#include <uiautomation.h>
#include "media.h"
#include "music.h"
#include "games.h"
#include "plugins.h"
#include "chat.h"
#include "update.h"
#include "qrcode.h"
#include <wincrypt.h>
#include <softpub.h>
#include <wintrust.h>
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")
#include <map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>

// ---------------------------------------------------------------- Konstanten

static const wchar_t* kClass = L"HoverBarWindow";
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunName = L"HoverBar";
// Cloud-Programm installieren (Musik-Sync): Stand für die Anzeige auf der Karte
static std::mutex sCloudMu;
static int sCloudState = 0;  // 0 nichts, 1 läuft, 2 fertig, 3 Fehler
static std::wstring sCloudMsg;

enum { WM_APP_LIST = WM_APP + 1, WM_APP_ICON, WM_APP_DONE, WM_APP_SHOW };
enum { WM_APP_SCROLLGO = WM_APP + 70, WM_APP_FAVICON };
static LARGE_INTEGER sScrollStart = {};
enum { T_POLL = 1, T_WATCH, T_FREE, T_CARET, T_REDRAW, T_DRAG, T_TIP, T_SCROLL, T_PLAYER, T_PIDLE, T_HOTKEY, T_ENDHOLD, T_GRAB, T_DUCK, T_GAMES, T_VOL, T_RUN, T_LYR, T_NPDROP, T_LYRBACK, T_CHATFREE, T_CLOUDWAIT, T_RELOAD, T_COUNT };
// Musik leiser, solange ein Browser/Video-Player Ton ausgibt
static double sDuck = 1.0;
static bool sDucked = false;
static int sDuckQuiet = 0;
static bool DuckEnabled();
static void StartDuckWatch(HWND hwnd) { if (DuckEnabled()) SetTimer(hwnd, T_DUCK, 300, nullptr); }
// Alt+Ende: markierten Link kopieren und als Song laden
static DWORD sClipSeq = 0;
static int sGrabTicks = 0;
static std::wstring sClipOld;
static bool sClipHad = false;
static bool ClipText(std::wstring& out) {
    bool ok = false;
    if (!OpenClipboard(nullptr)) return false;
    if (HANDLE hd = GetClipboardData(CF_UNICODETEXT))
        if (auto* t = (const wchar_t*)GlobalLock(hd)) { out = t; ok = true; GlobalUnlock(hd); }
    CloseClipboard();
    return ok;
}
static HWND sGrabFg = nullptr;
static int sGrabStage = 0, sGrabRetry = 0;  // 0 Auswahl kopieren, 1 Adressleiste lesen (mehrfach), 2 Notweg Strg+L, Strg+C
static std::wstring AsLink(std::wstring t);
// Adresse aus der Adressleiste des Browsers lesen (Bedienungshilfen/UI Automation, ohne Klicks).
// Nimmt das erste Eingabefeld, dessen Inhalt wie ein Link aussieht (Chrome-Browser bauen ihre Oberfläche für solche Abfragen erst nach der ersten Anfrage auf).
static std::wstring BrowserUrl(HWND fg) {
    std::wstring url;
    IUIAutomation* ua = nullptr;
    if (!fg || FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ua)))) return url;
    IUIAutomationElement* root = nullptr;
    if (SUCCEEDED(ua->ElementFromHandle(fg, &root)) && root) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = UIA_EditControlTypeId;
        IUIAutomationCondition* c = nullptr;
        IUIAutomationElementArray* arr = nullptr;
        if (SUCCEEDED(ua->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &c)) && c &&
            SUCCEEDED(root->FindAll(TreeScope_Descendants, c, &arr)) && arr) {
            int n = 0;
            arr->get_Length(&n);
            for (int i = 0; i < n && i < 12 && url.empty(); i++) {
                IUIAutomationElement* e = nullptr;
                if (FAILED(arr->GetElement(i, &e)) || !e) continue;
                VARIANT val;
                VariantInit(&val);
                if (SUCCEEDED(e->GetCurrentPropertyValue(UIA_ValueValuePropertyId, &val)) && val.vt == VT_BSTR && val.bstrVal) {
                    std::wstring s = val.bstrVal;
                    if (!AsLink(s).empty()) url = s;
                }
                VariantClear(&val);
                e->Release();
            }
            arr->Release();
        }
        if (c) c->Release();
        root->Release();
    }
    ua->Release();
    return url;
}
// ist das vordere Fenster ein Browser? (Notweg mit Strg+L nur dort, in anderen Programmen hätte die Taste andere Bedeutung)
static bool IsBrowserWindow(HWND fg) {
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    HANDLE h = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (!h) return false;
    wchar_t p[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    bool ok = QueryFullProcessImageNameW(h, 0, p, &n) != 0;
    CloseHandle(h);
    if (!ok) return false;
    std::wstring name = p;
    name = name.substr(name.find_last_of(L'\\') + 1);
    for (auto& ch : name) ch = towlower(ch);
    for (const wchar_t* b : {L"chrome.exe", L"msedge.exe", L"brave.exe", L"opera.exe", L"opera_gx.exe", L"vivaldi.exe", L"firefox.exe", L"arc.exe",
                             L"librewolf.exe", L"waterfox.exe", L"zen.exe", L"chromium.exe", L"iexplore.exe"})
        if (name == b) return true;
    return false;
}
static void SendKeys(std::initializer_list<std::pair<WORD, bool>> keys) {  // (Taste, losgelassen?)
    std::vector<INPUT> in;
    for (auto& k : keys) {
        INPUT i = {};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = k.first;
        if (k.second) i.ki.dwFlags = KEYEVENTF_KEYUP;
        in.push_back(i);
    }
    SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}
// erstes Wort als Link deuten (ggf. https:// ergänzen); leer, wenn es keiner ist
static std::wstring AsLink(std::wstring t) {
    size_t a = t.find_first_not_of(L" \t\r\n\"'<");
    t = a == std::wstring::npos ? L"" : t.substr(a);
    t = t.substr(0, t.find_first_of(L" \t\r\n\"'>"));
    if (t.find(L"://") == std::wstring::npos &&
        (t.rfind(L"www.", 0) == 0 || t.rfind(L"youtube.com", 0) == 0 || t.rfind(L"youtu.be", 0) == 0 ||
         t.rfind(L"music.youtube.com", 0) == 0 || t.rfind(L"m.youtube.com", 0) == 0))
        t = L"https://" + t;
    return (t.rfind(L"http://", 0) == 0 || t.rfind(L"https://", 0) == 0) && t.size() > 12 ? t : L"";
}
static void SetClipText(const std::wstring& s) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    HGLOBAL m = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    if (m) {
        memcpy(GlobalLock(m), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(m);
        if (!SetClipboardData(CF_UNICODETEXT, m)) GlobalFree(m);
    }
    CloseClipboard();
}
// Ende-Taste gehalten: Ende+Pos1/Bild auf/Bild ab steuern die Musik, Ende allein (loslassen) öffnet/schließt
static bool sHoldCombo = false, sHoldPassed = false, sHolding = false;

static const UINT kPollMs = 120;        // Abfrage-Intervall im Ruhezustand
static const DWORD kDwellMs = 150;      // so lange muss die Maus am Rand bleiben
static const DWORD kFreeMs = 30000;     // nach dem Schließen: Daten so lange behalten
static const DWORD kOpenMs[] = {560, 320, 180, 1};   // langsam, normal, schnell, sofort
static const DWORD kCloseMs[] = {380, 220, 130, 1};

// Flächenfarben sind zugleich "Schlüssel": Pixel genau in dieser Farbe werden
// beim Zusammensetzen halbtransparent, alles andere (Text, Symbole) bleibt deckend.
static const COLORREF cBg = RGB(16, 16, 20);
static const COLORREF cCard = RGB(40, 40, 48);
static const COLORREF cCardHot = RGB(50, 50, 60);
static const COLORREF cHover = RGB(68, 68, 80);
static const COLORREF cSearch = RGB(34, 34, 42);
static const COLORREF cText = RGB(245, 245, 248);
static const COLORREF cDim = RGB(175, 175, 186);

static const wchar_t* kOther = L"Sonstiges";
static const int kBarH = 132, kBarArt = 104, kBarBtn = 46;  // Musik-Leiste: Höhe, Cover, Knöpfe

// Farbvorlagen: Anzeigefarbe des Knopfs und dunkle Grundfarbe des Hintergrunds
struct Swatch { COLORREF disp, tint; };
static const Swatch kSwatches[] = {
    {RGB(120, 120, 132), RGB(16, 16, 20)},  // Graphit
    {RGB(60, 120, 240), RGB(10, 20, 46)},   // Blau
    {RGB(150, 90, 230), RGB(28, 14, 46)},   // Lila
    {RGB(60, 180, 110), RGB(8, 32, 20)},    // Grün
    {RGB(225, 70, 80), RGB(42, 10, 14)},    // Rot
    {RGB(240, 140, 50), RGB(46, 24, 6)},    // Orange
    {RGB(230, 90, 170), RGB(44, 10, 32)},   // Pink
    {RGB(40, 190, 200), RGB(6, 34, 38)},    // Türkis
};
static const int kSwatchCount = 8;
static const COLORREF cAccent = RGB(96, 165, 255);

// ---------------------------------------------------------------- Daten

struct App {
    std::wstring name, id, target, low, autoFolder, folder;
    std::wstring file;  // Verknüpfung vom Desktop: wird direkt gestartet
    HBITMAP icon = nullptr;       // große Ausführung (genau in Anzeigegröße geladen)
    HBITMAP iconSmall = nullptr;  // kleine Ausführung für Listen
    int launches = 0;
    bool hidden = false;
};
struct AppFolder {
    std::wstring name;
    std::vector<int> items;
    int shown = 0;  // davon direkt in der Ordnerkarte sichtbar
};

enum class View { Folders, Folder, Search, Settings, Music, MusicList, SongEdit, MusicSettings, AllApps, Calendar, Storage, Chat, NowPlaying, Timer };
enum class HT {
    None, Card, Slot, Mini, Cell, Back, Menu, Gear,
    // Einstellungen
    SetCard, SetFolderHead, SetFolderUp, SetFolderDown, SetFolderMenu,
    SetAppRow, SetAppUp, SetAppDown, SetAppMenu, SetDrop, SetHiddenHead, SetHiddenRow, SetUnhide,
    SetSection, SetRowLabel, SetSlider, SetToggle, SetSwatch, SetButton,
    Tab, Pane, PaneTitle, NewFolder, SearchR, AllAppRow, AllAppAdd, SetFolderDelete, SetFolderGame, GameRow, GameHead, Logo, Wifi, Battery, BarVol, AppSizeRow, SortBtn, TitleBtn, KillBtn, NavBar, Nav, LinkChip, LinkAdd, RecentCard, RecentItem, CalNav, CalHead, CalDay, CalEvent, CalAdd, CalInfo, ToolBtn, FolderBtn, PlugCard, PlugBtn, DriveRow, DriveOpen, StorageApps, StorageRefresh, PlayMode, ChatNew, NpCover, NpLyrics, BarCover, LibTab, FavBtn, UpdateBtn, SetSpeed, MusicBar, Fav, FavSlot, Bluetooth, MediaPrev, MediaPlay, MediaNext, MediaPhone,
    // Musik
    MusicJob, MusicEmpty, MCard, MTile, MMore, MPlayAll, MRow, PlHead, PlUp, PlDown, PlDelete, PlDrop, PlSongRow,
    AllSongRow, AllSongAdd, AllSongEdit, MusicOpen, MProgress, EditCover, EditBtn, EditInfo, CoverPick, EditLabel, EditField,
    MAlbum, VolIcon, MusicManage, MSize, MSort, GrabKeyBtn, MReload, TimerWheel, TimerShow, TimerBtn, CtlKeyBtn
};
struct Hit {
    RECT rc;
    HT type;
    int idx;       // App-Index (Slot/Cell/App-Zeilen) oder Ordner-Index (Card/Mini/Ordnerzeilen)
    int f = -1;    // Einstellungen: Ordner-Index in allFolders
    int pos = -1;  // Einstellungen: Position im Ordner
    int n = 0;     // Einstellungen: Anzahl Einträge (für ▲/▼ deaktivieren)
    int pane = 0;  // 0 = normaler Inhalt, 1 = linke Ordnerliste, 2 = rechte App-Liste
};

struct Rule {
    const wchar_t* folder;
    std::vector<const wchar_t*> keys;
};

static const std::vector<Rule>& Rules() {
    static const std::vector<Rule> r = {
        {L"Spiele", {L"steam", L"steamapps", L"epic games", L"epicgames", L"com.epicgames.launcher", L"gog galaxy", L"gog.com",
                     L"riot", L"riot games", L"valorant", L"league of legends", L"battle.net", L"blizzard", L"ubisoft", L"ubisoft connect",
                     L"uplay", L"ea", L"ea app", L"electronic arts", L"rockstar games", L"minecraft", L"roblox", L"xbox",
                     L"gamingapp", L"fortnite", L"rocket league", L"uplay", L"battlenet", L"origin2", L"game", L"games", L"spiel", L"spiele", L"solitaire", L"playnite", L"heroic",
                     L"curseforge", L"overwolf", L"geforce now", L"itch", L"amazon games", L"genshin", L"hoyoplay", L"wargaming"}},
        {L"Soziales", {L"discord", L"whatsapp", L"telegram", L"signal", L"teams", L"zoom", L"skype", L"slack", L"messenger",
                       L"instagram", L"tiktok", L"facebook", L"snapchat", L"teamspeak", L"threema", L"element", L"guilded"}},
        {L"Internet", {L"chrome", L"google chrome", L"firefox", L"msedge", L"microsoft edge", L"edge", L"opera", L"opera gx",
                       L"brave", L"vivaldi", L"tor browser", L"browser", L"filezilla", L"qbittorrent", L"utorrent", L"arc",
                       L"librewolf", L"waterfox", L"zen browser"}},
        {L"Musik & Video", {L"spotify", L"vlc", L"media player", L"netflix", L"prime video", L"disney", L"youtube", L"itunes",
                            L"apple music", L"apple tv", L"deezer", L"amazon music", L"kodi", L"plex", L"mpc-hc", L"potplayer",
                            L"music", L"musik", L"video", L"videos", L"filme", L"movies", L"twitch", L"foobar2000", L"winamp",
                            L"audible", L"tidal", L"crunchyroll", L"soundcloud"}},
        {L"Kreativ", {L"photoshop", L"illustrator", L"lightroom", L"premiere", L"premiere pro", L"after effects", L"gimp", L"krita",
                      L"blender", L"paint", L"paint 3d", L"paint.net", L"inkscape", L"davinci", L"resolve", L"audacity", L"obs",
                      L"obs studio", L"canva", L"figma", L"affinity", L"capcut", L"clipchamp", L"kdenlive", L"shotcut",
                      L"fl studio", L"ableton", L"cinema 4d", L"maya", L"3ds max", L"sketchup", L"fotos", L"photos", L"kamera",
                      L"camera", L"aseprite", L"clip studio", L"handbrake", L"camtasia", L"medibang", L"adobe", L"vegas",
                      L"filmora", L"wallpaper engine"}},
        {L"Entwicklung", {L"visual studio", L"visual studio code", L"vscode", L"python", L"git", L"github", L"github desktop",
                          L"node.js", L"android studio", L"intellij", L"pycharm", L"webstorm", L"rider", L"clion", L"jetbrains",
                          L"unity", L"unity hub", L"unreal", L"unreal engine", L"godot", L"postman", L"docker", L"sql", L"wsl",
                          L"ubuntu", L"eclipse", L"arduino", L"notepad++", L"sublime text", L"cursor", L"windsurf", L"claude"}},
        {L"Office & Produktiv", {L"word", L"excel", L"powerpoint", L"outlook", L"onenote", L"access", L"publisher", L"onedrive",
                                 L"notepad", L"editor", L"acrobat", L"pdf", L"libreoffice", L"openoffice", L"notion", L"obsidian",
                                 L"evernote", L"thunderbird", L"mail", L"kalender", L"calendar", L"rechner", L"calculator",
                                 L"todo", L"to do", L"sticky notes", L"kurznotizen", L"office", L"wordpad", L"google drive",
                                 L"dropbox", L"keepass", L"bitwarden", L"1password", L"uhr", L"clock", L"wecker", L"karten",
                                 L"maps", L"wetter", L"weather", L"news", L"nachrichten", L"copilot", L"chatgpt"}},
        {L"System & Tools", {L"einstellungen", L"settings", L"systemsteuerung", L"control panel", L"task-manager", L"task manager",
                             L"explorer", L"datei-explorer", L"file explorer", L"terminal", L"powershell", L"eingabeaufforderung",
                             L"command prompt", L"cmd", L"windows-tools", L"windows tools", L"regedit", L"registrierungs-editor",
                             L"defender", L"sicherheit", L"security", L"nvidia", L"geforce", L"amd", L"radeon", L"intel", L"realtek",
                             L"razer", L"logitech", L"logi", L"corsair", L"icue", L"steelseries", L"msi", L"asus", L"armoury",
                             L"lenovo", L"hp", L"dell", L"acer", L"7-zip", L"winrar", L"ccleaner", L"cpu-z", L"hwmonitor",
                             L"afterburner", L"revo", L"everything", L"powertoys", L"snipping tool", L"ausschneiden", L"remote",
                             L"remotedesktop", L"remotedesktopverbindung", L"virtualbox", L"vmware", L"backup", L"treiber",
                             L"driver", L"update", L"recovery", L"wiederherstellung", L"datenträger", L"disk", L"bildschirmtastatur",
                             L"on-screen keyboard", L"schnellhilfe", L"quick assist", L"sprachrekorder", L"tipps", L"tips",
                             L"hilfe", L"get help", L"feedback", L"windows", L"microsoft store", L"store", L"gerätemanager",
                             L"device manager", L"anydesk", L"teamviewer", L"vpn", L"nordvpn", L"protonvpn", L"malwarebytes"}},
    };
    return r;
}

static const std::vector<std::wstring>& DefaultFolders() {
    static const std::vector<std::wstring> f = {L"Spiele", L"Internet", L"Soziales", L"Musik & Video", L"Kreativ",
                                                L"Office & Produktiv", L"Entwicklung", L"System & Tools"};
    return f;
}

static const wchar_t* kExclude[] = {L"uninstall", L"uninstaller", L"deinstallieren", L"deinstallation", L"entfernen",
                                    L"readme", L"read me", L"liesmich", L"release notes", L"changelog", L"license",
                                    L"lizenz", L"documentation", L"dokumentation", L"manual", L"handbuch", L"website",
                                    L"homepage"};

// ---------------------------------------------------------------- Zustand

struct State {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    HFONT fTitle = nullptr, fText = nullptr, fHead = nullptr, fSmall = nullptr, fIcon = nullptr, fGlyph = nullptr, fGlyphL = nullptr, fBarTitle = nullptr, fLyric = nullptr, fNpTitle = nullptr;
    std::wstring glyphFont;

    // Ziehen & Ablegen in den Einstellungen
    bool dragCand = false, dragging = false;
    int favs[4] = {-1, -1, -1, -1};  // die vier Apps oben auf der Startseite (App-Index oder -1)
    int dragFromFav = -1;            // gezogen aus Favoriten-Feld Nr.
    int dragApp = -1, dragFromF = -1, dropF = -1, dropPos = -1, dropLineY = -1;
    POINT downPt = {}, mousePt = {};
    UINT dpi = 0;

    // Fenster / Animation
    bool open = false, closing = false, armed = true, animating = false, closeAfterAnim = false;
    int curX = 0, xShown = 0, xHidden = 0, y = 0, w = 0, h = 0;
    int monLeft = 0, monW = 0;  // Bildschirm, auf dem die Leiste offen ist
    BYTE fade = 255;
    DWORD edgeSince = 0;
    int watchTick = 0;
    bool menuOpen = false, tracking = false, caretOn = true, redrawPending = false;
    std::wstring query;

    // Daten
    std::vector<App> apps;
    std::vector<AppFolder> folders;     // nur Ordner mit Apps (Hauptansicht)
    std::vector<AppFolder> allFolders;  // alle Ordner, auch leere (Einstellungen)
    std::vector<int> hiddenApps;
    ULONGLONG lastLoad = 0;             // wann die App-Liste zuletzt geladen wurde
    bool loaded = false, loading = false;

    // Anzeige
    View view = View::Folders;
    int curFolder = -1, scroll = 0, contentH = 0;
    std::vector<int> results;
    std::vector<Hit> hits;
    int hot = -1, pressed = -1;
    int namingFor = -1;  // App, die in einen neuen Ordner soll

    // Zeichenpuffer
    HDC memDC = nullptr;
    HBITMAP memBmp = nullptr, memOld = nullptr;
    BYTE* bits = nullptr;
    int memW = 0, memH = 0;

    // Kurzinfo (App-Name beim Darüberfahren)
    bool tipShown = false;

    // Symbole werden nach dem Zusammensetzen mit echtem Alphakanal gezeichnet
    struct IconDraw { int idx, x, y, sz; RECT clip; float fade; HBITMAP bmp; };
    float drawFade = 1.f;  // Deckkraft für alles, was gerade aufgezeichnet wird
    std::vector<IconDraw> iconQueue;

    // Flächen und Texte werden erst aufgezeichnet und dann kantengeglättet zusammengesetzt
    enum ShapeKind { SH_CARD, SH_CARDHOT, SH_HOVER, SH_SEARCH, SH_SOLID };
    struct Shape { RECT r; float rad; int kind; COLORREF color; RECT clip; float fade; };
    struct TextRun { RECT r; COLORREF color; float fade; };
    std::vector<Shape> shapes;
    std::vector<TextRun> texts;
    std::vector<uint32_t> basePx;  // Grundebene (Glas/Farbe), vormultipliziert und zwischengespeichert
    std::wstring baseKey;
    int bdVersion = 0;  // zählt bei jedem neuen Glas-Hintergrund hoch
    HDC maskDC = nullptr;          // Texte weiß auf schwarz (ClearType-Abdeckung pro Farbkanal)
    HBITMAP maskBmp = nullptr, maskOld = nullptr;
    BYTE* maskBits = nullptr;

    // Aussehen
    std::wstring ini;
    bool onlyDesktop = false, blur = true;
    // Funktionen
    int musicSrc = 0;              // 0 = HoverBar-Player, 1 = Spotify
    bool duckOn = true, musicKeys = true, grabKey = true;
    bool musicOn = true;           // Musik-Player überhaupt anzeigen (aus = keine Musikleiste, keine Musik-Tasten)
    bool claudeOn = false;         // Erweiterung "Claude-Chat" hinzugefügt (Erweiterungen)
    bool syncOn = false;           // Erweiterung "Musik-Sync" eingerichtet
    std::wstring syncDir, syncNote;  // Cloud-Ordner der Songs; Hinweis auf der Karte
    std::wstring syncCloud, syncPkg, syncParent;  // gewählter Dienst, sein Android-Paketname, Elternordner (für den QR-Code fürs Handy)
    bool syncShowQr = false;       // nach dem Verschieben den QR-Code zeigen
    // Spielmodus-Ordner: Statistiken je App (nur solange die Ansicht offen ist)
    // Kopfzeile: WLAN/Netz, Akku, Logo
    int net = 0, netBars = 0;      // 0 kein Netz, 1 WLAN, 2 LAN, 3 Mobilfunk; Balken 0..4
    bool netInternet = false;
    std::wstring netName;
    int batt = -1;                 // Akku in %, -1 = kein Akku
    bool battCharging = false;
    ULONGLONG statusAt = 0;
    int headerRight = 0;           // linker Rand der Symbole oben rechts
    int barTextRight = 0;          // Musikleiste: Text endet hier (davor Lautstärke und Knöpfe)
    double volExp = 0, volTarget = 0;  // Lautstärkeregler ausgeklappt (0..1)
    ULONGLONG volLeaveAt = 0;          // einklappen ab diesem Zeitpunkt
    HBITMAP logoBmp = nullptr;
    int logoPx = 0;
    std::map<int, GameStats> gstats;
    // Liste "Alle Apps": App-Index und Größe in Bytes (-1 = unbekannt)
    std::vector<std::pair<int, long long>> appSizes;
    bool sortBySize = false;
    bool onlyRunning = false;      // Filter "Laufend"
    // Favoriten-Links (unter der Startseite) und "Zuletzt genutzt" (erst nach starkem Hochscrollen sichtbar)
    struct FavLink { std::wstring url, name, host; };
    std::vector<FavLink> links;
    struct RecentEntry { bool link; int app; std::wstring url, name, host; };
    std::vector<RecentEntry> recent;
    bool showRecent = false;
    // Kalender: angezeigter Monat, gewählter Tag (jjjjmmtt) und eigene Termine (termine.txt)
    struct CalEntry { int date; std::wstring time, title; };
    std::vector<CalEntry> events;
    bool eventsLoaded = false;
    int calY = 0, calM = 0, calSel = 0;
    std::wstring storageStamp;  // Speicher-Seite: wann zuletzt aktualisiert
    bool shuffle = false;       // Musik: zufällige Reihenfolge
    RECT chatRect = {};         // Claude-Chat: Bereich in Fensterkoordinaten
    int navAcc = 0;             // Mausrad auf der Leiste: angesammelte Drehung
    // "Läuft gerade": Songtext des aktuellen Songs
    View npReturn = View::Music;          // wohin "Zurück" führt
    int npFolder = -1;
    bool lyrAnim = false;
    // Beim Schließen gemerkte Ansicht: öffnet man die Leiste binnen 10 s wieder, ist alles noch so da
    struct Resume {
        ULONGLONG at = 0;
        View view = View::Folders;
        int folder = -1, scroll = 0, scrollR = 0, libTab = 0, setTab = 0, curPlaylist = -1, npFolder = -1;
        View npReturn = View::Music;
        std::wstring query;
    } resume;
    // Bibliothek: 0 Alle Songs, 1 Favoriten, 2 Playlists, 3 Zuletzt gespielt
    int libTab = 0;
    std::vector<std::wstring> favIds, recentIds;  // Song-IDs (neueste zuerst)
    std::vector<int> mFav, mRecent;               // als Indizes in Music_Songs()
    std::wstring lastRecentId;
    double npDrop = 0;           // Einklappen nach unten: Versatz in Pixeln
    float npAlpha = 1.f;         //   und Deckkraft dabei
    bool npDropping = false;
    bool lyrManual = false;      // Songtext wird gerade von Hand durchgescrollt
    double lyrUser = 0;          //   Ziel-Scrollstand dabei
    int lyrTotal = 0, lyrViewH = 0;
    std::wstring lyrId;                   // Song, zu dem lyr gehört
    std::vector<LyricLine> lyr;
    bool lyrSynced = false;
    int lyrState = 0;                     // 0 nichts, 1 lädt, 2 fertig
    std::vector<int> lyrH;                // Höhe jeder Zeile (umbrochen)
    int lyrW = 0;                         // Breite, für die lyrH gilt
    double lyrScroll = 0, lyrTarget = 0;  // weiches Mitscrollen
    std::vector<std::pair<RECT, double>> lyrRects;  // sichtbare Zeilen (zum Anklicken -> spulen)
    int repeat = 0;             // Musik: 0 aus, 1 Song wiederholen, 2 Playlist wiederholen
    int pullAcc = 0;
    ULONGLONG pullAt = 0, navWheelAt = 0;
    long long appMax = 1, appTotal = 0;
    std::map<int, AppMatcher> appMatch;                              // App -> woran ihre Prozesse erkannt werden
    std::map<int, std::pair<std::vector<DWORD>, SIZE_T>> running;    // laufende Apps: Prozesse und Arbeitsspeicher
    long long gMaxSize = 1;
    int gMaxMin = 1;
    int bgAlpha = 135;             // Deckkraft des Hintergrunds (40–255)
    COLORREF tint = RGB(16, 16, 20);  // Grundfarbe des Hintergrunds
    std::wstring imgPath;          // Hintergrundbild
    int imgStrength = 60;          // wie stark das Bild durchscheint (0–100)
    std::vector<uint32_t> img;     // auf Fenstergröße skaliertes Bild (nur solange offen)
    int imgW = 0, imgH = 0;
    std::wstring imgLoaded;
    int sliderDrag = -1;           // gerade gezogener Schieberegler
    // Bluetooth (nur beim Öffnen abgefragt, kostet im Ruhezustand nichts)
    struct BtDevice { std::wstring name; bool connected; };
    std::vector<BtDevice> btDevices;
    bool btRadio = false;
    // Musik-Bibliothek (Anzeige)
    std::vector<int> mAll;                       // alle Songs, alphabetisch (Indizes in Music_Songs())
    std::vector<std::vector<int>> plItems;       // Songs je Playlist
    std::vector<DownloadJob> jobsView;
    int curPlaylist = -1;                        // geöffnete Liste (-1 = alle Songs)
    std::wstring editId, editTitle, editArtist;  // Song bearbeiten
    int editField = 0;                           // 1 = Titel, 2 = Künstler
    double trimLen = 0, trimA = 0, trimB = 0;    // Schneiden: Länge, Anfang, Ende (Sekunden)
    int trimHandle = 0;                          // 1 = Anfang, 2 = Ende wird gezogen
    double trimPrevFrom = 0;                     // Start des gerade probegehörten Ausschnitts
    int trimSel = 0;                             // zuletzt gezogene Linie (Pfeiltasten verschieben sie fein)
    std::vector<float> wave;                     // Wellenform des Songs (0..1), leer solange sie lädt
    View editReturn = View::Music;
    int editReturnTab = 0;
    std::vector<std::wstring> coverResults;      // gefundene Cover (lokale Vorschaudateien)
    bool coverSearching = false;
    bool coverNone = false;                      // Suche war fertig, aber ohne Treffer
    std::wstring pendingUrl;                     // Link wartet auf den yt-dlp-Download
    std::map<std::wstring, HBITMAP> coverCache;
    PlayerState pst;                             // eigener Player
    bool dragMusic = false;
    int dragFromPos = -1;
    int pausedMinutes = 0;
    int volume = 80, volumeBeforeMute = 80;      // Lautstärke 0–100
    int lastSlider = -1;
    int musicSize = 1;                           // 0 = klein, 1 = mittel, 2 = groß
    // Textfelder: Schreibmarke, Markierungsanfang, Verschiebung bei langem Text
    int caret = 0, anchor = 0, fieldOff = 0;
    std::wstring* caretOwner = nullptr;          // zu welchem Text Schreibmarke/Markierung gehören
    RECT activeField = {};                       // Rechteck des aktiven Eingabefelds (zum Klicken)
    bool textDrag = false;
    // Taste zum Öffnen (global), Standard: Ende
    UINT hotVk = VK_END, hotMods = 0;
    UINT grabVk = VK_END, grabMods = MOD_ALT;  // Taste für "Link laden"
    bool grabCapture = false;
    bool tileCovers = true, songLines = true;   // Grafik: Cover in den Musik-Kacheln, Trennlinien
    int reload = 0, reloadTick = 0, reloadStable = 0;  // Neueinlesen: 0 nichts, 1 lädt, 2 fertig (Balken voll)
    unsigned long long reloadSnap = 0;
    bool appsOn = true;                          // App-Launcher; aus = nur Musik-Player
    bool timerOn = false, timerRinging = false;  // Erweiterung "Timer"
    double wheel[3] = {0, 5, 0};                 // Ziffernräder: h, min, s
    int wheelDrag = -1, wheelY = 0;
    bool wheelMoved = false;
    double wheelStart = 0;
    ULONGLONG timerEnd = 0, ringSince = 0;       // timerEnd 0 = läuft nicht
    long long timerLeft = 0;                     // pausiert: Restzeit in ms
    std::wstring ringFile;                       // leer = Standardton
    // Song-Steuerung (zusammen mit der gehaltenen Öffnen-Taste): Play/Pause, weiter, zurück, lauter, leiser
    UINT ctlKey[5] = {VK_HOME, VK_NEXT, VK_PRIOR, VK_OEM_PLUS, VK_OEM_MINUS};
    int ctlCapture = -1;                         // wartet auf die neue Taste für diese Funktion
    bool calendarOn = true;                      // Kalender-Seite
    bool kbdNav = false;                        // zuletzt mit Pfeiltasten navigiert (Enter aktiviert dann die Markierung)
    bool sortPlays = false;                    // "Alle Songs" nach Wiedergaben statt Titel
    bool hotCapture = false;                     // wartet in den Einstellungen auf eine neue Taste
    // Musik-Player
    MediaInfo media;               // aktuelle Wiedergabe (ohne Bild)
    HBITMAP mediaArt = nullptr;    // Albumbild in Anzeigegröße
    bool phoneConnected = false;
    std::wstring phoneName, mediaNote;
    ULONGLONG noteUntil = 0;
    int hintY = 0;
    int blurAmount = 55;           // Glas: Stärke der Unschärfe (0–100)
    std::wstring fontName = L"Segoe UI";
    bool fontBold = false;
    int animSpeed = 1;             // 0 = langsam, 1 = normal, 2 = schnell, 3 = sofort

    // Einstellungen: Reiter und zwei Bereiche
    int setTab = 0;                // 0 = Design, 1 = Ordner
    bool creatingFolder = false;   // Eingabefeld "Neuer Ordner" aktiv
    int scrollR = 0, contentHR = 0;
    // weiches Scrollen: Ziel und aktuelle Position (Kommazahl), Zeitmessung
    int scrollTo = 0, scrollRTo = 0;
    double scrollF = 0, scrollRF = 0;
    double scrollV = 0, scrollRV = 0;  // Geschwindigkeit (px/s): bleibt zwischen Mausrad-Rasten erhalten
    bool scrollAnim = false;
    LARGE_INTEGER scrollT = {};
    double renderMsSum = 0;
    double stageMs[5] = {};
    int renderFrames = 0;
    int lTop = 0, lBot = 0, rTop = 0, rBot = 0, paneMid = 0;
    std::vector<int> allList;      // rechte Liste (gefiltert)

    // Breite / Position
    int wNormal = 0, wWide = 0, workRight = 0, margin = 0;

    // Glas: Hintergrund hinter der Leiste, beim Öffnen aufgenommen (klein und weichgezeichnet)
    std::vector<uint32_t> bdRaw, bdBlur;
    int bdW = 0, bdH = 0, capX = 0, capY = 0;
    static const int bdFactor = 4;
} g;

static std::atomic<int> g_gen{0};

static int S(int v) { return MulDiv(v, g.dpi ? g.dpi : 96, 96); }

// ---------------------------------------------------------------- Hilfsfunktionen

static std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

static bool HasWord(const std::wstring& hay, const wchar_t* key) {
    size_t klen = wcslen(key);
    size_t pos = 0;
    while ((pos = hay.find(key, pos)) != std::wstring::npos) {
        bool startOk = pos == 0 || !IsCharAlphaNumericW(hay[pos - 1]);
        bool endOk = pos + klen >= hay.size() || !IsCharAlphaNumericW(hay[pos + klen]);
        if (startOk && endOk) return true;
        pos += 1;
    }
    return false;
}

static std::wstring IniGet(const wchar_t* sec, const std::wstring& key, const wchar_t* def = L"") {
    wchar_t buf[2048];
    GetPrivateProfileStringW(sec, key.c_str(), def, buf, 2048, g.ini.c_str());
    return buf;
}
static void IniSet(const wchar_t* sec, const std::wstring& key, const wchar_t* val) {
    WritePrivateProfileStringW(sec, key.c_str(), val, g.ini.c_str());
}

// "Link laden"-Taste belegen (id 5); true, wenn nichts zu belegen war oder es geklappt hat
static bool RegisterGrabKey() {
    return !g.grabKey || !g.musicOn || RegisterHotKey(g.hwnd, 5, g.grabMods | MOD_NOREPEAT, g.grabVk);
}

static void InitIni() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    dir += L"\\HoverBar";
    CreateDirectoryW(dir.c_str(), nullptr);
    g.ini = dir + L"\\HoverBar.ini";
    if (GetFileAttributesW(g.ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // UTF-16-BOM, damit Umlaute in der Datei korrekt gespeichert werden
        HANDLE f = CreateFileW(g.ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            const WORD bom = 0xFEFF;
            DWORD wr;
            WriteFile(f, &bom, 2, &wr, nullptr);
            CloseHandle(f);
        }
    }
    g.onlyDesktop = GetPrivateProfileIntW(L"Einstellungen", L"NurDesktop", 0, g.ini.c_str()) != 0;
    g.blur = GetPrivateProfileIntW(L"Einstellungen", L"Unschaerfe", 1, g.ini.c_str()) != 0;
    g.bgAlpha = std::max(40, std::min(255, (int)GetPrivateProfileIntW(L"Einstellungen", L"Deckkraft", 135, g.ini.c_str())));
    std::wstring col = IniGet(L"Einstellungen", L"Farbe");
    if (col.size() == 6) {
        unsigned v = wcstoul(col.c_str(), nullptr, 16);
        g.tint = RGB((v >> 16) & 255, (v >> 8) & 255, v & 255);
    }
    g.imgPath = IniGet(L"Einstellungen", L"Bild");
    g.imgStrength = std::max(0, std::min(100, (int)GetPrivateProfileIntW(L"Einstellungen", L"Bildstaerke", 60, g.ini.c_str())));
    g.blurAmount = std::max(0, std::min(100, (int)GetPrivateProfileIntW(L"Einstellungen", L"Unschaerfegrad", 55, g.ini.c_str())));
    std::wstring fn = IniGet(L"Einstellungen", L"Schrift");
    if (!fn.empty()) g.fontName = fn;
    g.fontBold = GetPrivateProfileIntW(L"Einstellungen", L"Fett", 0, g.ini.c_str()) != 0;
    g.animSpeed = std::max(0, std::min(3, (int)GetPrivateProfileIntW(L"Einstellungen", L"Tempo", 1, g.ini.c_str())));
    g.volume = std::max(0, std::min(100, (int)GetPrivateProfileIntW(L"Einstellungen", L"Lautstaerke", 80, g.ini.c_str())));
    g.musicSize = std::max(0, std::min(2, (int)GetPrivateProfileIntW(L"Einstellungen", L"MusikGroesse", 1, g.ini.c_str())));
    g.hotVk = GetPrivateProfileIntW(L"Einstellungen", L"Taste", VK_END, g.ini.c_str());
    g.grabVk = GetPrivateProfileIntW(L"Einstellungen", L"LinkTasteVk", VK_END, g.ini.c_str());
    g.grabMods = GetPrivateProfileIntW(L"Einstellungen", L"LinkTasteMods", MOD_ALT, g.ini.c_str());
    g.sortPlays = GetPrivateProfileIntW(L"Musik", L"NachWiedergaben", 0, g.ini.c_str()) != 0;
    g.tileCovers = GetPrivateProfileIntW(L"Einstellungen", L"KachelCover", 1, g.ini.c_str()) != 0;
    g.songLines = GetPrivateProfileIntW(L"Einstellungen", L"Trennlinien", 1, g.ini.c_str()) != 0;
    g.hotMods = GetPrivateProfileIntW(L"Einstellungen", L"TasteMods", 0, g.ini.c_str());
    g.musicSrc = GetPrivateProfileIntW(L"Funktionen", L"MusikQuelle", 0, g.ini.c_str()) == 1 ? 1 : 0;
    g.musicOn = GetPrivateProfileIntW(L"Funktionen", L"MusikPlayer", 1, g.ini.c_str()) != 0;
    g.claudeOn = GetPrivateProfileIntW(L"Erweiterungen", L"Claude", 0, g.ini.c_str()) != 0;
    g.timerOn = GetPrivateProfileIntW(L"Erweiterungen", L"Timer", 0, g.ini.c_str()) != 0;
    g.appsOn = GetPrivateProfileIntW(L"Funktionen", L"AppLauncher", 1, g.ini.c_str()) != 0;
    g.calendarOn = GetPrivateProfileIntW(L"Funktionen", L"Kalender", 1, g.ini.c_str()) != 0;
    g.wheel[0] = GetPrivateProfileIntW(L"Timer", L"Stunden", 0, g.ini.c_str()) % 24;
    g.wheel[1] = GetPrivateProfileIntW(L"Timer", L"Minuten", 5, g.ini.c_str()) % 60;
    g.wheel[2] = GetPrivateProfileIntW(L"Timer", L"Sekunden", 0, g.ini.c_str()) % 60;
    {
        wchar_t ring[MAX_PATH] = {};
        GetPrivateProfileStringW(L"Timer", L"Klingelton", L"", ring, MAX_PATH, g.ini.c_str());
        g.ringFile = ring;
    }
    g.syncOn = GetPrivateProfileIntW(L"Erweiterungen", L"MusikSync", 0, g.ini.c_str()) != 0;
    g.syncDir = IniGet(L"Erweiterungen", L"MusikSyncOrdner");
    g.syncCloud = IniGet(L"Erweiterungen", L"MusikSyncCloud");
    g.syncPkg = IniGet(L"Erweiterungen", L"MusikSyncPaket");
    g.syncParent = IniGet(L"Erweiterungen", L"MusikSyncElternordner");
    Music_SetSyncExport(g.syncOn);
    Music_SetFolder(IniGet(L"Funktionen", L"SongOrdner"));  // leer = Musik\HoverBar
    g.shuffle = GetPrivateProfileIntW(L"Einstellungen", L"Zufall", 0, g.ini.c_str()) != 0;
    g.repeat = std::max(0, std::min(2, (int)GetPrivateProfileIntW(L"Einstellungen", L"Wiederholen", 0, g.ini.c_str())));
    Player_SetModes(g.shuffle, g.repeat);
    g.duckOn = GetPrivateProfileIntW(L"Funktionen", L"LeiserBeiVideos", 1, g.ini.c_str()) != 0;
    g.musicKeys = GetPrivateProfileIntW(L"Funktionen", L"MusikTasten", 1, g.ini.c_str()) != 0;
    g.grabKey = GetPrivateProfileIntW(L"Funktionen", L"LinkTaste", 1, g.ini.c_str()) != 0;
    for (int i = 0; i < 5; i++)
        g.ctlKey[i] = GetPrivateProfileIntW(L"Einstellungen", (L"Steuerung" + std::to_wstring(i)).c_str(), g.ctlKey[i], g.ini.c_str());
}

static std::vector<std::wstring> CustomFolders() {
    std::vector<std::wstring> out;
    std::wstring s = IniGet(L"Ordner", L"Liste");
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(L'|', start);
        if (end == std::wstring::npos) end = s.size();
        std::wstring part = s.substr(start, end - start);
        if (!part.empty()) out.push_back(part);
        start = end + 1;
    }
    return out;
}
static void SaveCustomFolders(const std::vector<std::wstring>& f) {
    std::wstring s;
    for (size_t i = 0; i < f.size(); i++) s += (i ? L"|" : L"") + f[i];
    IniSet(L"Ordner", L"Liste", s.c_str());
}
static std::wstring IniGetLong(const wchar_t* sec, const std::wstring& key) {
    std::vector<wchar_t> buf(65536);
    GetPrivateProfileStringW(sec, key.c_str(), L"", buf.data(), (DWORD)buf.size(), g.ini.c_str());
    return buf.data();
}

static std::vector<std::wstring> SplitList(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find(L'|', start);
        if (end == std::wstring::npos) end = s.size();
        if (end > start) out.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

static std::wstring JoinList(const std::vector<std::wstring>& v) {
    std::wstring s;
    for (size_t i = 0; i < v.size(); i++) s += (i ? L"|" : L"") + v[i];
    return s;
}

// Alle Ordnernamen in der vom Nutzer gewählten Reihenfolge
static std::vector<std::wstring> RemovedFolders() { return SplitList(IniGetLong(L"Ordner", L"Entfernt")); }

static std::vector<std::wstring> AllFolderNames() {
    std::vector<std::wstring> base = DefaultFolders();
    for (auto& c : CustomFolders())
        if (std::find(base.begin(), base.end(), c) == base.end()) base.push_back(c);
    base.push_back(kOther);
    auto removed = RemovedFolders();
    base.erase(std::remove_if(base.begin(), base.end(),
                              [&](const std::wstring& n) { return std::find(removed.begin(), removed.end(), n) != removed.end(); }),
               base.end());
    std::vector<std::wstring> all;
    for (auto& n : SplitList(IniGetLong(L"Ordner", L"Reihenfolge")))
        if (std::find(base.begin(), base.end(), n) != base.end() && std::find(all.begin(), all.end(), n) == all.end())
            all.push_back(n);
    for (auto& n : base)
        if (std::find(all.begin(), all.end(), n) == all.end()) all.push_back(n);
    return all;
}

static std::wstring Categorize(const App& a) {
    std::wstring hay = a.low + L" | " + Lower(a.id) + L" | " + Lower(a.target);
    for (auto& r : Rules())
        for (auto k : r.keys)
            if (HasWord(hay, k)) return r.folder;
    return kOther;
}

static void EfficiencyMode(bool on) {
    PROCESS_POWER_THROTTLING_STATE s = {};
    s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    s.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    s.StateMask = on ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s));
    SetPriorityClass(GetCurrentProcess(), on ? IDLE_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
}

static void TrimMemory() {
    HeapCompact(GetProcessHeap(), 0);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

static bool AutostartEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunName, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}
static void SetAutostart(bool on) {
    if (on) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring v = L"\"" + std::wstring(path) + L"\"";
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunName, REG_SZ, v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunName);
    }
}

// ---------------------------------------------------------------- Apps laden (Hintergrund-Thread)

struct LoadParams { int gen; int iconSize; int smallSize; HWND hwnd; };
struct IconMsg { int gen; int idx; HBITMAP bmp, smallBmp; };
struct ListMsg { int gen; std::vector<App>* apps; };

static const PROPERTYKEY kTargetPath = {{0xB9B4B3FC, 0x2B51, 0x4A42, {0xB5, 0xD8, 0x32, 0x41, 0x46, 0xAF, 0xCF, 0x25}}, 2};

// Manche Symbole kommen ohne vormultiplizierten Alphakanal -> farbige Ränder bei AlphaBlend.
static void PremultiplyIfNeeded(HBITMAP bmp) {
    DIBSECTION ds = {};
    if (GetObjectW(bmp, sizeof(ds), &ds) != sizeof(ds) || ds.dsBm.bmBitsPixel != 32 || !ds.dsBm.bmBits) return;
    BYTE* px = (BYTE*)ds.dsBm.bmBits;
    size_t n = (size_t)ds.dsBm.bmWidth * std::abs(ds.dsBm.bmHeight);
    bool needs = false, anyAlpha = false;
    for (size_t i = 0; i < n; i++) {
        BYTE a = px[i * 4 + 3];
        if (a) anyAlpha = true;
        if (px[i * 4] > a || px[i * 4 + 1] > a || px[i * 4 + 2] > a) { needs = true; break; }
    }
    if (!anyAlpha) {  // gar kein Alpha: komplett deckend machen
        for (size_t i = 0; i < n; i++) px[i * 4 + 3] = 255;
        return;
    }
    if (!needs) return;
    for (size_t i = 0; i < n; i++) {
        BYTE* p = px + i * 4;
        p[0] = (BYTE)(p[0] * p[3] / 255);
        p[1] = (BYTE)(p[1] * p[3] / 255);
        p[2] = (BYTE)(p[2] * p[3] / 255);
    }
}

static DWORD WINAPI LoadThread(LPVOID param) {
    LoadParams p = *(LoadParams*)param;
    delete (LoadParams*)param;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    struct Tmp { App a; IShellItem* item; };
    std::vector<Tmp> list;

    IShellItem* appsFolder = nullptr;
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&appsFolder)))) {
        IEnumShellItems* en = nullptr;
        if (SUCCEEDED(appsFolder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en)))) {
            IShellItem* it = nullptr;
            while (en->Next(1, &it, nullptr) == S_OK) {
                if (p.gen != g_gen) { it->Release(); break; }
                App a;
                PWSTR s = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &s))) { a.name = s; CoTaskMemFree(s); }
                s = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &s))) { a.id = s; CoTaskMemFree(s); }
                IShellItem2* it2 = nullptr;
                if (SUCCEEDED(it->QueryInterface(IID_PPV_ARGS(&it2)))) {
                    s = nullptr;
                    if (SUCCEEDED(it2->GetString(kTargetPath, &s))) { a.target = s; CoTaskMemFree(s); }
                    it2->Release();
                }
                a.low = Lower(a.name);
                std::wstring lid = Lower(a.id);
                bool skip = a.name.empty() || a.id.empty() || lid.rfind(L"http://", 0) == 0 || lid.rfind(L"https://", 0) == 0;
                for (auto ex : kExclude)
                    if (!skip && HasWord(a.low, ex)) skip = true;
                if (skip) { it->Release(); continue; }
                a.autoFolder = Categorize(a);
                list.push_back({std::move(a), it});
            }
            en->Release();
        }
        appsFolder->Release();
    }

    // Zusätzlich: Verknüpfungen auf dem Desktop, die nicht im Startmenü stehen
    // (z. B. Epic-Spiele wie Fortnite oder Rocket League: .url mit com.epicgames.launcher://…)
    if (p.gen == g_gen) {
        for (const KNOWNFOLDERID* fid : {&FOLDERID_Desktop, &FOLDERID_PublicDesktop}) {
            PWSTR dir = nullptr;
            if (FAILED(SHGetKnownFolderPath(*fid, 0, nullptr, &dir))) continue;
            std::wstring base = dir;
            CoTaskMemFree(dir);
            WIN32_FIND_DATAW fd;
            HANDLE fh = FindFirstFileW((base + L"\\*").c_str(), &fd);
            if (fh == INVALID_HANDLE_VALUE) continue;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::wstring fn = fd.cFileName;
                size_t dot = fn.find_last_of(L'.');
                if (dot == std::wstring::npos) continue;
                std::wstring ext = Lower(fn.substr(dot));
                if (ext != L".url" && ext != L".lnk") continue;
                std::wstring path = base + L"\\" + fn, target;
                if (ext == L".url") {
                    wchar_t url[2048] = {};
                    GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"", url, 2048, path.c_str());
                    target = url;
                    std::wstring lt = Lower(target);
                    // nur Programm-/Spiel-Links, keine Webseiten
                    if (target.empty() || lt.rfind(L"http://", 0) == 0 || lt.rfind(L"https://", 0) == 0 || lt.rfind(L"file:", 0) == 0) continue;
                } else {
                    IShellLinkW* sl = nullptr;
                    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
                        IPersistFile* pf = nullptr;
                        if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
                            if (SUCCEEDED(pf->Load(path.c_str(), STGM_READ))) {
                                wchar_t tp[MAX_PATH] = {};
                                if (SUCCEEDED(sl->GetPath(tp, MAX_PATH, nullptr, SLGP_RAWPATH))) target = tp;
                            }
                            pf->Release();
                        }
                        sl->Release();
                    }
                    if (Lower(target).size() < 4 || Lower(target).substr(Lower(target).size() - 4) != L".exe") continue;
                }
                App a;
                a.name = fn.substr(0, dot);
                a.low = Lower(a.name);
                a.id = path;
                a.file = path;
                a.target = target;
                bool dup = false, skip = false;
                for (auto& t : list)
                    if (t.a.low == a.low || (!target.empty() && Lower(t.a.target) == Lower(target))) { dup = true; break; }
                for (auto ex : kExclude)
                    if (HasWord(a.low, ex)) skip = true;
                if (dup || skip) continue;
                IShellItem* it = nullptr;
                if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&it)))) continue;
                a.autoFolder = Categorize(a);
                list.push_back({std::move(a), it});
            } while (FindNextFileW(fh, &fd));
            FindClose(fh);
        }
    }

    std::sort(list.begin(), list.end(), [](const Tmp& x, const Tmp& y) { return lstrcmpiW(x.a.name.c_str(), y.a.name.c_str()) < 0; });

    if (p.gen == g_gen) {
        for (size_t i = 0; i < list.size() && p.gen == g_gen; i++) {
            // Symbole direkt in den Anzeigegrößen holen: Windows wählt dann das passende
            // Bild aus der Symboldatei und skaliert hochwertig – beim Zeichnen wird nichts mehr skaliert.
            IShellItemImageFactory* f = nullptr;
            HBITMAP bmp = nullptr, smallBmp = nullptr;
            if (SUCCEEDED(list[i].item->QueryInterface(IID_PPV_ARGS(&f)))) {
                SIZE sz = {p.iconSize, p.iconSize};
                if (FAILED(f->GetImage(sz, SIIGBF_ICONONLY, &bmp))) bmp = nullptr;
                SIZE ss = {p.smallSize, p.smallSize};
                if (FAILED(f->GetImage(ss, SIIGBF_ICONONLY, &smallBmp))) smallBmp = nullptr;
                f->Release();
            }
            if (bmp) PremultiplyIfNeeded(bmp);
            if (smallBmp) PremultiplyIfNeeded(smallBmp);
            list[i].a.icon = bmp;
            list[i].a.iconSmall = smallBmp;
        }
        // fertige Liste mit allen Symbolen übergeben: die Leiste zeigt nie halb geladene Apps
        auto* out = new std::vector<App>();
        out->reserve(list.size());
        for (auto& t : list) out->push_back(t.a);
        auto* m = new ListMsg{p.gen, out};
        if (p.gen != g_gen || !PostMessageW(p.hwnd, WM_APP_LIST, 0, (LPARAM)m)) {
            for (auto& a : *out) {
                if (a.icon) DeleteObject(a.icon);
                if (a.iconSmall) DeleteObject(a.iconSmall);
            }
            delete out;
            delete m;
        }
    } else {
        for (auto& t : list) {
            if (t.a.icon) DeleteObject(t.a.icon);
            if (t.a.iconSmall) DeleteObject(t.a.iconSmall);
        }
    }

    for (auto& t : list) t.item->Release();
    PostMessageW(p.hwnd, WM_APP_DONE, (WPARAM)p.gen, 0);
    CoUninitialize();
    return 0;
}

static void StartLoad() {
    g.loading = true;
    auto stdSize = [](int px) {
        static const int sizes[] = {16, 20, 24, 32, 40, 48, 64};
        int best = sizes[0];
        for (int s : sizes)
            if (std::abs(s - px) < std::abs(best - px)) best = s;
        return best;
    };
    auto* p = new LoadParams{g_gen.load(), S(48), stdSize(S(28)), g.hwnd};
    HANDLE t = CreateThread(nullptr, 0, LoadThread, p, 0, nullptr);
    if (t) CloseHandle(t);
    else { delete p; g.loading = false; }
}

static void FreeBuffer() {
    if (g.memDC) {
        SelectObject(g.memDC, g.memOld);
        DeleteObject(g.memBmp);
        DeleteDC(g.memDC);
        g.memDC = nullptr;
        g.memBmp = nullptr;
        g.bits = nullptr;
        g.memW = g.memH = 0;
    }
    if (g.maskDC) {
        SelectObject(g.maskDC, g.maskOld);
        DeleteObject(g.maskBmp);
        DeleteDC(g.maskDC);
        g.maskDC = nullptr;
        g.maskBmp = nullptr;
        g.maskBits = nullptr;
    }
    std::vector<State::Shape>().swap(g.shapes);
    std::vector<State::TextRun>().swap(g.texts);
    std::vector<uint32_t>().swap(g.basePx);
    g.baseKey.clear();
    if (!g.img.empty()) {
        std::vector<uint32_t>().swap(g.img);
        g.imgW = g.imgH = 0;
        g.imgLoaded.clear();
    }
}

static void FreeData() {
    g_gen++;  // laufendes Laden abbrechen
    for (auto& a : g.apps) {
        if (a.icon) DeleteObject(a.icon);
        if (a.iconSmall) DeleteObject(a.iconSmall);
    }
    std::vector<App>().swap(g.apps);
    std::vector<AppFolder>().swap(g.folders);
    std::vector<AppFolder>().swap(g.allFolders);
    std::vector<int>().swap(g.hiddenApps);
    for (int& fv : g.favs) fv = -1;  // zeigten sonst auf die gelöschte App-Liste
    g.dragCand = g.dragging = false;
    std::vector<int>().swap(g.results);
    std::vector<Hit>().swap(g.hits);
    g.loaded = g.loading = false;
    g.hot = g.pressed = -1;
    FreeBuffer();
    TrimMemory();
}

// ---------------------------------------------------------------- Ordner & Suche

// Spielmodus je Ordner: Listenansicht mit Größe, Spielzeit, zuletzt gespielt (Standard: nur "Spiele")
static bool FolderGameMode(const std::wstring& name) {
    wchar_t b[8] = {};
    GetPrivateProfileStringW(L"Spielmodus", name.c_str(), name == L"Spiele" ? L"1" : L"0", b, 8, g.ini.c_str());
    return b[0] == L'1';
}
// Spiele in Ordnern im Spielmodus: deren Spielzeit misst HoverBar (sofern der Launcher sie nicht selbst kennt)
static void UpdateGameWatch() {
    std::vector<GameRef> list;
    for (auto& f : g.allFolders)
        if (FolderGameMode(f.name))
            for (int i : f.items) list.push_back({g.apps[i].id, g.apps[i].file, g.apps[i].name});
    Games_SetWatched(list);
}

// ---------------------------------------------------------------- Favoriten-Links und "Zuletzt genutzt"

static std::wstring LinkHost(std::wstring url) {  // "https://www.youtube.com/xyz" -> "youtube.com"
    size_t p = url.find(L"://");
    if (p != std::wstring::npos) url = url.substr(p + 3);
    url = url.substr(0, url.find_first_of(L"/?#"));
    url = Lower(url);
    if (url.rfind(L"www.", 0) == 0) url = url.substr(4);
    return url;
}
static std::wstring LinkNiceName(const std::wstring& host) {  // "youtube.com" -> "Youtube"
    std::wstring n = host.substr(0, host.find(L'.'));
    if (!n.empty()) n[0] = towupper(n[0]);
    return n.empty() ? host : n;
}
static std::wstring FaviconFile(const std::wstring& host) {
    return g.ini.substr(0, g.ini.find_last_of(L'\\')) + L"\\favicons\\" + host + L".v2.img";
}
// ist die Datei wirklich ein Bild (PNG, ICO, JPEG, GIF, WebP)? Manche Seiten liefern statt des Symbols eine HTML-Seite
static bool IsImageFile(const std::wstring& file) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    unsigned char b[12] = {};
    DWORD got = 0, size = GetFileSize(h, nullptr);
    ReadFile(h, b, sizeof(b), &got, nullptr);
    CloseHandle(h);
    if (got < 12 || size < 64) return false;
    return (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') || (b[0] == 0 && b[1] == 0 && b[2] == 1 && b[3] == 0) ||
           (b[0] == 0xFF && b[1] == 0xD8) || (b[0] == 'G' && b[1] == 'I' && b[2] == 'F') ||
           (b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F' && b[8] == 'W' && b[9] == 'E' && b[10] == 'B' && b[11] == 'P');
}
// Symbol-Adressen aus der Startseite (<link rel="...icon..." sizes="..." href="...">), größtes zuerst
static std::vector<std::wstring> FaviconCandidates(const std::wstring& host) {
    std::string html;
    IStream* st = nullptr;
    if (SUCCEEDED(URLOpenBlockingStreamW(nullptr, (L"https://" + host + L"/").c_str(), &st, 0, nullptr))) {
        char buf[8192];
        ULONG got = 0;
        while (SUCCEEDED(st->Read(buf, sizeof(buf), &got)) && got > 0 && html.size() < 2 * 1024 * 1024) html.append(buf, got);
        st->Release();
    }
    std::string low = html;
    for (auto& c : low) c = (char)tolower((unsigned char)c);
    auto attr = [&](size_t a, size_t e, const char* name) -> std::string {
        std::string pat = std::string(name) + "=";
        size_t p = low.find(pat, a);
        while (p != std::string::npos && p < e && p > 0 && isalnum((unsigned char)low[p - 1])) p = low.find(pat, p + 1);  // "data-href=" usw. überspringen
        if (p == std::string::npos || p >= e) return "";
        p += pat.size();
        char q = html[p];
        if (q == '"' || q == '\'') { size_t z = html.find(q, p + 1); return z == std::string::npos || z > e ? "" : html.substr(p + 1, z - p - 1); }
        size_t z = html.find_first_of(" >", p);
        return html.substr(p, (z == std::string::npos ? e : std::min(z, e)) - p);
    };
    std::vector<std::pair<int, std::wstring>> found;
    size_t a = 0;
    while ((a = low.find("<link", a)) != std::string::npos) {
        size_t e = low.find('>', a);
        if (e == std::string::npos) break;
        std::string rel = attr(a, e, "rel"), href = attr(a, e, "href"), sizes = attr(a, e, "sizes");
        for (auto& c : rel) c = (char)tolower((unsigned char)c);
        a = e;
        if (rel.find("icon") == std::string::npos || rel.find("mask") != std::string::npos || href.empty()) continue;
        std::string hl = href;
        for (auto& c : hl) c = (char)tolower((unsigned char)c);
        if (hl.find(".svg") != std::string::npos || hl.rfind("data:", 0) == 0) continue;  // SVG kann Windows hier nicht zeichnen
        int score = rel.find("apple-touch") != std::string::npos ? 180 : 32;
        if (!sizes.empty()) score = std::max(score / 2, atoi(sizes.c_str()));
        size_t amp;
        while ((amp = href.find("&amp;")) != std::string::npos) href.replace(amp, 5, "&");
        std::wstring w(href.begin(), href.end());  // Adressen sind (fast immer) ASCII
        if (w.rfind(L"//", 0) == 0) w = L"https:" + w;
        else if (w.rfind(L"http", 0) != 0) w = L"https://" + host + (w[0] == L'/' ? L"" : L"/") + w;
        found.push_back({std::min(score, 512), w});
    }
    std::stable_sort(found.begin(), found.end(), [](auto& x, auto& y) { return x.first > y.first; });
    std::vector<std::wstring> out;
    for (auto& f : found) out.push_back(f.second);
    out.push_back(L"https://" + host + L"/apple-touch-icon.png");
    out.push_back(L"https://" + host + L"/favicon.ico");
    return out;
}
// Website-Symbol im Hintergrund holen: das größte Symbol, das die Seite selbst angibt, sonst Apple-Symbol / favicon.ico
static void StartFavicon(const std::wstring& host) {
    std::wstring file = FaviconFile(host);
    if (host.empty()) return;
    if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (IsImageFile(file)) return;
        DeleteFileW(file.c_str());  // kaputtes Symbol (z. B. HTML-Seite) neu holen
    }
    DeleteFileW((file.substr(0, file.size() - 7) + L".img").c_str());  // alter Zwischenspeicher (vor v2)
    struct Job { std::wstring host, file; HWND hwnd; };
    auto* j = new Job{host, file, g.hwnd};
    HANDLE t = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        auto* j = (Job*)p;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        SHCreateDirectoryExW(nullptr, j->file.substr(0, j->file.find_last_of(L'\\')).c_str(), nullptr);
        std::wstring tmp = j->file + L".tmp";
        int tries = 0;
        for (auto& u : FaviconCandidates(j->host)) {
            if (++tries > 6) break;
            if (SUCCEEDED(URLDownloadToFileW(nullptr, u.c_str(), tmp.c_str(), 0, nullptr)) && IsImageFile(tmp) &&
                MoveFileExW(tmp.c_str(), j->file.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                PostMessageW(j->hwnd, WM_APP_FAVICON, 0, 0);
                break;
            }
            DeleteFileW(tmp.c_str());
        }
        CoUninitialize();
        delete j;
        return 0;
    }, j, 0, nullptr);
    if (t) CloseHandle(t);
    else delete j;
}
static void LoadLinks() {
    g.links.clear();
    int n = GetPrivateProfileIntW(L"Links", L"Anzahl", 0, g.ini.c_str());
    for (int i = 1; i <= n; i++) {
        std::wstring v = IniGet(L"Links", L"Link" + std::to_wstring(i));
        size_t t = v.find(L'\t');
        if (v.empty()) continue;
        State::FavLink l;
        l.url = v.substr(0, t);
        l.name = t == std::wstring::npos ? L"" : v.substr(t + 1);
        l.host = LinkHost(l.url);
        if (l.name.empty()) l.name = LinkNiceName(l.host);
        g.links.push_back(l);
    }
}
static void SaveLinks() {
    WritePrivateProfileStringW(L"Links", nullptr, nullptr, g.ini.c_str());  // Abschnitt leeren
    IniSet(L"Links", L"Anzahl", std::to_wstring(g.links.size()).c_str());
    for (size_t i = 0; i < g.links.size(); i++)
        IniSet(L"Links", L"Link" + std::to_wstring(i + 1), (g.links[i].url + L"\t" + g.links[i].name).c_str());
}
// zuletzt genutzt: "a:<App-Kennung>" oder "l:<Link>", neueste zuerst, höchstens 12
static void AddRecent(const std::wstring& key) {
    std::vector<std::wstring> list = {key};
    std::wstring all = IniGetLong(L"Zuletzt", L"Liste");
    for (size_t a = 0; a < all.size();) {
        size_t b = all.find(L'\n', a);
        std::wstring k = all.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
        if (!k.empty() && k != key && list.size() < 12) list.push_back(k);
        if (b == std::wstring::npos) break;
        a = b + 1;
    }
    std::wstring out;
    for (auto& k : list) out += (out.empty() ? L"" : L"\n") + k;
    // Zeilenumbrüche sind in ini-Werten nicht erlaubt: als "\x1" speichern
    std::replace(out.begin(), out.end(), L'\n', L'\x1');
    IniSet(L"Zuletzt", L"Liste", out.c_str());
}
static void BuildRecent() {
    g.recent.clear();
    std::wstring all = IniGetLong(L"Zuletzt", L"Liste");
    std::replace(all.begin(), all.end(), L'\x1', L'\n');
    for (size_t a = 0; a < all.size();) {
        size_t b = all.find(L'\n', a);
        std::wstring k = all.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
        if (k.size() > 2 && k[1] == L':') {
            State::RecentEntry e = {k[0] == L'l', -1, L"", L"", L""};
            std::wstring v = k.substr(2);
            if (e.link) {
                e.url = v;
                e.host = LinkHost(v);
                e.name = LinkNiceName(e.host);
                for (auto& l : g.links)
                    if (l.url == v) e.name = l.name;
                g.recent.push_back(e);
            } else {
                for (int i = 0; i < (int)g.apps.size(); i++)
                    if (g.apps[i].id == v) { e.app = i; g.recent.push_back(e); break; }
            }
        }
        if (b == std::wstring::npos) break;
        a = b + 1;
    }
}

// ---------------------------------------------------------------- Kalender

static const wchar_t* kMonths[12] = {L"Januar", L"Februar", L"März", L"April", L"Mai", L"Juni",
                                     L"Juli", L"August", L"September", L"Oktober", L"November", L"Dezember"};
static int Today() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    return t.wYear * 10000 + t.wMonth * 100 + t.wDay;
}
static int DaysIn(int y, int m) {
    static const int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : d[m - 1];
}
static int WeekdayMon0(int y, int m, int d) {  // 0 = Montag … 6 = Sonntag
    static const int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y--;
    int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;  // 0 = Sonntag
    return (w + 6) % 7;
}
static std::wstring EventsFile() { return g.ini.substr(0, g.ini.find_last_of(L'\\')) + L"\\termine.txt"; }
static void LoadEvents() {
    g.eventsLoaded = true;
    g.events.clear();
    HANDLE h = CreateFileW(EventsFile().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    std::string data((size_t)sz.QuadPart, '\0');
    DWORD rd = 0;
    if (!data.empty()) ReadFile(h, &data[0], (DWORD)data.size(), &rd, nullptr);
    CloseHandle(h);
    int n = MultiByteToWideChar(CP_UTF8, 0, data.data(), (int)rd, nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, data.data(), (int)rd, &w[0], n);
    for (size_t a = 0; a < w.size();) {  // Zeilen: 20261124<TAB>14:30<TAB>Titel
        size_t b = w.find(L'\n', a);
        std::wstring line = w.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t t1 = line.find(L'\t'), t2 = t1 == std::wstring::npos ? t1 : line.find(L'\t', t1 + 1);
        if (t2 != std::wstring::npos) g.events.push_back({_wtoi(line.c_str()), line.substr(t1 + 1, t2 - t1 - 1), line.substr(t2 + 1)});
        if (b == std::wstring::npos) break;
        a = b + 1;
    }
}
static void SaveEvents() {
    std::wstring w;
    for (auto& e : g.events) w += std::to_wstring(e.date) + L"\t" + e.time + L"\t" + e.title + L"\r\n";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &u[0], n, nullptr, nullptr);
    std::wstring tmp = EventsFile() + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr;
    WriteFile(h, u.data(), (DWORD)u.size(), &wr, nullptr);
    CloseHandle(h);
    MoveFileExW(tmp.c_str(), EventsFile().c_str(), MOVEFILE_REPLACE_EXISTING);
}
// Termine eines Tages, nach Uhrzeit sortiert (ganztägige zuerst); liefert Indizes in g.events
static std::vector<int> EventsOn(int date) {
    std::vector<int> v;
    for (int i = 0; i < (int)g.events.size(); i++)
        if (g.events[i].date == date) v.push_back(i);
    std::sort(v.begin(), v.end(), [](int a, int b) { return g.events[a].time < g.events[b].time; });
    return v;
}
static std::wstring DateText(int date) {
    int y = date / 10000, m = date / 100 % 100, d = date % 100;
    static const wchar_t* wd[7] = {L"Montag", L"Dienstag", L"Mittwoch", L"Donnerstag", L"Freitag", L"Samstag", L"Sonntag"};
    return std::wstring(wd[WeekdayMon0(y, m, d)]) + L", " + std::to_wstring(d) + L". " + kMonths[m - 1] + L" " + std::to_wstring(y);
}

static void BuildFolders() {
    for (auto& a : g.apps) {
        a.hidden = IniGet(L"Ausgeblendet", a.id) == L"1";
        a.launches = (int)GetPrivateProfileIntW(L"Starts", a.id.c_str(), 0, g.ini.c_str());
        a.folder = a.autoFolder;
    }
    auto names = AllFolderNames();
    for (auto& a : g.apps) {
        std::wstring o = IniGet(L"Zuordnung", a.id);
        if (o == L"-") a.folder.clear();  // bewusst aus allen Ordnern genommen
        else if (!o.empty() && std::find(names.begin(), names.end(), o) != names.end()) a.folder = o;
        // Ordner gelöscht: nach "Sonstiges", sonst in keinen Ordner
        if (!a.folder.empty() && std::find(names.begin(), names.end(), a.folder) == names.end())
            a.folder = std::find(names.begin(), names.end(), std::wstring(kOther)) != names.end() ? kOther : L"";
    }
    g.folders.clear();
    g.allFolders.clear();
    g.hiddenApps.clear();
    for (int i = 0; i < (int)g.apps.size(); i++)
        if (g.apps[i].hidden) g.hiddenApps.push_back(i);
    for (auto& n : names) {
        AppFolder f;
        f.name = n;
        for (int i = 0; i < (int)g.apps.size(); i++)
            if (!g.apps[i].hidden && g.apps[i].folder == n) f.items.push_back(i);
        // Standard: oft gestartete Apps zuerst
        std::stable_sort(f.items.begin(), f.items.end(), [](int a, int b) { return g.apps[a].launches > g.apps[b].launches; });
        // Eigene Reihenfolge aus den Einstellungen hat Vorrang
        auto order = SplitList(IniGetLong(L"Reihenfolge", n));
        if (!order.empty()) {
            std::vector<int> sorted;
            for (auto& id : order)
                for (int i : f.items)
                    if (g.apps[i].id == id && std::find(sorted.begin(), sorted.end(), i) == sorted.end()) { sorted.push_back(i); break; }
            for (int i : f.items)
                if (std::find(sorted.begin(), sorted.end(), i) == sorted.end()) sorted.push_back(i);
            f.items = sorted;
        }
        g.allFolders.push_back(f);
        if (!f.items.empty()) g.folders.push_back(std::move(f));
    }
    for (int k = 0; k < 4; k++) {
        g.favs[k] = -1;
        std::wstring id = IniGet(L"Favoriten", L"Platz" + std::to_wstring(k + 1));
        if (id.empty()) continue;
        for (int i = 0; i < (int)g.apps.size(); i++)
            if (g.apps[i].id == id) { g.favs[k] = i; break; }
    }
    if (g.curFolder >= (int)g.folders.size()) { g.curFolder = -1; if (g.view == View::Folder) g.view = View::Folders; }
    UpdateGameWatch();
}

static void SaveOrder(const AppFolder& f) {
    std::vector<std::wstring> ids;
    for (int i : f.items) ids.push_back(g.apps[i].id);
    IniSet(L"Reihenfolge", f.name, JoinList(ids).c_str());
}

static void SaveFolderOrder() {
    std::vector<std::wstring> names;
    for (auto& f : g.allFolders) names.push_back(f.name);
    IniSet(L"Ordner", L"Reihenfolge", JoinList(names).c_str());
}

static int FolderOfApp(int app) {
    for (int f = 0; f < (int)g.allFolders.size(); f++)
        for (int i : g.allFolders[f].items)
            if (i == app) return f;
    return -1;
}

// App an eine Stelle in einem Ordner legen (egal, ob sie vorher in einem anderen oder in keinem Ordner war)
static void MoveApp(int app, int toF, int toPos) {
    if (toF < 0 || toF >= (int)g.allFolders.size()) return;
    int fromF = FolderOfApp(app);
    if (fromF >= 0) {
        auto& src = g.allFolders[fromF].items;
        auto it = std::find(src.begin(), src.end(), app);
        int oldPos = (int)(it - src.begin());
        src.erase(it);
        if (fromF == toF && oldPos < toPos) toPos--;
    }
    auto& dst = g.allFolders[toF].items;
    toPos = std::max(0, std::min(toPos, (int)dst.size()));
    dst.insert(dst.begin() + toPos, app);
    const std::wstring& name = g.allFolders[toF].name;
    IniSet(L"Zuordnung", g.apps[app].id, name == g.apps[app].autoFolder ? nullptr : name.c_str());
    if (fromF >= 0 && fromF != toF) SaveOrder(g.allFolders[fromF]);
    SaveOrder(g.allFolders[toF]);
    BuildFolders();
}

// Favoriten-Feld k mit App belegen (-1 = leeren); ist die App schon in einem anderen Feld, werden die Felder getauscht
static void SetFav(int k, int app) {
    if (k < 0 || k > 3) return;
    if (app >= 0)
        for (int j = 0; j < 4; j++)
            if (j != k && g.favs[j] == app) {
                g.favs[j] = g.favs[k];
                IniSet(L"Favoriten", L"Platz" + std::to_wstring(j + 1), g.favs[j] >= 0 ? g.apps[g.favs[j]].id.c_str() : nullptr);
            }
    g.favs[k] = app;
    IniSet(L"Favoriten", L"Platz" + std::to_wstring(k + 1), app >= 0 ? g.apps[app].id.c_str() : nullptr);
}

static int FavIndexOf(int app) {
    for (int k = 0; k < 4; k++)
        if (g.favs[k] == app && app >= 0) return k;
    return -1;
}

// App aus ihrem Ordner nehmen: sie bleibt über die Suche erreichbar
static void RemoveFromFolder(int app) {
    int fromF = FolderOfApp(app);
    if (fromF >= 0) {
        auto& src = g.allFolders[fromF].items;
        src.erase(std::find(src.begin(), src.end(), app));
        SaveOrder(g.allFolders[fromF]);
    }
    IniSet(L"Zuordnung", g.apps[app].id, L"-");
    BuildFolders();
}

static void MoveFolder(int f, int delta) {
    int t = f + delta;
    if (f < 0 || t < 0 || f >= (int)g.allFolders.size() || t >= (int)g.allFolders.size()) return;
    std::swap(g.allFolders[f], g.allFolders[t]);
    SaveFolderOrder();
    BuildFolders();
}

static void CreateFolder(std::wstring name) {
    name.erase(std::remove(name.begin(), name.end(), L'|'), name.end());
    while (!name.empty() && name.back() == L' ') name.pop_back();
    while (!name.empty() && name.front() == L' ') name.erase(0, 1);
    if (name.empty()) return;
    auto all = AllFolderNames();
    if (std::find(all.begin(), all.end(), name) != all.end()) return;
    auto removed = RemovedFolders();
    if (std::find(removed.begin(), removed.end(), name) != removed.end()) {
        // gelöschten Standardordner wiederherstellen
        removed.erase(std::remove(removed.begin(), removed.end(), name), removed.end());
        IniSet(L"Ordner", L"Entfernt", JoinList(removed).c_str());
    } else {
        auto c = CustomFolders();
        c.push_back(name);
        SaveCustomFolders(c);
    }
    BuildFolders();
}

// Ordner löschen; seine Apps werden neu einsortiert (Standardordner werden nur ausgeblendet)
static void DeleteFolder(const std::wstring& name) {
    auto custom = CustomFolders();
    if (std::find(custom.begin(), custom.end(), name) != custom.end()) {
        custom.erase(std::remove(custom.begin(), custom.end(), name), custom.end());
        SaveCustomFolders(custom);
    } else {
        auto removed = RemovedFolders();
        removed.push_back(name);
        IniSet(L"Ordner", L"Entfernt", JoinList(removed).c_str());
    }
    IniSet(L"Reihenfolge", name, nullptr);
    for (auto& a : g.apps)
        if (IniGet(L"Zuordnung", a.id) == name) IniSet(L"Zuordnung", a.id, nullptr);
    BuildFolders();
}

static bool ConfirmDeleteFolder(const std::wstring& name) {
    std::wstring msg = L"Ordner \u201E" + name + L"\u201C löschen?\n\nDie Apps darin werden automatisch neu einsortiert. "
                       L"Wenn du den Ordner später mit gleichem Namen neu anlegst, ist er wieder da.";
    g.menuOpen = true;
    int r = MessageBoxW(g.hwnd, msg.c_str(), L"HoverBar", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_TOPMOST);
    g.menuOpen = false;
    SetForegroundWindow(g.hwnd);
    return r == IDYES;
}

static void UpdateSearch() {
    std::wstring q = Lower(g.query);
    g.results.clear();
    if (q.empty()) return;
    std::vector<int> pre, rest;
    for (int i = 0; i < (int)g.apps.size(); i++) {
        if (g.apps[i].hidden) continue;
        size_t pos = g.apps[i].low.find(q);
        if (pos == 0) pre.push_back(i);
        else if (pos != std::wstring::npos) rest.push_back(i);
    }
    g.results = pre;
    g.results.insert(g.results.end(), rest.begin(), rest.end());
}

// ---------------------------------------------------------------- Layout

static int ContentTop() { return g.view == View::Music ? S(176) : S(108); }  // Bibliothek: feste Reihe Favoriten/Playlists/Zuletzt
static bool DuckEnabled() { return g.duckOn; }
static HBITMAP MakeLogoBitmap(int px);
static std::wstring HeaderTitle();
static std::vector<int> NavPages();
static int ContentBottom();
static SIZE_T RunningRam(int idx);
static bool ConfirmBox(const std::wstring& msg);
static bool SpotifyMode() { return g.musicSrc == 1; }

// Breite: normal ein Drittel des Bildschirms, die Ordner-Einstellungen etwas breiter
static int RailW() { return S(58); }  // Navigationsleiste links (Home, Musik, Kalender)
static int WantedWidth() { return (g.view == View::Settings && g.setTab == 1) || g.view == View::MusicSettings ? g.wWide : g.wNormal; }  // beide inkl. Navigationsleiste

static void UpdateAllList() {
    g.allList.clear();
    std::wstring q = Lower(g.creatingFolder ? L"" : g.query);
    for (int i = 0; i < (int)g.apps.size(); i++)
        if (q.empty() || g.apps[i].low.find(q) != std::wstring::npos) g.allList.push_back(i);
}

static void LayoutDesign(int W, int pad, int y0) {
    int cw = W - 2 * pad, ip = S(8), headH = S(42), gap = S(12);
    int rowH2 = S(48), labelW = S(140);
    auto row = [&](int& ry, int label) {
        RECT rr = {pad + ip, ry, pad + cw - ip, ry + rowH2};
        g.hits.push_back({rr, HT::SetRowLabel, label});
        ry += rowH2;
        return rr;
    };
    auto toggle = [&](RECT rr, int idx) {
        int tw = S(46), th = S(26), ty = rr.top + (rowH2 - th) / 2;
        g.hits.push_back({{rr.right - tw - S(6), ty, rr.right - S(6), ty + th}, HT::SetToggle, idx});
    };
    auto slider = [&](RECT rr, int idx) {
        g.hits.push_back({{rr.left + labelW, rr.top, rr.right - S(60), rr.bottom}, HT::SetSlider, idx});
    };
    int y = y0;
    {   // Aussehen
        int rows = 6 + (g.blur ? 1 : 0) + (g.imgPath.empty() ? 0 : 1);
        int ch = headH + rows * rowH2 + ip;
        g.hits.push_back({{pad, y, pad + cw, y + ch}, HT::SetCard, -2, -2});
        g.hits.push_back({{pad, y, pad + cw, y + headH}, HT::SetSection, 0});
        int ry = y + headH;
        toggle(row(ry, 1), 0);                  // Glas-Effekt
        if (g.blur) slider(row(ry, 7), 2);      // Unschärfe
        slider(row(ry, 0), 0);                  // Transparenz / Tönung
        RECT r2 = row(ry, 2);                   // Farbe
        int d = S(24), step = S(31), x = r2.left + labelW;
        for (int k = 0; k <= kSwatchCount && x + d <= r2.right; k++, x += step)
            g.hits.push_back({{x, r2.top + (rowH2 - d) / 2, x + d, r2.top + (rowH2 - d) / 2 + d}, HT::SetSwatch, k});
        RECT r3 = row(ry, 3);                   // Hintergrundbild
        int bh = S(32), by = r3.top + (rowH2 - bh) / 2, bx = r3.left + labelW;
        g.hits.push_back({{bx, by, bx + S(130), by + bh}, HT::SetButton, 0});
        g.hits.push_back({{bx + S(138), by, bx + S(138) + S(100), by + bh}, HT::SetButton, 1});
        if (!g.imgPath.empty()) slider(row(ry, 4), 1);  // Bildstärke
        toggle(row(ry, 25), 20);                // Cover in den Musik-Kacheln
        toggle(row(ry, 26), 21);                // Trennlinien zwischen Songs
        y += ch + gap;
    }
    {   // Schrift & Bewegung
        int ch = headH + 3 * rowH2 + ip;
        g.hits.push_back({{pad, y, pad + cw, y + ch}, HT::SetCard, -4, -4});
        g.hits.push_back({{pad, y, pad + cw, y + headH}, HT::SetSection, 2});
        int ry = y + headH;
        RECT r8 = row(ry, 8);  // Schriftart
        int bh = S(32), by = r8.top + (rowH2 - bh) / 2;
        g.hits.push_back({{r8.left + labelW, by, r8.right - S(6), by + bh}, HT::SetButton, 2});
        toggle(row(ry, 9), 3);  // fett
        RECT r10 = row(ry, 10);  // Ausfahren
        int sx = r10.left + labelW, sw = (r10.right - S(6) - sx) / 4;
        by = r10.top + (rowH2 - bh) / 2;
        for (int k = 0; k < 4; k++)
            g.hits.push_back({{sx + k * sw, by, sx + (k + 1) * sw, by + bh}, HT::SetSpeed, k});
        y += ch + gap;
    }
    {   // Allgemein
        int ch = headH + 3 * rowH2 + ip;
        g.hits.push_back({{pad, y, pad + cw, y + ch}, HT::SetCard, -3, -3});
        g.hits.push_back({{pad, y, pad + cw, y + headH}, HT::SetSection, 1});
        int ry = y + headH;
        toggle(row(ry, 5), 1);
        toggle(row(ry, 6), 2);
        RECT r11 = row(ry, 11);  // Taste zum Öffnen
        int bh = S(32), by = r11.top + (rowH2 - bh) / 2;
        g.hits.push_back({{r11.left + labelW + S(40), by, r11.right - S(6), by + bh}, HT::SetButton, 3});
        y += ch + gap;
    }
    g.contentH = y - y0;
}

// Funktionen: Schalter für Musikquelle, Leiser bei Videos, Tasten
// Erweiterungen: optionale Erweiterungen, die man bei Bedarf installiert
static void LayoutSuggested(int W, int pad, int y0) {
    int cw = W - 2 * pad, y = y0;
    static bool askedIcon = false;
    if (!askedIcon) { askedIcon = true; StartFavicon(L"claude.ai"); }  // öffentliches Claude-Symbol für die Karte (einmalig, nur beim Öffnen dieses Reiters)
    bool have = !WizTree_Exe().empty();
    int ch = S(196);
    g.hits.push_back({{pad, y, pad + cw, y + ch}, HT::PlugCard, 0});
    int bw = S(150), bh = S(36), by = y + ch - S(14) - bh;
    g.hits.push_back({{pad + cw - S(16) - bw, by, pad + cw - S(16), by + bh}, HT::PlugBtn, 0});  // Installieren / Öffnen
    if (have) g.hits.push_back({{pad + cw - S(16) - 2 * bw - S(10), by, pad + cw - S(16) - bw - S(10), by + bh}, HT::PlugBtn, 1});  // Entfernen
    y += ch + S(12);
    {   // Claude-Chat: wird nur hinzugefügt (nichts herunterzuladen)
        int ch2 = S(168);
        g.hits.push_back({{pad, y, pad + cw, y + ch2}, HT::PlugCard, 1});
        int by2 = y + ch2 - S(14) - bh;
        g.hits.push_back({{pad + cw - S(16) - bw, by2, pad + cw - S(16), by2 + bh}, HT::PlugBtn, 10});  // Hinzufügen / Öffnen
        if (g.claudeOn) g.hits.push_back({{pad + cw - S(16) - 2 * bw - S(10), by2, pad + cw - S(16) - bw - S(10), by2 + bh}, HT::PlugBtn, 11});  // Entfernen
        y += ch2 + S(12);
    }
    {   // Musik-Sync: Songs und Playlists in einem Cloud-Ordner
        int ch3 = S(216);
        g.hits.push_back({{pad, y, pad + cw, y + ch3}, HT::PlugCard, 2});
        int by3 = y + ch3 - S(14) - bh;
        int bw3 = std::min(bw, (cw - 2 * S(16) - 2 * S(8)) / 3);  // drei Knöpfe müssen nebeneinander in die Karte passen
        g.hits.push_back({{pad + cw - S(16) - bw3, by3, pad + cw - S(16), by3 + bh}, HT::PlugBtn, 20});  // Einrichten / Jetzt abgleichen
        if (g.syncOn) {
            g.hits.push_back({{pad + cw - S(16) - 2 * bw3 - S(8), by3, pad + cw - S(16) - bw3 - S(8), by3 + bh}, HT::PlugBtn, 21});  // Ausschalten
            g.hits.push_back({{pad + cw - S(16) - 3 * bw3 - S(16), by3, pad + cw - S(16) - 2 * bw3 - S(16), by3 + bh}, HT::PlugBtn, 22});  // Handy verbinden
        }
        y += ch3 + S(12);
    }
    {   // Timer
        int ch4 = S(130);
        g.hits.push_back({{pad, y, pad + cw, y + ch4}, HT::PlugCard, 3});
        int by4 = y + ch4 - S(14) - bh;
        g.hits.push_back({{pad + cw - S(16) - bw, by4, pad + cw - S(16), by4 + bh}, HT::PlugBtn, 12});
        if (g.timerOn) g.hits.push_back({{pad + cw - S(16) - 2 * bw - S(10), by4, pad + cw - S(16) - bw - S(10), by4 + bh}, HT::PlugBtn, 13});
        y += ch4 + S(12);
    }
    g.contentH = y - y0;
}

static void LayoutFunctions(int W, int pad, int y0) {
    int cw = W - 2 * pad, ip = S(8), headH = S(42), gap = S(12), rowH2 = S(58);
    auto rowToggle = [&](int& ry, int label, int idx) {
        RECT rr = {pad + ip, ry, pad + cw - ip, ry + rowH2};
        g.hits.push_back({rr, HT::SetRowLabel, label});
        int tw = S(46), th = S(26), ty = rr.top + (rowH2 - th) / 2;
        g.hits.push_back({{rr.right - tw - S(6), ty, rr.right - S(6), ty + th}, HT::SetToggle, idx});
        ry += rowH2;
    };
    int y = y0;
    {   // HoverBar selbst: Version und Update holen
        int ch = headH + 3 * rowH2 + ip;
        g.hits.push_back({{pad, y, pad + cw, y + ch}, HT::SetCard, -8, -8});
        g.hits.push_back({{pad, y, pad + cw, y + headH}, HT::SetSection, 7});
        RECT rr = {pad + ip, y + headH, pad + cw - ip, y + headH + rowH2};
        g.hits.push_back({rr, HT::SetRowLabel, 24});
        int bw = S(170), bh = S(34), by = rr.top + (rowH2 - bh) / 2;
        g.hits.push_back({{rr.right - bw - S(6), by, rr.right - S(6), by + bh}, HT::UpdateBtn, 0});
        int ry2 = y + headH + rowH2;
        rowToggle(ry2, 27, 22);  // Home (Apps)
        rowToggle(ry2, 33, 23);  // Kalender
        y += ch + gap;
    }
    {   // Musik: Hauptschalter, die übrigen Optionen nur, wenn er an ist
        int ch = headH + (g.musicOn ? 6 : 1) * rowH2 + ip;
        g.hits.push_back({{pad, y, pad + cw, y + ch}, HT::SetCard, -5, -5});
        g.hits.push_back({{pad, y, pad + cw, y + headH}, HT::SetSection, 4});
        int ry = y + headH;
        rowToggle(ry, 17, 15);  // Musik-Player an/aus
        if (g.musicOn) {
            rowToggle(ry, 12, 10);  // HoverBar-Player
            rowToggle(ry, 13, 11);  // Spotify
            rowToggle(ry, 14, 12);  // Leiser bei Videos
            // yt-dlp: Version anzeigen, neueste suchen und herunterladen
            RECT rr = {pad + ip, ry, pad + cw - ip, ry + rowH2};
            g.hits.push_back({rr, HT::SetRowLabel, 22});
            int bw = S(150), bh = S(34), by = rr.top + (rowH2 - bh) / 2;
            g.hits.push_back({{rr.right - bw - S(6), by, rr.right - S(6), by + bh}, HT::ToolBtn, 0});
            ry += rowH2;
            Music_ToolCheckLocal(g.hwnd);  // installierte Version im Hintergrund ermitteln (einmal)
            // Song-Ordner wählen
            RECT fr = {pad + ip, ry, pad + cw - ip, ry + rowH2};
            g.hits.push_back({fr, HT::SetRowLabel, 23});
            g.hits.push_back({{fr.right - bw - S(6), fr.top + (rowH2 - bh) / 2, fr.right - S(6), fr.top + (rowH2 + bh) / 2}, HT::FolderBtn, 0});
            ry += rowH2;
        }
        y += ch + gap;
    }
    {   // Spiele: welche Launcher für die Spielmodus-Liste ausgelesen werden
        int ch = headH + 4 * rowH2 + ip;
        g.hits.push_back({{pad, y, pad + cw, y + ch}, HT::SetCard, -7, -7});
        g.hits.push_back({{pad, y, pad + cw, y + headH}, HT::SetSection, 6});
        int ry = y + headH;
        rowToggle(ry, 18, 16);  // Steam
        rowToggle(ry, 19, 17);  // Epic Games
        rowToggle(ry, 20, 18);  // Riot Games
        rowToggle(ry, 21, 19);  // andere Spiele selbst messen
        y += ch + gap;
    }
    if (g.musicOn) {   // Tasten (gehören alle zur Musik)
        int ch = headH + (g.musicKeys ? 7 : 2) * rowH2 + ip;
        g.hits.push_back({{pad, y, pad + cw, y + ch}, HT::SetCard, -6, -6});
        g.hits.push_back({{pad, y, pad + cw, y + headH}, HT::SetSection, 5});
        int ry = y + headH;
        rowToggle(ry, 15, 13);  // Musik-Tasten
        for (int i = 0; g.musicKeys && i < 5; i++, ry += rowH2) {
            RECT rr = {pad + ip, ry, pad + cw - ip, ry + rowH2};
            g.hits.push_back({rr, HT::SetRowLabel, 28 + i});
            g.hits.push_back({{rr.right - S(166), ry + S(12), rr.right - S(6), ry + rowH2 - S(12)}, HT::CtlKeyBtn, i});
        }
        rowToggle(ry, 16, 14);  // Link laden
        g.hits.push_back({{pad + cw - ip - S(222), ry - rowH2 + S(12), pad + cw - ip - S(62), ry - S(12)}, HT::GrabKeyBtn, 0});
        y += ch + gap;
    }
    g.contentH = y - y0;
}

// Ordner-Seite: links die Ordner (umsortieren per Ziehen), rechts alle Apps alphabetisch mit "+"
static void LayoutFolders(int W, int H, int pad) {
    int top = ContentTop(), gapP = S(12);
    g.paneMid = W / 2;
    RECT L = {pad, top, g.paneMid - gapP / 2, H - S(18)};
    RECT R = {g.paneMid + gapP / 2, top, W - pad, H - S(18)};
    g.hits.push_back({L, HT::Pane, 0});
    g.hits.push_back({R, HT::Pane, 1});
    g.hits.push_back({{L.left, L.top, L.right, L.top + S(44)}, HT::PaneTitle, 0});
    g.hits.push_back({{R.left, R.top, R.right, R.top + S(44)}, HT::PaneTitle, 1});

    // links
    g.lTop = L.top + S(44);
    g.lBot = L.bottom - S(58);
    int ip = S(8), headH = S(38), rowH = S(38), btn = S(28);
    int y = g.lTop - g.scroll;
    {
        int cwL = (L.right - L.left) - 2 * ip;
        int gapF = S(8), side = std::min(S(96), (cwL - S(16) - 3 * gapF) / 4);
        int ch = S(34) + side + S(12);
        RECT card = {L.left + ip, y, L.right - ip, y + ch};
        g.hits.push_back({card, HT::SetCard, -6, -6, 0, 0, 1});
        g.hits.push_back({{card.left, y, card.right, y + S(34)}, HT::SetSection, 3, -1, 0, 0, 1});
        int total = 4 * side + 3 * gapF, sx = card.left + (card.right - card.left - total) / 2;
        for (int k = 0; k < 4; k++) {
            int x = sx + k * (side + gapF);
            g.hits.push_back({{x, y + S(34), x + side, y + S(34) + side}, HT::FavSlot, g.favs[k] < (int)g.apps.size() ? g.favs[k] : -1, -1, k, 0, 1});
        }
        y += ch + S(10);
    }
    int nf = (int)g.allFolders.size();
    for (int fi = 0; fi < nf; fi++) {
        auto& items = g.allFolders[fi].items;
        int ni = (int)items.size();
        int ch = headH + (ni ? ni * rowH : S(40)) + S(6);
        RECT card = {L.left + ip, y, L.right - ip, y + ch};
        g.hits.push_back({card, HT::SetCard, fi, fi, 0, 0, 1});
        RECT hr = {card.left, y, card.right, y + headH};
        g.hits.push_back({hr, HT::SetFolderHead, fi, fi, 0, ni, 1});
        int by = y + (headH - btn) / 2, bx = card.right - S(4) - 4 * btn;
        g.hits.push_back({{bx - btn - S(4), by, bx - S(4), by + btn}, HT::SetFolderGame, fi, fi, fi, nf, 1});  // Spielmodus
        g.hits.push_back({{bx, by, bx + btn, by + btn}, HT::SetFolderUp, fi, fi, fi, nf, 1});
        g.hits.push_back({{bx + btn, by, bx + 2 * btn, by + btn}, HT::SetFolderDown, fi, fi, fi, nf, 1});
        g.hits.push_back({{bx + 2 * btn, by, bx + 3 * btn, by + btn}, HT::SetFolderDelete, fi, fi, fi, nf, 1});
        g.hits.push_back({{bx + 3 * btn, by, bx + 4 * btn, by + btn}, HT::SetFolderMenu, fi, fi, fi, nf, 1});
        int ry = y + headH;
        if (!ni) g.hits.push_back({{card.left + S(6), ry, card.right - S(6), ry + S(34)}, HT::SetDrop, fi, fi, 0, 0, 1});
        for (int p = 0; p < ni; p++, ry += rowH)
            g.hits.push_back({{card.left + S(4), ry, card.right - S(4), ry + rowH}, HT::SetAppRow, items[p], fi, p, ni, 1});
        y += ch + S(8);
    }
    g.contentH = y + g.scroll - g.lTop;
    g.hits.push_back({{L.left + S(10), L.bottom - S(48), L.right - S(10), L.bottom - S(10)}, HT::NewFolder, 0});

    // rechts
    g.hits.push_back({{R.left + S(10), R.top + S(44), R.right - S(10), R.top + S(80)}, HT::SearchR, 0});
    g.rTop = R.top + S(90);
    g.rBot = R.bottom - S(8);
    UpdateAllList();
    int arH = S(44);
    y = g.rTop - g.scrollR;
    for (int a : g.allList) {
        RECT rr = {R.left + S(6), y, R.right - S(6), y + arH};
        g.hits.push_back({rr, HT::AllAppRow, a, -1, -1, 0, 2});
        int b = S(30), bby = y + (arH - b) / 2;
        g.hits.push_back({{rr.right - S(4) - b, bby, rr.right - S(4), bby + b}, HT::AllAppAdd, a, -1, -1, 0, 2});
        y += arH;
    }
    g.contentHR = y + g.scrollR - g.rTop;
}

// ---------------------------------------------------------------- Musik: Daten für die Anzeige

// Schieberegler 0–100 -> Verstärkung 0–1; das Ohr hört logarithmisch, daher kubisch
static double VolumeGain(int v) {
    double x = std::max(0, std::min(100, v)) / 100.0;
    return x * x * x;
}

// Favoriten und zuletzt gespielte Songs (in der Ini, IDs mit | getrennt)
static std::wstring JoinIds(const std::vector<std::wstring>& v) {
    std::wstring s;
    for (auto& id : v) s += (s.empty() ? L"" : L"|") + id;
    return s;
}
static void LoadFavRecent() {
    g.favIds = SplitList(IniGetLong(L"Musik", L"Favoriten"));
    g.recentIds = SplitList(IniGetLong(L"Musik", L"ZuletztGespielt"));
    Music_SetFavorites(g.favIds);
}
static bool IsFav(const std::wstring& id) { return std::find(g.favIds.begin(), g.favIds.end(), id) != g.favIds.end(); }
static void BuildFavRecent() {
    g.mFav.clear();
    g.mRecent.clear();
    for (auto& id : g.favIds) { int i = Music_Find(id); if (i >= 0) g.mFav.push_back(i); }
    for (auto& id : g.recentIds) { int i = Music_Find(id); if (i >= 0) g.mRecent.push_back(i); }
}
static void ToggleFav(const std::wstring& id) {
    if (id.empty()) return;
    if (IsFav(id)) g.favIds.erase(std::remove(g.favIds.begin(), g.favIds.end(), id), g.favIds.end());
    else g.favIds.insert(g.favIds.begin(), id);  // neueste zuerst
    IniSet(L"Musik", L"Favoriten", JoinIds(g.favIds).c_str());
    BuildFavRecent();
    Music_SetFavorites(g.favIds);
    if (g.syncOn) Music_ExportLibrary();  // Musik-Sync: Favoriten in die Bibliotheksdatei
}
static void AddRecentSong(const std::wstring& id) {
    if (id.empty()) return;
    g.recentIds.erase(std::remove(g.recentIds.begin(), g.recentIds.end(), id), g.recentIds.end());
    g.recentIds.insert(g.recentIds.begin(), id);
    if (g.recentIds.size() > 40) g.recentIds.resize(40);
    IniSet(L"Musik", L"ZuletztGespielt", JoinIds(g.recentIds).c_str());
    BuildFavRecent();
}

static void BuildMusic() {
    auto& songs = Music_Songs();
    g.mAll.clear();
    for (int i = 0; i < (int)songs.size(); i++) g.mAll.push_back(i);
    std::sort(g.mAll.begin(), g.mAll.end(), [&](int a, int b) {
        if (g.sortPlays && songs[a].plays != songs[b].plays) return songs[a].plays > songs[b].plays;
        return lstrcmpiW(songs[a].title.c_str(), songs[b].title.c_str()) < 0;
    });
    BuildFavRecent();
    g.plItems.clear();
    for (auto& p : Music_Playlists()) {
        std::vector<int> v;
        for (auto& id : p.ids) {
            int i = Music_Find(id);
            if (i >= 0) v.push_back(i);
        }
        g.plItems.push_back(v);
    }
}

// Listen: >= 0 Playlist, -1 alle Songs, -2 Favoriten, -3 zuletzt gespielt
static const std::vector<int>& MusicListItems(int pl) {
    if (pl == -2) return g.mFav;
    if (pl == -3) return g.mRecent;
    return pl < 0 || pl >= (int)g.plItems.size() ? g.mAll : g.plItems[pl];
}
static std::wstring PlaylistName(int pl) {
    if (pl == -2) return L"Favoriten";
    if (pl == -3) return L"Zuletzt gespielt";
    return pl < 0 || pl >= (int)Music_Playlists().size() ? L"Alle Songs" : Music_Playlists()[pl].name;
}
static std::vector<std::wstring> IdsOf(const std::vector<int>& v) {
    std::vector<std::wstring> ids;
    for (int i : v) ids.push_back(Music_Songs()[i].id);
    return ids;
}
static std::wstring FmtTime(double s) {
    int t = (int)(s + 0.5);
    wchar_t b[16];
    wsprintfW(b, L"%d:%02d", t / 60, t % 60);
    return b;
}
static std::wstring FmtMs(double s) {  // m:ss.mmm
    int ms = (int)(std::max(0.0, s) * 1000 + 0.5);
    wchar_t b[24];
    wsprintfW(b, L"%d:%02d.%03d", ms / 60000, ms / 1000 % 60, ms % 1000);
    return b;
}
static bool MusicView() { return g.view == View::Music || g.view == View::MusicList || g.view == View::SongEdit || g.view == View::MusicSettings; }
static bool HasMusicBar() { return g.musicOn && g.view != View::Settings && g.view != View::SongEdit && g.view != View::MusicSettings && g.view != View::NowPlaying; }

// "Läuft gerade": großes Cover oben mittig, Titel, Fortschritt, Steuerung zentriert, darunter der Songtext
static int sNpL = 0, sNpR = 0;  // Textbreite für Titel/Künstler
static void LayoutNowPlaying(int W, int H, int pad) {
    int cw = W - 2 * pad, cx = pad + cw / 2;
    sNpL = pad + S(10);
    sNpR = W - pad - S(10);
    size_t first = g.hits.size();
    int top = S(64);
    int cs = std::min({cw - S(60), H * 36 / 100, S(400)});
    cs = std::max(cs, S(120));
    g.hits.push_back({{cx - cs / 2, top, cx + cs / 2, top + cs}, HT::NpCover, 0});
    int y = top + cs + S(100);  // Titel + Künstler, dann etwas Luft bis zum Fortschritt
    int tl = pad + S(24), tr = W - pad - S(24);
    if (g.pst.active) g.hits.push_back({{tl, y - S(8), tr, y + S(14)}, HT::MProgress, 0});
    y += S(44);  // unter Fortschritt + Zeiten
    int b = S(60), bs = S(44);
    g.hits.push_back({{cx - b / 2, y, cx + b / 2, y + b}, HT::MediaPlay, 0});
    g.hits.push_back({{cx - b / 2 - S(20) - bs, y + (b - bs) / 2, cx - b / 2 - S(20), y + (b + bs) / 2}, HT::MediaPrev, 0});
    g.hits.push_back({{cx + b / 2 + S(20), y + (b - bs) / 2, cx + b / 2 + S(20) + bs, y + (b + bs) / 2}, HT::MediaNext, 0});
    // links Lautstärke (klappt beim Darüberfahren aus), rechts Zufall / Wiederholen
    int vy = y + (b - S(26)) / 2, vh = S(26), vi = S(30), vw = (int)(S(80) * g.volExp);
    g.hits.push_back({{tl - S(4), vy, tl - S(4) + vi, vy + vh}, HT::VolIcon, 0});
    if (vw > S(12)) g.hits.push_back({{tl - S(4) + vi, vy, tl - S(4) + vi + vw, vy + vh}, HT::BarVol, 3});
    g.hits.push_back({{tr + S(4) - S(40), y + (b - S(40)) / 2, tr + S(4), y + (b + S(40)) / 2}, HT::PlayMode, 0});
    if (g.pst.active)  // Herz direkt neben Zufall / Wiederholen
        g.hits.push_back({{tr + S(4) - S(86), y + (b - S(40)) / 2, tr + S(4) - S(46), y + (b + S(40)) / 2}, HT::FavBtn, 0});
    y += b + S(16);
    if (H - S(18) - y > S(70)) g.hits.push_back({{pad, y, W - pad, H - S(18)}, HT::NpLyrics, 0});
    g.contentH = 0;
    if (g.npDrop > 0)  // beim Einklappen alles nach unten schieben
        for (size_t i = first; i < g.hits.size(); i++) OffsetRect(&g.hits[i].rc, 0, (int)g.npDrop);
    // Songtext zum laufenden Song holen (lokal, sonst online)
    if (g.pst.active && g.pst.songId != g.lyrId) {
        g.lyrId = g.pst.songId;
        g.lyr.clear();
        g.lyrH.clear();
        g.lyrSynced = false;
        g.lyrState = 1;
        g.lyrScroll = g.lyrTarget = 0;
        g.lyrManual = false;
        Music_LoadLyrics(g.lyrId, true);
    }
}

// aktuelle Zeile des Songtexts (-1 vor der ersten)
static int LyricIndex(double pos) {
    if (!g.lyrSynced) return -1;
    int k = -1;
    for (int i = 0; i < (int)g.lyr.size(); i++)
        if (g.lyr[i].t <= pos + 0.15) k = i;
        else break;
    return k;
}

// Musik-Startseite: Lautstärke, Downloads, dann "Alle Songs" und die Playlists als Alben (großes Cover links, Name rechts)
static void LayoutMusic(int W, int pad, int y0) {
    int cw = W - 2 * pad;
    int y = y0;
    g.jobsView = Music_Jobs();  // (Lautstärke sitzt jetzt unten in der Musikleiste)
    for (int j = 0; j < (int)g.jobsView.size(); j++) {
        g.hits.push_back({{pad, y, pad + cw, y + S(52)}, HT::MusicJob, j});
        y += S(58);
    }
    if (Music_Songs().empty() && g.jobsView.empty()) {
        g.hits.push_back({{pad, y, pad + cw, y + S(110)}, HT::MusicEmpty, 0});
        y += S(122);
    }
    // feste Reihe oben (scrollt nicht mit): Favoriten | Playlists | Zuletzt gespielt
    {
        int gp = S(10), tw = (cw - 2 * gp) / 3, ty = S(106), th = S(58);
        for (int k = 0; k < 3; k++) g.hits.push_back({{pad + k * (tw + gp), ty, pad + k * (tw + gp) + tw, ty + th}, HT::LibTab, k + 1});
    }
    if (g.libTab == 2) {  // Playlists als Alben
        static const int albumH[] = {72, 88, 108};
        int rowH = S(albumH[g.musicSize]);
        int npl = (int)Music_Playlists().size();
        for (int pl = 0; pl < npl; pl++) {
            g.hits.push_back({{pad, y, pad + cw, y + rowH}, HT::MAlbum, pl});
            y += rowH + S(8);
        }
        if (!npl) {
            g.hits.push_back({{pad, y, pad + cw, y + S(40)}, HT::EditInfo, 10});
            y += S(48);
        }
    } else {  // Songliste: alle (Standard), Favoriten oder zuletzt gespielt
        static const int songH[] = {54, 64, 80};
        int rowH = S(songH[g.musicSize]);
        int list = g.libTab == 1 ? -2 : g.libTab == 3 ? -3 : -1;
        const auto& items = MusicListItems(list);
        if (list == -1 && items.empty()) {  // leere Bibliothek: trotzdem neu einlesen können
            g.hits.push_back({{pad + cw - S(44), y, pad + cw, y + S(44)}, HT::MReload, 0});
            y += S(54);
        }
        if (!items.empty()) {
            int sortW = list == -1 ? S(156) : 0;  // Sortierung nur bei "Alle Songs"
            int reloadW = sortW ? S(52) : 0;  // Knopf zum Neueinlesen ganz rechts
            g.hits.push_back({{pad, y, pad + cw - sortW - reloadW - (sortW ? S(8) : 0), y + S(44)}, HT::MPlayAll, list});
            if (sortW) {
                g.hits.push_back({{pad + cw - sortW - reloadW, y, pad + cw - reloadW, y + S(44)}, HT::MSort, 0});
                g.hits.push_back({{pad + cw - S(44), y, pad + cw, y + S(44)}, HT::MReload, 0});
            }
            y += S(54);
        } else if (!Music_Songs().empty()) {
            g.hits.push_back({{pad, y, pad + cw, y + S(40)}, HT::EditInfo, g.libTab == 1 ? 11 : 12});
            y += S(48);
        }
        for (int p = 0; p < (int)items.size(); p++) {
            g.hits.push_back({{pad, y, pad + cw, y + rowH}, HT::MRow, items[p], list, p});
            y += rowH + S(4);
        }
    }
    g.contentH = y - y0;
}

// Songs einer Playlist (oder alle): gleiches Format wie die Alben, Cover etwas kleiner
static void LayoutMusicList(int W, int pad, int y0) {
    static const int songH[] = {54, 64, 80};
    int cw = W - 2 * pad, y = y0, rowH = S(songH[g.musicSize]);
    const auto& items = MusicListItems(g.curPlaylist);
    if (!items.empty()) {
        g.hits.push_back({{pad, y, pad + cw, y + S(44)}, HT::MPlayAll, g.curPlaylist});
        y += S(54);
    }
    for (int p = 0; p < (int)items.size(); p++) {
        g.hits.push_back({{pad, y, pad + cw, y + rowH}, HT::MRow, items[p], g.curPlaylist, p});
        y += rowH + S(4);
    }
    g.contentH = y - y0;
}
// Song bearbeiten: Cover (lokal oder aus dem Web), Titel, Künstler
static void LayoutSongEdit(int W, int pad, int y0) {
    int cw = W - 2 * pad, y = y0 + S(4), big = S(150);
    g.hits.push_back({{pad + (cw - big) / 2, y, pad + (cw + big) / 2, y + big}, HT::EditCover, 0});
    y += big + S(14);
    int bw = (cw - S(10)) / 2;
    g.hits.push_back({{pad, y, pad + bw, y + S(36)}, HT::EditBtn, 0});
    g.hits.push_back({{pad + bw + S(10), y, pad + cw, y + S(36)}, HT::EditBtn, 1});
    y += S(46);
    if (g.coverSearching) {
        g.hits.push_back({{pad, y, pad + cw, y + S(30)}, HT::EditInfo, 0});
        y += S(38);
    } else if (g.coverNone) {
        g.hits.push_back({{pad, y, pad + cw, y + S(56)}, HT::EditInfo, 13});
        y += S(64);
    } else if (!g.coverResults.empty()) {
        int c = 4, gp = S(8), sz = (cw - 3 * gp) / 4;
        for (int k = 0; k < (int)g.coverResults.size(); k++) {
            int x = pad + (k % c) * (sz + gp), yy = y + (k / c) * (sz + gp);
            g.hits.push_back({{x, yy, x + sz, yy + sz}, HT::CoverPick, k});
        }
        y += (int)((g.coverResults.size() + c - 1) / c) * (sz + gp) + S(4);
    }
    g.hits.push_back({{pad, y, pad + cw, y + S(22)}, HT::EditLabel, 0});
    y += S(24);
    g.hits.push_back({{pad, y, pad + cw, y + S(38)}, HT::EditField, 1});
    y += S(48);
    g.hits.push_back({{pad, y, pad + cw, y + S(22)}, HT::EditLabel, 1});
    y += S(24);
    g.hits.push_back({{pad, y, pad + cw, y + S(38)}, HT::EditField, 2});
    y += S(52);
    if (g.trimLen > 1) {  // Schneiden
        g.hits.push_back({{pad, y, pad + cw, y + S(22)}, HT::EditLabel, 2});
        y += S(26);
        g.hits.push_back({{pad, y, pad + cw, y + S(64)}, HT::SetSlider, 10});
        y += S(68);
        g.hits.push_back({{pad, y, pad + cw, y + S(20)}, HT::EditLabel, 3});  // Millisekunden beider Linien
        y += S(28);
        int tw = (cw - 2 * S(10)) / 3;
        g.hits.push_back({{pad, y, pad + tw, y + S(36)}, HT::EditBtn, 4});
        g.hits.push_back({{pad + tw + S(10), y, pad + 2 * tw + S(10), y + S(36)}, HT::EditBtn, 5});
        g.hits.push_back({{pad + 2 * tw + S(20), y, pad + cw, y + S(36)}, HT::EditBtn, 8});
        y += S(44);
        g.hits.push_back({{pad, y, pad + bw, y + S(36)}, HT::EditBtn, 6});
        g.hits.push_back({{pad + bw + S(10), y, pad + cw, y + S(36)}, HT::EditBtn, 7});
        y += S(52);
    }
    g.hits.push_back({{pad, y, pad + bw, y + S(38)}, HT::EditBtn, 2});
    g.hits.push_back({{pad + bw + S(10), y, pad + cw, y + S(38)}, HT::EditBtn, 3});
    y += S(48);
    g.contentH = y - y0;
}

// Einstellungen > Musik: links Playlists (umsortieren per Ziehen), rechts alle Songs mit "+" und Bearbeiten
static void LayoutMusicSettings(int W, int H, int pad) {
    int top = ContentTop(), gapP = S(12);
    g.paneMid = W / 2;
    RECT L = {pad, top, g.paneMid - gapP / 2, H - S(18)};
    RECT R = {g.paneMid + gapP / 2, top, W - pad, H - S(18)};
    g.hits.push_back({L, HT::Pane, 0});
    g.hits.push_back({R, HT::Pane, 1});
    g.hits.push_back({{L.left, L.top, L.right, L.top + S(44)}, HT::PaneTitle, 2});
    g.hits.push_back({{R.left, R.top, R.right, R.top + S(44)}, HT::PaneTitle, 3});

    g.lTop = L.top + S(44);
    g.lBot = L.bottom - S(58);
    int ip = S(8), headH = S(38), rowH = S(42), btn = S(28);
    int y = g.lTop - g.scroll;
    int npl = (int)Music_Playlists().size();
    if (!npl) {
        g.hits.push_back({{L.left + ip, y, L.right - ip, y + S(70)}, HT::EditInfo, 1, -1, 0, 0, 1});
        y += S(80);
    }
    for (int pl = 0; pl < npl; pl++) {
        const auto& items = g.plItems[pl];
        int ni = (int)items.size();
        int ch = headH + (ni ? ni * rowH : S(40)) + S(6);
        RECT card = {L.left + ip, y, L.right - ip, y + ch};
        g.hits.push_back({card, HT::SetCard, pl, pl, 0, 0, 1});
        RECT hr = {card.left, y, card.right, y + headH};
        g.hits.push_back({hr, HT::PlHead, pl, pl, 0, ni, 1});
        int by = y + (headH - btn) / 2, bx = card.right - S(4) - 3 * btn;
        g.hits.push_back({{bx, by, bx + btn, by + btn}, HT::PlUp, pl, pl, pl, npl, 1});
        g.hits.push_back({{bx + btn, by, bx + 2 * btn, by + btn}, HT::PlDown, pl, pl, pl, npl, 1});
        g.hits.push_back({{bx + 2 * btn, by, bx + 3 * btn, by + btn}, HT::PlDelete, pl, pl, pl, npl, 1});
        int ry = y + headH;
        if (!ni) g.hits.push_back({{card.left + S(6), ry, card.right - S(6), ry + S(34)}, HT::PlDrop, pl, pl, 0, 0, 1});
        for (int p = 0; p < ni; p++, ry += rowH)
            g.hits.push_back({{card.left + S(4), ry, card.right - S(4), ry + rowH}, HT::PlSongRow, items[p], pl, p, ni, 1});
        y += ch + S(8);
    }
    g.contentH = y + g.scroll - g.lTop;
    g.hits.push_back({{L.left + S(10), L.bottom - S(48), L.right - S(10), L.bottom - S(10)}, HT::NewFolder, 1});

    g.hits.push_back({{R.left + S(10), R.top + S(44), R.right - S(10), R.top + S(80)}, HT::SearchR, 1});
    g.rTop = R.top + S(90);
    g.rBot = R.bottom - S(8);
    std::wstring q = Lower(g.creatingFolder ? L"" : g.query);
    int arH = S(48);
    y = g.rTop - g.scrollR;
    for (int i : g.mAll) {
        const Song& s = Music_Songs()[i];
        if (!q.empty() && Lower(s.title + L" " + s.artist).find(q) == std::wstring::npos) continue;
        RECT rr = {R.left + S(6), y, R.right - S(6), y + arH};
        g.hits.push_back({rr, HT::AllSongRow, i, -1, -1, 0, 2});
        int b = S(30), bby = y + (arH - b) / 2;
        g.hits.push_back({{rr.right - S(4) - 2 * b, bby, rr.right - S(4) - b, bby + b}, HT::AllSongEdit, i, -1, -1, 0, 2});
        g.hits.push_back({{rr.right - S(4) - b, bby, rr.right - S(4), bby + b}, HT::AllSongAdd, i, -1, -1, 0, 2});
        y += arH;
    }
    g.contentHR = y + g.scrollR - g.rTop;
}

// Ordneransicht: ein Ordner pro Reihe, bis zu zwei Reihen Apps direkt anklickbar
// ---------------------------------------------------------------- Timer (Erweiterung)
static int WheelMax(int i) { return i == 0 ? 24 : 60; }  // Stunden, Minuten, Sekunden
static int WheelValue(int i) {
    int n = WheelMax(i), v = (int)std::lround(g.wheel[i]) % n;
    return v < 0 ? v + n : v;
}
static void SnapWheel(int i) {
    g.wheel[i] = WheelValue(i);
    IniSet(L"Timer", i == 0 ? L"Stunden" : i == 1 ? L"Minuten" : L"Sekunden", std::to_wstring((int)g.wheel[i]).c_str());
}
static long long TimerRemaining() {  // Millisekunden
    return g.timerEnd ? std::max(0LL, (long long)(g.timerEnd - GetTickCount64())) : g.timerLeft;
}
static void StopRinging() {
    g.timerRinging = false;
    PlaySoundW(nullptr, nullptr, 0);
    mciSendStringW(L"close hbring", nullptr, 0, nullptr);
    KillTimer(g.hwnd, T_COUNT);
}
// eigener Klingelton (MP3, WAV, M4A) in Schleife; geht er nicht, der Wecker-Ton von Windows
static void StartRinging() {
    g.timerRinging = true;
    g.ringSince = GetTickCount64();
    if (!g.ringFile.empty()) {
        std::wstring open = L"open \"" + g.ringFile + L"\" type mpegvideo alias hbring";
        if (!mciSendStringW(open.c_str(), nullptr, 0, nullptr) && !mciSendStringW(L"play hbring repeat", nullptr, 0, nullptr)) return;
        mciSendStringW(L"close hbring", nullptr, 0, nullptr);
    }
    wchar_t win[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    std::wstring wav = std::wstring(win) + L"\\Media\\Alarm01.wav";
    if (!PlaySoundW(wav.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_LOOP | SND_NODEFAULT))
        PlaySoundW((LPCWSTR)SND_ALIAS_SYSTEMEXCLAMATION, nullptr, SND_ALIAS_ID | SND_ASYNC | SND_LOOP);
}
static void PickRingtone() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC spec[] = {{L"Audio (MP3, WAV, M4A)", L"*.mp3;*.wav;*.m4a"}};
    dlg->SetFileTypes(1, spec);
    dlg->SetTitle(L"Klingelton für den Timer wählen");
    g.menuOpen = true;
    HRESULT hr = dlg->Show(g.hwnd);
    g.menuOpen = false;
    SetForegroundWindow(g.hwnd);
    IShellItem* it = nullptr;
    if (SUCCEEDED(hr) && SUCCEEDED(dlg->GetResult(&it))) {
        PWSTR p = nullptr;
        if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
            g.ringFile = p;
            IniSet(L"Timer", L"Klingelton", g.ringFile.c_str());
            CoTaskMemFree(p);
        }
        it->Release();
    }
    dlg->Release();
}
static void LayoutTimer(int W, int pad) {
    int cw = W - 2 * pad, y = ContentTop() + S(4), rowH = S(44), bh = S(44), half = (cw - S(10)) / 2;
    bool idle = !g.timerEnd && !g.timerLeft && !g.timerRinging;
    if (idle) {
        int ww = std::min(S(110), cw / 3), x = pad + (cw - 3 * ww) / 2;
        for (int i = 0; i < 3; i++) g.hits.push_back({{x + i * ww, y, x + (i + 1) * ww, y + 5 * rowH}, HT::TimerWheel, i});
    } else {
        g.hits.push_back({{pad, y, pad + cw, y + 5 * rowH}, HT::TimerShow, 0});
    }
    y += 5 * rowH + S(30);
    if (idle || g.timerRinging) {
        g.hits.push_back({{pad, y, pad + cw, y + bh}, HT::TimerBtn, 0});  // Start / Stopp
    } else {
        g.hits.push_back({{pad, y, pad + half, y + bh}, HT::TimerBtn, 1});       // Abbrechen
        g.hits.push_back({{pad + cw - half, y, pad + cw, y + bh}, HT::TimerBtn, 0});  // Pause / Weiter
    }
    y += bh + S(12);
    g.hits.push_back({{pad, y, g.ringFile.empty() ? pad + cw : pad + half, y + bh}, HT::TimerBtn, 2});  // Klingelton
    if (!g.ringFile.empty()) g.hits.push_back({{pad + cw - half, y, pad + cw, y + bh}, HT::TimerBtn, 3});  // Standardton
    g.contentH = 0;
}
static void TimerButton(int i) {
    if (i == 2) PickRingtone();
    else if (i == 3) {
        g.ringFile.clear();
        IniSet(L"Timer", L"Klingelton", nullptr);
    } else if (g.timerRinging) {
        StopRinging();
    } else if (i == 1) {  // Abbrechen
        g.timerEnd = 0;
        g.timerLeft = 0;
        KillTimer(g.hwnd, T_COUNT);
    } else if (g.timerEnd) {  // Pause
        g.timerLeft = TimerRemaining();
        g.timerEnd = 0;
        KillTimer(g.hwnd, T_COUNT);
    } else {  // Start / Weiter
        long long ms = g.timerLeft ? g.timerLeft : (WheelValue(0) * 3600LL + WheelValue(1) * 60 + WheelValue(2)) * 1000;
        if (ms <= 0) { MessageBeep(MB_ICONHAND); return; }
        g.timerLeft = 0;
        g.timerEnd = GetTickCount64() + ms;
        SetTimer(g.hwnd, T_COUNT, 200, nullptr);  // läuft auch bei geschlossener Leiste weiter
    }
}

static void Layout() {
    g.hits.clear();
    int W = g.w + RailW(), H = g.h, pad = S(18) + RailW();  // links Platz für die Navigationsleiste
    int top = ContentTop();
    int y0 = top - g.scroll;

    if (g.view == View::Folders) {
        int gap = S(12), ip = S(14), headH = S(30);
        int cw = W - 2 * pad;
        int cols = std::max(3, (cw - 2 * ip) / S(86));
        int cellW = (cw - 2 * ip) / cols, cellH = S(86);
        int y = y0;
        if (g.showRecent) {  // "Zuletzt genutzt": eine Reihe, erst nach starkem Hochscrollen sichtbar
            int rh = S(34) + cellH + S(8);
            g.hits.push_back({{pad, y, pad + cw, y + rh}, HT::RecentCard, 0});
            int n = std::min((int)g.recent.size(), cols);
            int rw = (cw - 2 * ip) / std::max(1, cols);
            for (int k = 0; k < n; k++) {
                int cx = pad + ip + k * rw;
                g.hits.push_back({{cx, y + S(32), cx + rw, y + S(32) + cellH}, HT::RecentItem, k});
            }
            y += rh + gap;
        }
        {
            int fh = S(12) + cellH + S(8);
            g.hits.push_back({{pad, y, pad + cw, y + fh}, HT::SetCard, -5, -5});
            int fw = (cw - 2 * ip) / 4;
            for (int k = 0; k < 4; k++) {
                int cx = pad + ip + k * fw;
                int fa = g.favs[k] < (int)g.apps.size() ? g.favs[k] : -1;
                g.hits.push_back({{cx, y + S(10), cx + fw, y + S(10) + cellH}, HT::Fav, fa, -1, k});
            }
            y += fh + S(8);
        }
        {   // Favoriten-Links: dünne Reihe kleiner Kacheln (Website-Symbol + Name), am Ende "+"
            int lh = S(30), x = pad, plusW = S(34), gp = S(6);
            HDC sdc = GetDC(nullptr);
            HGDIOBJ of = SelectObject(sdc, g.fSmall);
            for (int k = 0; k < (int)g.links.size(); k++) {
                SIZE ext = {};
                GetTextExtentPoint32W(sdc, g.links[k].name.c_str(), (int)g.links[k].name.size(), &ext);
                int w = S(10) + S(16) + S(6) + std::min((int)ext.cx, S(120)) + S(10);
                if (x + w > pad + cw - plusW - gp) break;  // passt nicht mehr in die Reihe
                g.hits.push_back({{x, y, x + w, y + lh}, HT::LinkChip, k});
                x += w + gp;
            }
            SelectObject(sdc, of);
            ReleaseDC(nullptr, sdc);
            g.hits.push_back({{x, y, x + plusW, y + lh}, HT::LinkAdd, 0});
            y += lh + gap;
        }
        for (int i = 0; i < (int)g.folders.size(); i++) {
            auto& items = g.folders[i].items;
            int n = (int)items.size();
            int cap = cols * 2;
            bool more = n > cap;
            int shown = more ? cap - 1 : n;
            g.folders[i].shown = shown;
            int slots = shown + (more ? 1 : 0);
            int rows = (slots + cols - 1) / cols;
            int ch = ip + headH + rows * cellH + S(6);
            g.hits.push_back({{pad, y, pad + cw, y + ch}, HT::Card, i});
            for (int k = 0; k < slots; k++) {
                int cx = pad + ip + (k % cols) * cellW, cy = y + ip + headH + (k / cols) * cellH;
                RECT r = {cx, cy, cx + cellW, cy + cellH};
                if (k < shown) g.hits.push_back({r, HT::Slot, items[k]});
                else g.hits.push_back({r, HT::Mini, i});
            }
            y += ch + gap;
        }
        g.contentH = y - y0;
    } else if (g.view == View::Settings) {
        int tw = (W - 2 * pad) / 4;
        for (int k = 0; k < 4; k++) g.hits.push_back({{pad + k * tw, S(58), k == 3 ? W - pad : pad + (k + 1) * tw, S(94)}, HT::Tab, k});
        if (g.setTab == 0) LayoutDesign(W, pad, y0);
        else if (g.setTab == 2) LayoutFunctions(W, pad, y0);
        else if (g.setTab == 3) LayoutSuggested(W, pad, y0);
        else LayoutFolders(W, H, pad);
    } else if (g.view == View::MusicSettings) {
        LayoutMusicSettings(W, H, pad);
    } else if (g.view == View::NowPlaying) {
        LayoutNowPlaying(W, H, pad);
    } else if (g.view == View::Music) {
        LayoutMusic(W, pad, y0);
    } else if (g.view == View::MusicList) {
        LayoutMusicList(W, pad, y0);
    } else if (g.view == View::SongEdit) {
        LayoutSongEdit(W, pad, y0);
    } else if (g.view == View::Timer) {
        LayoutTimer(W, pad);
    } else if (g.view == View::Chat) {
        g.hits.push_back({{W - pad - S(130), S(60), W - pad, S(92)}, HT::ChatNew, 0});
        g.chatRect = {pad, S(102), W - pad, ContentBottom()};
        g.contentH = 0;
    } else if (g.view == View::Storage) {
        int cw = W - 2 * pad, y = y0, rh = S(76);
        g.hits.push_back({{W - pad - S(150), S(60), W - pad, S(92)}, HT::StorageRefresh, 0});
        DWORD mask = GetLogicalDrives();
        for (int d = 0; d < 26; d++) {
            if (!(mask & (1u << d))) continue;
            wchar_t root[4] = {(wchar_t)(L'A' + d), L':', L'\\', 0};
            if (GetDriveTypeW(root) != DRIVE_FIXED) continue;  // nur eingebaute Laufwerke
            g.hits.push_back({{pad, y, pad + cw, y + rh}, HT::DriveRow, d});
            g.hits.push_back({{pad + cw - S(16) - S(150), y + (rh - S(34)) / 2, pad + cw - S(16), y + (rh + S(34)) / 2}, HT::DriveOpen, d});
            y += rh + S(8);
        }
        y += S(6);
        g.hits.push_back({{pad, y, pad + cw, y + S(42)}, HT::StorageApps, 0});
        y += S(52);
        g.contentH = y - y0;
    } else if (g.view == View::Calendar) {
        // oben: Monat + Pfeile + "Heute"; darunter Wochentage, 6 Wochen; darunter die Termine des gewählten Tags
        if (!g.calY) { int t = Today(); g.calY = t / 10000; g.calM = t / 100 % 100; g.calSel = t; }
        if (!g.eventsLoaded) LoadEvents();
        int cw = W - 2 * pad, b = S(34);
        g.hits.push_back({{W - pad - 2 * b - S(80) - S(8), S(60), W - pad - 2 * b - S(8), S(92)}, HT::CalNav, 2});  // Heute
        g.hits.push_back({{W - pad - 2 * b, S(60), W - pad - b, S(92)}, HT::CalNav, 0});                           // zurück
        g.hits.push_back({{W - pad - b, S(60), W - pad, S(92)}, HT::CalNav, 1});                                   // weiter
        int y = y0;
        g.hits.push_back({{pad, y, pad + cw, y + S(24)}, HT::CalHead, 0});
        y += S(26);
        int colW = cw / 7, cellH = S(42);
        int first = WeekdayMon0(g.calY, g.calM, 1), days = DaysIn(g.calY, g.calM);
        int py = g.calM == 1 ? g.calY - 1 : g.calY, pm = g.calM == 1 ? 12 : g.calM - 1, pdays = DaysIn(py, pm);
        for (int cell = 0; cell < 42; cell++) {
            int day = cell - first + 1, dy = g.calY, dm = g.calM, other = 0;
            if (day < 1) { dy = py; dm = pm; day += pdays; other = 1; }
            else if (day > days) { day -= days; dm = g.calM == 12 ? 1 : g.calM + 1; dy = g.calM == 12 ? g.calY + 1 : g.calY; other = 1; }
            int cx = pad + (cell % 7) * colW, cy = y + (cell / 7) * cellH;
            g.hits.push_back({{cx, cy, cx + colW, cy + cellH}, HT::CalDay, dy * 10000 + dm * 100 + day, other});
        }
        y += 6 * cellH + S(14);
        g.hits.push_back({{pad, y, pad + cw, y + S(28)}, HT::CalInfo, g.calSel});
        y += S(32);
        for (int i : EventsOn(g.calSel)) {
            g.hits.push_back({{pad, y, pad + cw, y + S(42)}, HT::CalEvent, i});
            y += S(46);
        }
        g.hits.push_back({{pad, y, pad + cw, y + S(38)}, HT::CalAdd, 0});
        y += S(48);
        g.contentH = y - y0;
    } else if (g.view == View::AllApps) {
        // alle Apps mit Größe: oben Sortierung, darunter Zeilen
        int sw = S(76), sx = W - pad - 3 * sw;
        for (int k = 0; k < 3; k++) g.hits.push_back({{sx + k * sw, S(60), sx + (k + 1) * sw, S(92)}, HT::SortBtn, k});
        int y = y0, rowH = S(46);
        for (auto& e : g.appSizes) {
            bool run = g.running.count(e.first) > 0;
            if (g.onlyRunning && !run) continue;
            g.hits.push_back({{pad, y, W - pad, y + rowH}, HT::AppSizeRow, e.first});
            if (run && !g.appMatch[e.first].system)  // Beenden-Knopf ganz rechts
                g.hits.push_back({{W - pad - S(40), y + (rowH - S(32)) / 2, W - pad - S(8), y + (rowH + S(32)) / 2}, HT::KillBtn, e.first});
            y += rowH + S(2);
        }
        g.contentH = y - y0;
    } else if (g.view == View::Folder && FolderGameMode(g.folders[g.curFolder].name)) {
        // Spielmodus: Liste mit Größe, Spielzeit, zuletzt gespielt
        const auto& list = g.folders[g.curFolder].items;
        int y = y0, rowH = S(62);
        for (int i : list) {
            g.hits.push_back({{pad, y, W - pad, y + rowH}, HT::GameRow, i});
            y += rowH + S(2);
        }
        g.contentH = y - y0;
    } else {
        const std::vector<int>& list = g.view == View::Folder ? g.folders[g.curFolder].items : g.results;
        int cols = std::max(4, (W - 2 * pad) / S(96));
        int cellW = (W - 2 * pad) / cols, cellH = S(104);
        for (int i = 0; i < (int)list.size(); i++) {
            int x = pad + (i % cols) * cellW, y = y0 + (i / cols) * cellH;
            g.hits.push_back({{x, y, x + cellW, y + cellH}, HT::Cell, list[i]});
        }
        g.contentH = (int)((list.size() + cols - 1) / cols) * cellH;
    }

    // Musik-Leiste unten (nicht in den Einstellungen)
    if (HasMusicBar()) {
        RECT mb = {pad, H - S(18) - S(kBarH), W - pad, H - S(18)};  // unten: normaler Rand (pad enthält links die Navigationsleiste)
        g.hits.push_back({mb, HT::MusicBar, 0});
        // Aufbau: links großes Cover | daneben oben links der Bibliothek-Knopf, oben mittig Titel + Künstler |
        //        darunter breiter Fortschrittsbalken | unten mittig Zurück/Play/Weiter, unten links Lautstärke, unten rechts Zeit
        int textL = mb.left + S(14) + S(kBarArt) + S(16), areaR = mb.right - S(14), cx = (textL + areaR) / 2;
        g.hits.push_back({{mb.left + S(14), mb.top + S(14), mb.left + S(14) + S(kBarArt), mb.top + S(14) + S(kBarArt)}, HT::BarCover, 0});  // Cover -> "Läuft gerade"
        g.hits.push_back({{textL - S(6), mb.top + S(10), textL - S(6) + S(40), mb.top + S(50)}, HT::FavBtn, 0});  // Herz (Favorit)
        g.hits.push_back({{areaR + S(6) - S(40), mb.top + S(10), areaR + S(6), mb.top + S(50)}, HT::PlayMode, 0});  // Zufall / Wiederholen
        g.barTextRight = areaR - S(44);
        int b = S(kBarBtn), bs = S(38), by = mb.top + S(80);
        g.hits.push_back({{cx - b / 2, by, cx + b / 2, by + b}, HT::MediaPlay, 0});
        g.hits.push_back({{cx - b / 2 - S(14) - bs, by + (b - bs) / 2, cx - b / 2 - S(14), by + (b + bs) / 2}, HT::MediaPrev, 0});
        g.hits.push_back({{cx + b / 2 + S(14), by + (b - bs) / 2, cx + b / 2 + S(14) + bs, by + (b + bs) / 2}, HT::MediaNext, 0});
        if (g.pst.active)  // Fortschritt (zum Spulen anklickbar), volle Breite
            g.hits.push_back({{textL, mb.top + S(54), areaR, mb.top + S(76)}, HT::MProgress, 0});
        // Lautstärke unten links: eingeklappt nur der Lautsprecher, beim Darüberfahren klappt ein kurzer Regler aus
        int vy = mb.top + S(92), vh = S(22), vi = S(26), vw = (int)(S(80) * g.volExp);
        int vx = textL - S(5);
        g.hits.push_back({{vx, vy, vx + vi, vy + vh}, HT::VolIcon, 0});
        if (vw > S(12)) g.hits.push_back({{vx + vi, vy, vx + vi + vw, vy + vh}, HT::BarVol, 3});
    }
    // Navigationsleiste ganz links: Home (Apps), Musik, Kalender
    {
        // volle Höhe, die Symbole unten angeheftet (Home, Musik, Kalender von oben nach unten)
        int rx = S(10), rw = RailW() - S(14), bottom = H - S(18);
        g.hits.push_back({{rx, S(12), rx + rw, bottom}, HT::NavBar, 0});
        std::vector<int> pages = NavPages();
        int ny = bottom - S(6) - (int)pages.size() * S(50) + S(4);
        for (int k : pages) {
            g.hits.push_back({{rx + S(3), ny, rx + rw - S(3), ny + S(46)}, HT::Nav, k});
            ny += S(50);
        }
    }
    // Kopfzeile (liegt über allem)
    // links: Logo, Titel, direkt daneben das Zahnrad (Unterseiten: Zurück statt Logo, kein Zahnrad)
    if ((g.view == View::Folder || g.view == View::Settings || MusicView() || g.view == View::AllApps || g.view == View::NowPlaying) &&
        !(!g.appsOn && g.view == View::Music)) {
        g.hits.push_back({{pad - S(6), S(14), pad + S(30), S(50)}, HT::Back, 0});
    } else {
        g.hits.push_back({{pad - S(6), S(14), pad + S(30), S(50)}, HT::Logo, 0});
        HDC sdc = GetDC(nullptr);
        HGDIOBJ of = SelectObject(sdc, g.fTitle);
        std::wstring t = HeaderTitle();
        SIZE ext = {};
        GetTextExtentPoint32W(sdc, t.c_str(), (int)t.size(), &ext);
        SelectObject(sdc, of);
        ReleaseDC(nullptr, sdc);
        int gx = pad + S(36) + ext.cx + S(6);
        if (g.view == View::Folders) g.hits.push_back({{pad + S(30), S(15), gx - S(2), S(49)}, HT::TitleBtn, 0});  // "Apps" -> alle Apps mit Größe
        g.hits.push_back({{gx, S(15), gx + S(34), S(49)}, HT::Gear, 0});
    }
    // rechts, von links nach rechts: (Musik verwalten) | Bluetooth | WLAN | Akku | ···
    {
        int x = W - pad, b = S(34), gp = S(6);
        auto put = [&](HT t, int w) {
            g.hits.push_back({{x - w, S(14), x, S(48)}, t, 0});
            x -= w + gp;
        };
        put(HT::Menu, b);
        if (g.batt >= 0) put(HT::Battery, S(64));
        put(HT::Wifi, b);
        put(HT::Bluetooth, b);
        if (g.view == View::Music || g.view == View::MusicList) put(HT::MusicManage, b);  // Musik verwalten
        g.headerRight = x + gp;
    }
    if (g.view == View::MusicSettings) {  // Größe der Alben und Songs
        int sw = S(84), sx = W - pad - 3 * sw;
        for (int k = 0; k < 3; k++) g.hits.push_back({{sx + k * sw, S(60), sx + (k + 1) * sw, S(92)}, HT::MSize, k});
    }
}

static std::wstring HeaderTitle() {
    if (g.view == View::Folder && g.curFolder >= 0 && g.curFolder < (int)g.folders.size()) return g.folders[g.curFolder].name;
    if (g.view == View::Search) return L"Suche";
    if (g.view == View::Settings) return L"Einstellungen";
    if (g.view == View::Music) return L"Bibliothek";
    if (g.view == View::MusicList) return PlaylistName(g.curPlaylist);
    if (g.view == View::SongEdit) return L"Song bearbeiten";
    if (g.view == View::MusicSettings) return L"Musik verwalten";
    if (g.view == View::AllApps) return L"Alle Apps";
    if (g.view == View::Calendar) return L"Kalender";
    if (g.view == View::Storage) return L"Speicher";
    if (g.view == View::Chat) return L"Claude";
    if (g.view == View::Timer) return L"Timer";
    if (g.view == View::NowPlaying) return L"Läuft gerade";
    return L"Apps";
}
// aktuelle Seite der Navigationsleiste: 0 = Apps, 1 = Musik, 2 = Kalender
// Seiten der Navigationsleiste: Apps, (Musik), Kalender, (Speicher – nur mit WizTree)
static std::vector<int> NavPages() {
    std::vector<int> p;
    if (g.appsOn) p.push_back(0);
    if (g.musicOn) p.push_back(1);
    if (g.calendarOn) p.push_back(2);
    if (g.timerOn) p.push_back(5);  // Timer (Erweiterung)
    if (g.claudeOn) p.push_back(4);  // Claude-Chat (Erweiterung)
    if (g.appsOn && !WizTree_Exe().empty()) p.push_back(3);
    return p;
}
static int NavPage() {
    if (g.view == View::Storage) return 3;
    if (g.view == View::Chat) return 4;
    if (g.view == View::Timer) return 5;
    if (g.view == View::Calendar) return 2;
    if (g.view == View::Music || g.view == View::MusicList || g.view == View::SongEdit || g.view == View::MusicSettings || g.view == View::NowPlaying) return 1;
    return 0;
}
static bool IsHeaderHit(HT t) { return t == HT::Back || t == HT::NavBar || t == HT::Nav || t == HT::Menu || t == HT::Gear || t == HT::Tab || t == HT::MusicBar || t == HT::Bluetooth ||
                                       t == HT::Logo || t == HT::Wifi || t == HT::Battery || t == HT::BarVol || t == HT::VolIcon || t == HT::TitleBtn || t == HT::SortBtn || t == HT::CalNav || t == HT::StorageRefresh || t == HT::ChatNew ||
                                     t == HT::MediaPrev || t == HT::MediaPlay || t == HT::MediaNext || t == HT::MediaPhone ||
                                     t == HT::MusicOpen || t == HT::PlayMode || t == HT::MProgress || t == HT::MusicManage || t == HT::MSize ||
                                     t == HT::NpCover || t == HT::NpLyrics || t == HT::BarCover || t == HT::LibTab || t == HT::FavBtn; }
static int ContentBottom() { return !HasMusicBar() ? g.h : g.h - S(18) - S(kBarH) - S(8); }
static bool OrdnerPage() { return (g.view == View::Settings && g.setTab == 1) || g.view == View::MusicSettings; }  // zweigeteilte Seiten (Ordner, Musik)

static RECT PaneClip(int pane) {
    if (pane == 1) return {0, g.lTop, g.paneMid, g.lBot};
    if (pane == 2) return {g.paneMid, g.rTop, g.w, g.rBot};
    return {0, ContentTop(), g.w, ContentBottom()};
}

static int HitTest(POINT pt) {
    for (int i = (int)g.hits.size() - 1; i >= 0; i--) {
        auto& h = g.hits[i];
        if (!IsHeaderHit(h.type) && !(OrdnerPage() && h.pane == 0)) {
            RECT c = PaneClip(h.pane);
            if (!PtInRect(&c, pt)) continue;
        }
        if (PtInRect(&h.rc, pt)) return i;
    }
    return -1;
}

static void ClampScroll() {
    int maxL, maxR = 0;
    if (OrdnerPage()) {
        maxL = std::max(0, g.contentH + S(8) - (g.lBot - g.lTop));
        maxR = std::max(0, g.contentHR + S(8) - (g.rBot - g.rTop));
    } else {
        maxL = std::max(0, g.contentH + S(8) - (ContentBottom() - ContentTop()));
    }
    g.scroll = std::max(0, std::min(g.scroll, maxL));
    g.scrollR = std::max(0, std::min(g.scrollR, maxR));
    if (!g.scrollAnim) {
        g.scrollTo = g.scroll;
        g.scrollRTo = g.scrollR;
        g.scrollF = g.scroll;
        g.scrollRF = g.scrollR;
    } else {
        g.scrollTo = std::max(0, std::min(g.scrollTo, maxL));
        g.scrollRTo = std::max(0, std::min(g.scrollRTo, maxR));
    }
}

// ---------------------------------------------------------------- Zeichnen (transparentes Fenster)

static RECT CurrentClip(HDC dc) {
    RECT c;
    if (GetClipBox(dc, &c) == NULLREGION) SetRectEmpty(&c);
    return c;
}

// Abgerundete Fläche (rad = Durchmesser der Ecke wie bei GDI-RoundRect)
static void FillRound(HDC dc, const RECT& r, int rad, COLORREF c) {
    int kind = c == cCard ? State::SH_CARD : c == cCardHot ? State::SH_CARDHOT : c == cHover ? State::SH_HOVER
             : c == cSearch ? State::SH_SEARCH : State::SH_SOLID;
    float maxR = std::min(r.right - r.left, r.bottom - r.top) / 2.f;
    g.shapes.push_back({r, std::min(rad / 2.f, maxR), kind, c, CurrentClip(dc), g.drawFade});
}

static void FillCircle(HDC dc, int cx, int cy, int r, COLORREF c) {
    g.shapes.push_back({{cx - r, cy - r, cx + r, cy + r}, (float)r, State::SH_SOLID, c, CurrentClip(dc), g.drawFade});
}

// Text weiß in die Maske schreiben und Farbe/Bereich merken
// Fertig gezeichnete Texte (weiß auf schwarz, ClearType) zwischenspeichern: beim Scrollen ändern sich nur die
// Positionen, also wird jeder Text nur einmal gezeichnet und danach bloß kopiert. Wird beim Schließen geleert.
static std::unordered_map<std::wstring, HBITMAP> sTextCache;
static HDC sTextDC = nullptr;
static size_t sTextCacheBytes = 0;
static void ClearTextCache() {
    for (auto& e : sTextCache) DeleteObject(e.second);
    sTextCache.clear();
    sTextCacheBytes = 0;
    if (sTextDC) { DeleteDC(sTextDC); sTextDC = nullptr; }
}
static void Text(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT fl) {
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0 || s.empty()) return;
    if (dc == g.maskDC && (size_t)w * h <= 300000) {
        wchar_t k[64];
        wsprintfW(k, L"\x1%p|%u|%d|%d", (void*)f, fl, w, h);
        std::wstring key = s + k;
        auto it = sTextCache.find(key);
        if (!sTextDC) sTextDC = CreateCompatibleDC(dc);
        HBITMAP bmp;
        if (it == sTextCache.end()) {
            if (sTextCacheBytes > 24u * 1024 * 1024) ClearTextCache(), sTextDC = CreateCompatibleDC(dc);  // Obergrenze
            BITMAPINFO bi = {};
            bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
            void* bits = nullptr;
            bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!bmp) goto direct;
            HGDIOBJ o = SelectObject(sTextDC, bmp);
            SelectObject(sTextDC, f);
            SetBkMode(sTextDC, TRANSPARENT);
            SetTextColor(sTextDC, RGB(255, 255, 255));
            RECT rr = {0, 0, w, h};
            DrawTextW(sTextDC, s.c_str(), -1, &rr, fl | DT_NOPREFIX);
            SelectObject(sTextDC, o);
            sTextCache[key] = bmp;
            sTextCacheBytes += (size_t)w * h * 4;
        } else {
            bmp = it->second;
        }
        HGDIOBJ o = SelectObject(sTextDC, bmp);
        BitBlt(dc, r.left, r.top, w, h, sTextDC, 0, 0, SRCPAINT);  // Abdeckung dazu (beachtet den Clip-Bereich)
        SelectObject(sTextDC, o);
    } else {
    direct:
        SelectObject(dc, f);
        SetTextColor(dc, RGB(255, 255, 255));
        DrawTextW(dc, s.c_str(), -1, &r, fl | DT_NOPREFIX);
    }
    RECT clip = CurrentClip(dc), area;
    if (IntersectRect(&area, &r, &clip)) g.texts.push_back({area, c, g.drawFade});
}
static HBITMAP PickIcon(const App& a, int sz) {
    // passende Ausführung: klein für Listen, groß für Kacheln
    if (a.iconSmall && (sz <= S(28) || !a.icon)) return a.iconSmall;
    return a.icon ? a.icon : a.iconSmall;
}

// Symbol sofort zeichnen (mit echtem Alphakanal, nach dem Zusammensetzen)
static void BlitIcon(HDC dc, int idx, int x, int y, int sz, float fade = 1.f) {
    const App& a = g.apps[idx];
    HBITMAP icon = PickIcon(a, sz);
    if (!icon) return;
    BITMAP bm = {};
    GetObjectW(icon, sizeof(bm), &bm);
    if (std::abs(bm.bmWidth - sz) <= sz / 4 && bm.bmWidth == bm.bmHeight) {
        // Pixel 1:1 übernehmen und mittig setzen statt (unscharf) zu skalieren
        x += (sz - bm.bmWidth) / 2;
        y += (sz - bm.bmHeight) / 2;
        sz = bm.bmWidth;
    }
    HDC m = CreateCompatibleDC(dc);
    HGDIOBJ o = SelectObject(m, icon);
    if (bm.bmBitsPixel == 32) {
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, (BYTE)(255 * fade), AC_SRC_ALPHA};
        AlphaBlend(dc, x, y, sz, sz, m, 0, 0, bm.bmWidth, bm.bmHeight, bf);
    } else {
        SetStretchBltMode(dc, HALFTONE);
        StretchBlt(dc, x, y, sz, sz, m, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
        GdiFlush();
        for (int yy = std::max(0, y); yy < std::min(g.memH, y + sz); yy++)
            for (int xx = std::max(0, x); xx < std::min(g.memW, x + sz); xx++)
                g.bits[((size_t)yy * g.memW + xx) * 4 + 3] = 255;
    }
    SelectObject(m, o);
    DeleteDC(m);
}

// Im ersten Durchgang: Symbol vormerken bzw. Platzhalter zeichnen
static void DrawAppIcon(HDC dc, int idx, int x, int y, int sz) {
    const App& a = g.apps[idx];
    if (a.icon || a.iconSmall) {
        RECT clip;
        GetClipBox(dc, &clip);
        g.iconQueue.push_back({idx, x, y, sz, clip, g.drawFade});
    } else {
        RECT r = {x, y, x + sz, y + sz};
        FillRound(dc, r, sz / 3, cHover);
        if (sz >= S(24)) {
            wchar_t ch[2] = {a.name.empty() ? L'?' : a.name[0], 0};
            CharUpperBuffW(ch, 1);
            Text(dc, ch, r, g.fIcon, cDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
}

static bool MakeDib(int W, int H, HDC& dc, HBITMAP& bmp, HBITMAP& old, BYTE*& bitsOut) {
    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB};
    HDC screen = GetDC(nullptr);
    dc = CreateCompatibleDC(screen);
    void* bits = nullptr;
    bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!bmp) {
        DeleteDC(dc);
        dc = nullptr;
        return false;
    }
    bitsOut = (BYTE*)bits;
    old = (HBITMAP)SelectObject(dc, bmp);
    return true;
}

static bool EnsureDib(int W, int H) {
    if (g.memDC && g.maskDC && g.memW == W && g.memH == H) return true;
    // nur die Zeichenpuffer neu anlegen (Bild/Glas bleiben)
    if (g.memDC) { SelectObject(g.memDC, g.memOld); DeleteObject(g.memBmp); DeleteDC(g.memDC); g.memDC = nullptr; }
    if (g.maskDC) { SelectObject(g.maskDC, g.maskOld); DeleteObject(g.maskBmp); DeleteDC(g.maskDC); g.maskDC = nullptr; }
    if (!MakeDib(W, H, g.memDC, g.memBmp, g.memOld, g.bits)) return false;
    if (!MakeDib(W, H, g.maskDC, g.maskBmp, g.maskOld, g.maskBits)) return false;
    g.memW = W;
    g.memH = H;
    return true;
}
// Eingabefeld (Suche, neuer Ordner) mit Platzhalter und blinkender Schreibmarke
static void DrawField(HDC dc, RECT box, const std::wstring& text, const wchar_t* placeholder, bool active) {
    FillRound(dc, box, S(12), cSearch);
    RECT tr = {box.left + S(14), box.top, box.right - S(14), box.bottom};
    bool empty = text.empty();
    SelectObject(dc, g.fText);
    int n = (int)text.size();
    auto xAt = [&](int i) {
        SIZE e = {};
        if (i > 0) GetTextExtentPoint32W(dc, text.c_str(), i, &e);
        return (int)e.cx;
    };
    int car = active && g.caretOwner ? std::min(g.caret, n) : n;
    int anc = active && g.caretOwner ? std::min(g.anchor, n) : n;
    int avail = tr.right - tr.left - S(4), full = xAt(n), cx = xAt(car), off = 0;
    if (active) {
        // Ausschnitt so verschieben, dass die Schreibmarke sichtbar bleibt
        off = g.fieldOff;
        if (cx - off > avail) off = cx - avail;
        if (cx - off < 0) off = cx;
        if (full - off < avail) off = std::max(0, full - avail);
        g.fieldOff = off;
        g.activeField = box;
    } else if (full > avail) {
        off = 0;
    }
    SaveDC(dc);
    IntersectClipRect(dc, tr.left, tr.top, tr.right, tr.bottom);
    if (active && car != anc && !empty) {  // Markierung
        int a = std::min(car, anc), b = std::max(car, anc);
        FillRound(dc, {tr.left + xAt(a) - off, box.top + S(7), tr.left + xAt(b) - off, box.bottom - S(7)}, S(4), RGB(60, 110, 210));
    }
    Text(dc, empty ? placeholder : text, {tr.left - off, tr.top, tr.left - off + std::max(full, avail) + S(40), tr.bottom}, g.fText,
         empty ? cDim : cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RestoreDC(dc, -1);
    if (active && g.caretOn && car == anc && GetForegroundWindow() == g.hwnd) {
        int x = tr.left + cx - off + S(1);
        FillRound(dc, {x, box.top + S(9), x + std::max(2, S(2)), box.bottom - S(9)}, 2, cText);
    }
}

// Name der Taste, z. B. "Ende" oder "Strg+Alt+M"
static std::wstring HotkeyName(UINT vk, UINT mods) {
    if (!vk) return L"keine";
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Strg+";
    if (mods & MOD_ALT) s += L"Alt+";
    if (mods & MOD_SHIFT) s += L"Umschalt+";
    if (mods & MOD_WIN) s += L"Win+";
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lp = (LONG)(sc << 16);
    switch (vk) {  // Navigationstasten sind "erweiterte" Tasten
    case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME: case VK_INSERT: case VK_DELETE:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
        lp |= 1 << 24;
    }
    wchar_t name[64] = {};
    if (GetKeyNameTextW(lp, name, 64) > 0) s += name;
    else s += L"Taste " + std::to_wstring(vk);
    return s;
}

static void DrawGlyphButton(HDC dc, const RECT& r, const wchar_t* glyph, bool hot, bool enabled) {
    if (hot && enabled) FillRound(dc, r, S(8), cHover);
    RECT rr = r;
    Text(dc, glyph, rr, g.fGlyph, enabled ? cDim : RGB(85, 85, 95), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// ---------------------------------------------------------------- Musik: Cover-Bilder

static bool LoadImageCover(const std::wstring& path, int W, int H, std::vector<uint32_t>& out);

// Cover in Anzeigegröße mit runden Ecken (zwischengespeichert, solange die Leiste offen ist)
static HBITMAP CoverBmp(const std::wstring& path, int px) {
    if (path.empty()) return nullptr;
    std::wstring key = path + L"|" + std::to_wstring(px);
    auto it = g.coverCache.find(key);
    if (it != g.coverCache.end()) return it->second;
    HBITMAP bmp = nullptr;
    std::vector<uint32_t> img;
    if (LoadImageCover(path, px, px, img)) {
        BITMAPINFO bi = {};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), px, -px, 1, 32, BI_RGB};
        void* bits = nullptr;
        bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp) {
            float R = std::max(3.f, px * 0.16f);
            uint32_t* d = (uint32_t*)bits;
            for (int y = 0; y < px; y++)
                for (int x = 0; x < px; x++) {
                    float cov = 1;
                    if ((x < R || x >= px - R) && (y < R || y >= px - R)) {
                        float cx = x < R ? R : px - R, cy = y < R ? R : px - R;
                        float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                        cov = R - std::sqrt(dx * dx + dy * dy) + 0.5f;
                        cov = cov < 0 ? 0 : cov > 1 ? 1 : cov;
                    }
                    uint32_t p = img[(size_t)y * px + x];
                    int a = (int)(255 * cov);
                    d[(size_t)y * px + x] = ((uint32_t)a << 24) | ((((p >> 16) & 255) * a / 255) << 16) | ((((p >> 8) & 255) * a / 255) << 8) |
                                            ((p & 255) * a / 255);
                }
        }
    }
    g.coverCache[key] = bmp;
    return bmp;
}

static void FreeCovers() {
    for (auto& kv : g.coverCache)
        if (kv.second) DeleteObject(kv.second);
    g.coverCache.clear();
}

// Cover zeichnen (nach dem Zusammensetzen) oder Platzhalter mit Notensymbol
// Claude-Symbol (von claude.ai geholt wie die Website-Symbole der Favoriten-Links), aber nur der helle Strahlenstern ohne die orange Fläche:
// die Helligkeit im Blaukanal (Orange ≈ 70–90, Stern ≈ 220+) wird zur Deckkraft, gezeichnet wird in Weiß. nullptr, solange das Symbol fehlt.
static HBITMAP ClaudeIcon(int px) {
    std::wstring f = FaviconFile(L"claude.ai");
    if (GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
    std::wstring key = L"claude-glyph|" + std::to_wstring(px);
    auto it = g.coverCache.find(key);
    if (it != g.coverCache.end()) return it->second;
    HBITMAP bmp = nullptr;
    std::vector<uint32_t> img;
    if (LoadImageCover(f, px, px, img)) {
        BITMAPINFO bi = {};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), px, -px, 1, 32, BI_RGB};
        void* bits = nullptr;
        bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp) {
            uint32_t* d = (uint32_t*)bits;
            for (size_t i = 0; i < (size_t)px * px; i++) {
                int b = img[i] & 255;
                float t = (b - 110) / 90.f;  // 110 -> 0, 200 -> 1
                t = t < 0 ? 0 : t > 1 ? 1 : t;
                t = t * t * (3 - 2 * t);
                int a = (int)(255 * t), c = 245 * a / 255;  // weiß (leicht gedämpft), vormultipliziert
                d[i] = ((uint32_t)a << 24) | ((uint32_t)c << 16) | ((uint32_t)c << 8) | (uint32_t)c;
            }
        }
    }
    g.coverCache[key] = bmp;
    return bmp;
}

static void DrawCover(HDC dc, const std::wstring& path, int x, int y, int px) {
    HBITMAP b = CoverBmp(path, px);
    if (b) {
        g.iconQueue.push_back({-3, x, y, px, CurrentClip(dc), g.drawFade, b});
    } else {
        FillRound(dc, {x, y, x + px, y + px}, std::max(S(8), px / 3), cCardHot);
        Text(dc, L"", {x, y, x + px, y + px}, g.fGlyph, cDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

static const Song* SongAt(int idx) { return idx >= 0 && idx < (int)Music_Songs().size() ? &Music_Songs()[idx] : nullptr; }

static void DrawTimerHit(HDC dc, const Hit& h, bool hot) {
    static HFONT big = nullptr;
    static UINT bigDpi = 0;
    if (!big || bigDpi != g.dpi) {
        if (big) DeleteObject(big);
        bigDpi = g.dpi;
        big = CreateFontW(-S(34), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH, g.fontName.c_str());
    }
    if (h.type == HT::TimerWheel) {
        int rowH = S(44), mid = (h.rc.top + h.rc.bottom) / 2, n = WheelMax(h.idx);
        FillRound(dc, {h.rc.left + S(2), mid - rowH / 2, h.rc.right - S(2), mid + rowH / 2}, S(10), cCardHot);
        double pos = g.wheel[h.idx];
        int base = (int)std::floor(pos);
        for (int k = base - 2; k <= base + 3; k++) {
            double off = k - pos;
            if (std::fabs(off) > 2.2) continue;
            int cy = mid + (int)(off * rowH), v = ((k % n) + n) % n;
            float fade = g.drawFade;
            g.drawFade = fade * (float)std::max(0.12, 1.0 - std::fabs(off) * 0.42);
            wchar_t t[4];
            swprintf(t, 4, L"%02d", v);
            Text(dc, t, {h.rc.left, cy - rowH / 2, h.rc.right - S(28), cy + rowH / 2}, big, cText, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            g.drawFade = fade;
        }
        Text(dc, h.idx == 0 ? L"h" : h.idx == 1 ? L"min" : L"s", {h.rc.right - S(26), mid - rowH / 2, h.rc.right, mid + rowH / 2}, g.fSmall, cDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    if (h.type == HT::TimerShow) {
        long long s = (TimerRemaining() + 999) / 1000;
        wchar_t t[16];
        swprintf(t, 16, L"%02lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
        Text(dc, g.timerRinging ? L"Zeit abgelaufen" : t, h.rc, big, g.timerRinging ? RGB(255, 150, 150) : cText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    bool main = h.idx == 0;
    FillRound(dc, h.rc, S(22), main ? (g.timerRinging ? RGB(200, 60, 60) : hot ? RGB(120, 180, 255) : cAccent) : hot ? cCardHot : cSearch);
    std::wstring label = h.idx == 1 ? L"Abbrechen" : h.idx == 3 ? L"Standardton" : h.idx == 2
        ? (g.ringFile.empty() ? std::wstring(L"Eigenen Klingelton wählen …") : g.ringFile.substr(g.ringFile.find_last_of(L'\\') + 1))
        : std::wstring(g.timerRinging ? L"Stopp" : g.timerEnd ? L"Pause" : g.timerLeft ? L"Weiter" : L"Start");
    Text(dc, label, {h.rc.left + S(10), h.rc.top, h.rc.right - S(10), h.rc.bottom}, main ? g.fHead : g.fText, main ? RGB(255, 255, 255) : cText,
         DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
static void DrawHit(HDC dc, const Hit& h, bool hot, int cardHot) {
    switch (h.type) {
    // ---------- Hauptansicht
    case HT::Card: {
        FillRound(dc, h.rc, S(20), h.idx == cardHot ? cCardHot : cCard);
        int ip = S(14);
        RECT nr = {h.rc.left + ip + S(4), h.rc.top + ip - S(4), h.rc.right - ip - S(90), h.rc.top + ip + S(26)};
        Text(dc, g.folders[h.idx].name, nr, g.fHead, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT cr2 = {h.rc.right - ip - S(120), nr.top, h.rc.right - ip - S(4), nr.bottom};
        size_t n = g.folders[h.idx].items.size();
        Text(dc, std::to_wstring(n) + (n == 1 ? L" App  ›" : L" Apps  ›"), cr2, g.fSmall, cDim, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case HT::AppSizeRow: {  // Alle Apps: Symbol, Name, rechts Größe mit Vergleichsbalken
        if (hot) FillRound(dc, h.rc, S(12), cHover);
        int isz = S(30), iy = (h.rc.top + h.rc.bottom - isz) / 2;
        DrawAppIcon(dc, h.idx, h.rc.left + S(10), iy, isz);
        bool run = g.running.count(h.idx) > 0;
        int barW = std::min(S(130), (int)((h.rc.right - h.rc.left) * 24 / 100)), sizeW = S(70);
        int bx1 = h.rc.right - S(52), bx0 = bx1 - barW;  // rechts Platz für den Beenden-Knopf
        RECT nr = {h.rc.left + S(10) + isz + S(12), h.rc.top, bx0 - sizeW - S(10), h.rc.bottom};
        if (run) {  // laufend: grüner Punkt, darunter Arbeitsspeicher
            RECT n1 = nr, n2 = nr;
            n1.bottom = (nr.top + nr.bottom) / 2 + S(2);
            n2.top = n1.bottom;
            Text(dc, g.apps[h.idx].name, n1, g.fText, cText, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
            FillCircle(dc, h.rc.left + S(10) + isz - S(1), iy + isz - S(1), S(5), RGB(80, 210, 120));
            wchar_t ram[48];
            swprintf(ram, 48, L"läuft · %.0f MB RAM", RunningRam(h.idx) / 1048576.0);
            Text(dc, ram, n2, g.fSmall, RGB(110, 210, 140), DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
            Text(dc, g.apps[h.idx].name, nr, g.fText, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        long long sz = -1;
        for (auto& e : g.appSizes)
            if (e.first == h.idx) { sz = e.second; break; }
        std::wstring st;
        if (sz <= 0) st = L"–";  // unbekannt (0 = nicht messbar)
        else {
            wchar_t t[32];
            double gb = sz / 1073741824.0;
            if (gb >= 10) swprintf(t, 32, L"%.0f GB", gb);
            else if (gb >= 1) swprintf(t, 32, L"%.1f GB", gb);
            else swprintf(t, 32, L"%.0f MB", sz / 1048576.0);
            st = t;
        }
        Text(dc, st, {bx0 - sizeW - S(8), h.rc.top, bx0 - S(10), h.rc.bottom}, g.fSmall, sz < 0 ? cDim : cText, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        int cy = (h.rc.top + h.rc.bottom) / 2;
        FillRound(dc, {bx0, cy - S(2), bx1, cy + S(2)}, S(2), cSearch);
        if (sz > 0) {
            int fw = std::max(S(4), (int)((bx1 - bx0) * std::min(1.0, (double)sz / g.appMax)));
            FillRound(dc, {bx0, cy - S(2), bx0 + fw, cy + S(2)}, S(2), cAccent);
        }
        break;
    }
    case HT::SortBtn: {
        bool sel = h.idx == 2 ? g.onlyRunning : !g.onlyRunning && g.sortBySize == (h.idx == 1);
        RECT r = {h.rc.left + S(2), h.rc.top, h.rc.right - S(2), h.rc.bottom};
        FillRound(dc, r, S(10), sel ? cHover : hot ? cCardHot : cSearch);
        Text(dc, h.idx == 0 ? L"A–Z" : h.idx == 1 ? L"Größe" : L"Laufend", r, sel ? g.fHead : g.fSmall, sel ? cText : cDim,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case HT::KillBtn:  // laufende App beenden
        FillRound(dc, h.rc, S(10), hot ? RGB(200, 60, 60) : cSearch);
        Text(dc, L"\uE711", h.rc, g.fGlyph, hot ? RGB(255, 255, 255) : RGB(255, 150, 150), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    case HT::GameHead:
    case HT::GameRow: {
        // [Symbol Name] | rechts drei kleine Zeilen übereinander: Größe, Spielzeit (je mit Balken), zuletzt gespielt
        if (h.type == HT::GameHead) break;
        int w = h.rc.right - h.rc.left, statsW = std::min(S(300), w * 50 / 100);
        int sx = h.rc.right - statsW - S(10);
        if (hot) FillRound(dc, h.rc, S(14), cHover);
        int isz = S(40), iy = h.rc.top + (h.rc.bottom - h.rc.top - isz) / 2;
        DrawAppIcon(dc, h.idx, h.rc.left + S(10), iy, isz);
        RECT nr = {h.rc.left + S(10) + isz + S(12), h.rc.top, sx - S(8), h.rc.bottom};
        Text(dc, g.apps[h.idx].name, nr, g.fText, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        GameStats s;
        auto it = g.gstats.find(h.idx);
        if (it != g.gstats.end()) s = it->second;
        auto fmtSize = [](long long b) -> std::wstring {
            if (b < 0) return L"–";
            wchar_t t[32];
            double gb = b / 1073741824.0;
            if (gb >= 10) swprintf(t, 32, L"%.0f GB", gb);
            else if (gb >= 1) swprintf(t, 32, L"%.1f GB", gb);
            else swprintf(t, 32, L"%.0f MB", b / 1048576.0);
            return t;
        };
        auto fmtTime = [](int m, bool measured) -> std::wstring {
            if (m < 0) return L"–";
            if (m == 0) return measured ? L"misst ab jetzt" : L"0 Min";
            if (m < 60) return std::to_wstring(m) + L" Min";
            if (m < 6000) { wchar_t t[32]; swprintf(t, 32, L"%.1f Std", m / 60.0); return t; }
            return std::to_wstring(m / 60) + L" Std";
        };
        auto fmtAgo = [](long long t) -> std::wstring {
            if (t <= 0) return L"–";
            long long d = ((long long)time(nullptr) - t) / 86400;
            if (d <= 0) return L"heute";
            if (d == 1) return L"gestern";
            if (d < 14) return L"vor " + std::to_wstring(d) + L" Tagen";
            if (d < 60) return L"vor " + std::to_wstring(d / 7) + L" Wochen";
            if (d < 365) return L"vor " + std::to_wstring(d / 30) + L" Monaten";
            return d < 730 ? L"vor 1 Jahr" : L"vor " + std::to_wstring(d / 365) + L" Jahren";
        };
        int lineH = S(17), top = (h.rc.top + h.rc.bottom) / 2 - lineH * 3 / 2;
        int labW = S(62), valW = S(92);
        auto stat = [&](int k, const wchar_t* label, const std::wstring& txt, double frac, bool bar) {
            int y = top + k * lineH;
            Text(dc, label, {sx, y, sx + labW, y + lineH}, g.fSmall, cDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            Text(dc, txt, {sx + labW, y, sx + labW + valW, y + lineH}, g.fSmall, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (!bar) return;
            int cy = y + lineH / 2;
            RECT br = {sx + labW + valW, cy - S(2), h.rc.right - S(14), cy + S(2)};
            if (br.right - br.left < S(20)) return;
            FillRound(dc, br, S(2), cSearch);
            if (frac > 0) FillRound(dc, {br.left, br.top, br.left + std::max(S(4), (int)((br.right - br.left) * std::min(1.0, frac))), br.bottom}, S(2), cAccent);
        };
        stat(0, L"Größe", fmtSize(s.size), s.size > 0 ? (double)s.size / g.gMaxSize : 0, true);
        stat(1, L"Spielzeit", fmtTime(s.minutes, s.measured), s.minutes > 0 ? (double)s.minutes / g.gMaxMin : 0, true);
        stat(2, L"Zuletzt", fmtAgo(s.lastPlayed), 0, false);
        break;
    }
    case HT::Slot:
    case HT::Cell: {
        bool cell = h.type == HT::Cell;
        if (hot) FillRound(dc, {h.rc.left + S(3), h.rc.top + S(1), h.rc.right - S(3), h.rc.bottom - S(1)}, S(14), cHover);
        int cw = h.rc.right - h.rc.left, isz = S(48);
        int iy = h.rc.top + (cell ? S(10) : S(6));
        DrawAppIcon(dc, h.idx, h.rc.left + (cw - isz) / 2, iy, isz);
        RECT nr = {h.rc.left + S(5), iy + isz + S(5), h.rc.right - S(5), h.rc.bottom - S(3)};
        Text(dc, g.apps[h.idx].name, nr, g.fSmall, cText,
             DT_CENTER | DT_END_ELLIPSIS | (cell ? DT_WORDBREAK | DT_EDITCONTROL : DT_SINGLELINE));
        break;
    }
    case HT::Mini: {
        if (hot) FillRound(dc, {h.rc.left + S(3), h.rc.top + S(1), h.rc.right - S(3), h.rc.bottom - S(1)}, S(14), cHover);
        auto& items = g.folders[h.idx].items;
        int cw = h.rc.right - h.rc.left, box = S(48);
        int bx = h.rc.left + (cw - box) / 2, by = h.rc.top + S(6);
        FillRound(dc, {bx, by, bx + box, by + box}, S(12), hot ? cCardHot : cHover);
        int shown = g.folders[h.idx].shown;
        int gap = S(3), m = (box - 3 * gap) / 2;
        for (int k = 0; k < 4 && shown + k < (int)items.size(); k++)
            DrawAppIcon(dc, items[shown + k], bx + gap + (k % 2) * (m + gap), by + gap + (k / 2) * (m + gap), m);
        RECT nr = {h.rc.left + S(4), by + box + S(5), h.rc.right - S(4), h.rc.bottom - S(3)};
        Text(dc, L"+" + std::to_wstring((int)items.size() - shown) + L" weitere", nr, g.fSmall, cDim, DT_CENTER | DT_SINGLELINE);
        break;
    }
    // ---------- Einstellungen: Reiter
    case HT::Tab: {
        bool sel = g.setTab == h.idx;
        RECT r = {h.rc.left + S(2), h.rc.top + S(2), h.rc.right - S(2), h.rc.bottom - S(2)};
        if (sel) FillRound(dc, r, S(10), cHover);
        else if (hot) FillRound(dc, r, S(10), cCardHot);
        Text(dc, h.idx == 0 ? L"Design" : h.idx == 1 ? L"Ordner" : h.idx == 2 ? L"Funktionen" : L"Erweiterungen", h.rc, sel ? g.fHead : g.fText, sel ? cText : cDim,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    // ---------- Design
    case HT::SetCard:
        FillRound(dc, h.rc, S(18), h.pane == 1 ? cCardHot : cCard);
        break;
    case HT::TimerWheel:
    case HT::TimerShow:
    case HT::TimerBtn:
        DrawTimerHit(dc, h, hot);
        break;
    case HT::SetSection: {
        RECT nr = {h.rc.left + S(16), h.rc.top, h.rc.right - S(16), h.rc.bottom};
        if (h.idx == 3) {
            Text(dc, L"Startseite", nr, g.fHead, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            Text(dc, L"4 Apps hierher ziehen", nr, g.fSmall, cDim, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            break;
        }
        Text(dc, h.idx == 0 ? L"Aussehen" : h.idx == 1 ? L"Allgemein" : h.idx == 4 ? L"Musik" : h.idx == 5 ? L"Tasten" : h.idx == 6 ? L"Spiele" : h.idx == 7 ? L"HoverBar" : L"Schrift & Bewegung", nr, g.fHead, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case HT::SetRowLabel: {
        static const wchar_t* labels[] = {L"Tönung", L"Glas-Effekt", L"Farbe", L"Hintergrundbild", L"Bildstärke",
                                          L"Mit Windows starten", L"Nur auf dem Desktop öffnen", L"Unschärfe",
                                          L"Schriftart", L"Fette Schrift", L"Ausfahren", L"Taste zum Öffnen",
                                          L"HoverBar-Player", L"Spotify", L"Leiser bei Videos", L"Musik-Tasten", L"Link laden",
                                          L"Musik-Player", L"Steam", L"Epic Games", L"Riot Games", L"Andere Spiele"};
        if (h.idx >= 28 && h.idx <= 32) {
            static const wchar_t* names[] = {L"Play / Pause", L"Nächster Song", L"Voriger Song", L"Lauter", L"Leiser"};
            Text(dc, names[h.idx - 28], {h.rc.left + S(8), h.rc.top, h.rc.right - S(176), h.rc.bottom}, g.fText, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            break;
        }
        if (h.idx == 33) {
            Text(dc, L"Kalender", {h.rc.left + S(8), h.rc.top, h.rc.right - S(70), h.rc.bottom}, g.fText, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            break;
        }
        if (h.idx == 27) {
            RECT a = {h.rc.left + S(8), h.rc.top, h.rc.right - S(70), (h.rc.top + h.rc.bottom) / 2 + S(1)};
            RECT b = {a.left, a.bottom, a.right, h.rc.bottom};
            Text(dc, L"Home (Apps)", a, g.fText, cText, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
            Text(dc, L"Aus: Start in der Musik-Bibliothek, ohne Apps", b, g.fSmall, cDim, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
            break;
        }
        if (h.idx == 25 || h.idx == 26) {
            Text(dc, h.idx == 25 ? L"Cover in Kacheln" : L"Trennlinien", {h.rc.left + S(8), h.rc.top, h.rc.right - S(70), h.rc.bottom}, g.fText, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            break;
        }
        if (h.idx == 24) {  // HoverBar: Version + Update-Stand
            UpdateStatus us = Update_Status();
            std::wstring sub = us.state == UPD_IDLE ? L"Version " + std::wstring(HB_VERSION) : us.msg;
            if (us.state == UPD_DOWNLOADING && us.percent >= 0) sub += L" " + std::to_wstring(us.percent) + L" %";
            RECT a = {h.rc.left + S(8), h.rc.top, h.rc.right - S(190), (h.rc.top + h.rc.bottom) / 2 + S(1)};
            RECT b = {a.left, a.bottom, a.right, h.rc.bottom};
            Text(dc, L"HoverBar " + std::wstring(HB_VERSION), a, g.fText, cText, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
            Text(dc, sub, b, g.fSmall, us.state == UPD_ERROR ? RGB(255, 150, 150) : us.state == UPD_READY || us.state == UPD_AVAILABLE ? RGB(110, 210, 140) : cDim,
                 DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
            break;
        }
        if (h.idx == 23) {  // Song-Ordner: Name + Pfad
            RECT a = {h.rc.left + S(8), h.rc.top, h.rc.right - S(170), (h.rc.top + h.rc.bottom) / 2 + S(1)};
            RECT b = {a.left, a.bottom, a.right, h.rc.bottom};
            Text(dc, L"Song-Ordner", a, g.fText, cText, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
            Text(dc, Music_Folder(), b, g.fSmall, cDim, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_PATH_ELLIPSIS);
            break;
        }
        if (h.idx == 22) {  // yt-dlp: Name + Stand
            ToolStatus ts = Music_ToolStatus();
            std::wstring sub;
            bool have = Music_ToolAvailable();
            if (ts.state == TOOL_DOWNLOADING) sub = L"Lädt herunter … " + (ts.percent >= 0 ? std::to_wstring(ts.percent) + L" %" : L"");
            else if (ts.state == TOOL_CHECKING || ts.state == TOOL_VERIFYING || ts.state == TOOL_ERROR || ts.state == TOOL_DONE) sub = ts.msg;
            else if (!have) sub = L"Fehlt – wird für YouTube-Links gebraucht";
            else sub = L"Installiert" + (ts.local.empty() ? std::wstring() : L": Version " + ts.local);
            if (ts.state == TOOL_DONE && !ts.local.empty()) sub += L" (Version " + ts.local + L")";
            RECT a = {h.rc.left + S(8), h.rc.top, h.rc.right - S(170), (h.rc.top + h.rc.bottom) / 2 + S(1)};
            RECT b = {a.left, a.bottom, a.right, h.rc.bottom};
            Text(dc, L"yt-dlp (YouTube-Downloader)", a, g.fText, cText, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
            Text(dc, sub, b, g.fSmall, ts.state == TOOL_ERROR ? RGB(255, 150, 150) : ts.state == TOOL_DONE ? RGB(110, 210, 140) : cDim,
                 DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
            break;
        }
        if (h.idx >= 12) {  // Funktionen: Name + kurze Erklärung
            const wchar_t* subs[] = {L"Deine Songs aus YouTube-Links (Musik-Ordner)",
                                            L"Leiste und Tasten steuern Spotify, Play startet es",
                                            L"Musik auf 10 %, solange YouTube & Co. Ton ausgeben",
                                            L"Öffnen-Taste halten + eine dieser Tasten",
                                            L"Lädt den YouTube-Link der offenen Seite",
                                            g.musicOn ? L"Musikleiste unten in der HoverBar" : L"Aus: keine Musikleiste, keine Musik-Tasten",
                                            L"Spielzeit, zuletzt gespielt und Größe von Steam",
                                            L"Größe und protokollierte Spielzeit vom Epic Launcher",
                                            L"VALORANT, League of Legends, TFT: Größe + eigene Messung",
                                            L"Spielzeit selbst messen (einmal pro Minute), Ordnergröße"};
            RECT a = {h.rc.left + S(8), h.rc.top, h.rc.right - S(70), (h.rc.top + h.rc.bottom) / 2 + S(1)};
            RECT b = {a.left, a.bottom, a.right, h.rc.bottom};
            Text(dc, labels[h.idx], a, g.fText, cText, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
            Text(dc, subs[h.idx - 12], b, g.fSmall, cDim, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
            break;
        }
        bool wide = h.idx == 11 || h.idx == 1 || h.idx == 5 || h.idx == 6 || h.idx == 9;
        RECT nr = {h.rc.left + S(8), h.rc.top, wide ? h.rc.right - S(70) : h.rc.left + S(136), h.rc.bottom};
        if (h.idx == 3 && !g.imgPath.empty()) {
            RECT a = nr, b = nr;
            a.bottom = (nr.top + nr.bottom) / 2 + S(2);
            b.top = a.bottom - S(1);
            Text(dc, labels[3], a, g.fText, cText, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
            const wchar_t* fn = wcsrchr(g.imgPath.c_str(), L'\\');
            Text(dc, fn ? fn + 1 : g.imgPath.c_str(), b, g.fSmall, cDim, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
            Text(dc, labels[h.idx], nr, g.fText, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        break;
    }
    case HT::SetSlider: {
        if (h.idx == 10) {  // Schneiden: Spur mit zwei schmalen, abgerundeten Linien
            int cy = (h.rc.top + h.rc.bottom) / 2, kr = S(10);
            int x0 = h.rc.left + kr, x1 = h.rc.right - kr;
            double L = std::max(1.0, g.trimLen);
            int xa = x0 + (int)((x1 - x0) * g.trimA / L), xb = x0 + (int)((x1 - x0) * g.trimB / L);
            int maxH = (h.rc.bottom - h.rc.top) / 2 - S(3);
            FillRound(dc, {x0 - S(4), h.rc.top, x1 + S(4), h.rc.bottom}, S(8), cSearch);
            if (!g.wave.empty()) {
                // Wellenform: schmale Balken, in der Auswahl hell, außerhalb gedimmt
                int step = S(3), bwid = std::max(1, S(2));
                int nw = (int)g.wave.size();
                for (int x = x0; x < x1; x += step) {
                    int a = (x - x0) * nw / std::max(1, x1 - x0), b = std::min(nw, (x + step - x0) * nw / std::max(1, x1 - x0) + 1);
                    float v = 0;
                    for (int k = a; k < b; k++) v = std::max(v, g.wave[k]);
                    int hh = std::max(S(1), (int)(v * maxH));
                    bool in = x >= xa && x <= xb;
                    FillRound(dc, {x, cy - hh, x + bwid, cy + hh}, bwid, in ? cAccent : RGB(85, 90, 105));
                }
            } else {
                FillRound(dc, {x0, cy - S(1), x1, cy + S(1)}, S(1), RGB(85, 90, 105));
                FillRound(dc, {xa, cy - S(1), std::max(xa + S(2), xb), cy + S(1)}, S(1), cAccent);
            }
            // Wiedergabe-Position beim Probehören
            if (g.pst.active && g.pst.songId == g.editId && g.pst.playing) {
                double pos = g.pst.pos + (g.pst.dur < L - 0.5 ? g.trimPrevFrom : 0);
                int xp = x0 + (int)((x1 - x0) * std::min(1.0, pos / L));
                FillRound(dc, {xp - S(1), cy - S(12), xp + S(1), cy + S(12)}, S(2), RGB(250, 250, 252));
            }
            int lw = S(2), lh = (h.rc.bottom - h.rc.top) / 2 - S(1);
            for (int k = 1; k <= 2; k++) {
                int xl = k == 1 ? xa : xb;
                bool sel = g.trimHandle == k || g.trimSel == k;
                FillRound(dc, {xl - lw, cy - lh, xl + lw, cy + lh}, lw * 2, sel ? RGB(120, 180, 255) : RGB(250, 250, 252));
            }
            break;
        }
        int v =h.idx == 0 ? (g.bgAlpha - 40) * 100 / 215 : h.idx == 1 ? g.imgStrength : h.idx == 3 ? g.volume : g.blurAmount;
        int cy = (h.rc.top + h.rc.bottom) / 2, kr = S(9);
        int x0 = h.rc.left + kr, x1 = h.rc.right - kr;
        int kx = x0 + (x1 - x0) * v / 100;
        FillRound(dc, {x0, cy - S(3), x1, cy + S(3)}, S(6), cSearch);
        FillRound(dc, {x0, cy - S(3), std::max(x0 + S(6), kx), cy + S(3)}, S(6), cAccent);
        FillCircle(dc, kx, cy, kr, RGB(250, 250, 252));
        RECT vr = {h.rc.right + S(6), h.rc.top, h.rc.right + S(58), h.rc.bottom};
        Text(dc, std::to_wstring(v) + L" %", vr, g.fSmall, cDim, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case HT::SetToggle: {
        bool on = h.idx == 0 ? g.blur : h.idx == 1 ? AutostartEnabled() : h.idx == 2 ? g.onlyDesktop : h.idx == 3 ? g.fontBold
                : h.idx == 10 ? g.musicSrc == 0 : h.idx == 11 ? g.musicSrc == 1 : h.idx == 12 ? g.duckOn
                : h.idx == 13 ? g.musicKeys : h.idx == 15 ? g.musicOn
                : h.idx >= 16 && h.idx <= 19 ? Games_SourceOn(h.idx - 16) : h.idx == 20 ? g.tileCovers : h.idx == 21 ? g.songLines : h.idx == 22 ? g.appsOn : h.idx == 23 ? g.calendarOn : g.grabKey;
        int th = h.rc.bottom - h.rc.top;
        FillRound(dc, h.rc, th, on ? cAccent : (hot ? cCardHot : cHover));
        int kr = th / 2 - S(4);
        FillCircle(dc, on ? h.rc.right - S(4) - kr : h.rc.left + S(4) + kr, (h.rc.top + h.rc.bottom) / 2, kr, RGB(250, 250, 252));
        break;
    }
    case HT::CtlKeyBtn: {
        bool wait = g.ctlCapture == h.idx;
        FillRound(dc, h.rc, S(10), wait ? cAccent : hot ? cHover : cSearch);
        std::wstring key = HotkeyName(g.hotVk, 0) + L" + " + HotkeyName(g.ctlKey[h.idx], 0);
        Text(dc, wait ? L"Taste drücken …" : key, h.rc, g.fSmall, cText, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        break;
    }
    case HT::GrabKeyBtn:
        FillRound(dc, h.rc, S(10), g.grabCapture ? cAccent : hot ? cHover : cSearch);
        Text(dc, g.grabCapture ? L"Taste drücken …" : HotkeyName(g.grabVk, g.grabMods), h.rc, g.fSmall, cText, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        break;
    case HT::SetSwatch: {
        bool isPreset = false;
        for (int k = 0; k < kSwatchCount; k++)
            if (kSwatches[k].tint == g.tint) isPreset = true;
        bool custom = h.idx == kSwatchCount;
        bool sel = custom ? !isPreset : kSwatches[h.idx].tint == g.tint;
        COLORREF col;
        if (custom) {
            int r = GetRValue(g.tint), gg = GetGValue(g.tint), b = GetBValue(g.tint);
            int m = std::max(1, std::max(r, std::max(gg, b)));
            col = sel ? RGB(r * 200 / m, gg * 200 / m, b * 200 / m) : RGB(90, 90, 100);
        } else {
            col = kSwatches[h.idx].disp;
        }
        int cx = (h.rc.left + h.rc.right) / 2, cy = (h.rc.top + h.rc.bottom) / 2, rad = (h.rc.right - h.rc.left) / 2;
        if (sel || hot) {
            FillCircle(dc, cx, cy, rad + S(3), sel ? RGB(250, 250, 252) : RGB(140, 140, 150));
            FillCircle(dc, cx, cy, rad + S(1), RGB(36, 36, 44));
        }
        FillCircle(dc, cx, cy, rad, col);
        if (custom && !sel) Text(dc, L"+", h.rc, g.fHead, cText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case HT::SetSpeed: {
        static const wchar_t* names[] = {L"Langsam", L"Normal", L"Schnell", L"Sofort"};
        bool sel = g.animSpeed == h.idx;
        RECT r = {h.rc.left + S(2), h.rc.top, h.rc.right - S(2), h.rc.bottom};
        FillRound(dc, r, S(10), sel ? cHover : hot ? cCardHot : cSearch);
        Text(dc, names[h.idx], r, sel ? g.fHead : g.fSmall, sel ? cText : cDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case HT::SetButton:
        if (h.idx == 3) {  // Taste zum Öffnen
            FillRound(dc, h.rc, S(10), g.hotCapture ? cAccent : hot ? cHover : cSearch);
            Text(dc, g.hotCapture ? L"Taste drücken …  (Esc = keine)" : HotkeyName(g.hotVk, g.hotMods), h.rc, g.hotCapture ? g.fHead : g.fText, cText,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            break;
        }
        if (h.idx == 2) {  // Schriftauswahl: Name in der eigenen Schrift
            FillRound(dc, h.rc, S(10), hot ? cHover : cSearch);
            HFONT pf = CreateFontW(-S(15), 0, 0, 0, g.fontBold ? FW_SEMIBOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH,
                                   g.fontName.c_str());
            Text(dc, g.fontName, {h.rc.left + S(14), h.rc.top, h.rc.right - S(34), h.rc.bottom}, pf, cText,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            DeleteObject(pf);
            Text(dc, L"\uE70D", {h.rc.right - S(34), h.rc.top, h.rc.right - S(8), h.rc.bottom}, g.fGlyph, cDim,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            break;
        }
        {
        bool enabled = h.idx == 0 || !g.imgPath.empty();
        FillRound(dc, h.rc, S(10), hot && enabled ? cHover : cSearch);
        Text(dc, h.idx == 0 ? L"Bild wählen …" : L"Entfernen", h.rc, g.fSmall, enabled ? cText : RGB(95, 95, 105),
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        break;
    case HT::PlugCard: {  // Erweiterung: Beschreibung, Lizenz, Stand
        if (h.idx == 1) {  // Claude-Chat
            FillRound(dc, h.rc, S(18), cCard);
            RECT ic = {h.rc.left + S(18), h.rc.top + S(18), h.rc.left + S(18) + S(44), h.rc.top + S(18) + S(44)};
            FillRound(dc, ic, S(12), cCardHot);
            if (HBITMAP cb = ClaudeIcon(S(32))) {
                g.iconQueue.push_back({-3, ic.left + S(6), ic.top + S(6), S(32), CurrentClip(dc), g.drawFade, cb});
            } else {
                Text(dc, L"", ic, g.fGlyphL, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            int tx = ic.right + S(14);
            Text(dc, L"Claude – KI-Chat", {tx, h.rc.top + S(16), h.rc.right - S(18), h.rc.top + S(40)}, g.fHead, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            Text(dc, L"claude.ai direkt in der Leiste, schneller Chat ohne Browser.", {tx, h.rc.top + S(40), h.rc.right - S(18), h.rc.top + S(60)},
                 g.fSmall, cDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            Text(dc, L"Dafür brauchst du ein Konto bei claude.ai · nutzt WebView2 (bei Windows 11 dabei)",
                 {h.rc.left + S(18), h.rc.top + S(76), h.rc.right - S(18), h.rc.top + S(96)}, g.fSmall, cDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            Text(dc, L"Danach: „Claude“ links in der Leiste. Die Anmeldung bleibt gespeichert.",
                 {h.rc.left + S(18), h.rc.top + S(96), h.rc.right - S(18), h.rc.top + S(116)}, g.fSmall, cDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            Text(dc, g.claudeOn ? L"Hinzugefügt" : L"Nicht hinzugefügt", {h.rc.left + S(18), h.rc.bottom - S(50), h.rc.right - S(340), h.rc.bottom - S(14)}, g.fSmall,
                 g.claudeOn ? RGB(110, 210, 140) : cDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            break;
        }
        if (h.idx == 3) {  // Timer
            FillRound(dc, h.rc, S(18), cCard);
            RECT ic = {h.rc.left + S(18), h.rc.top + S(18), h.rc.left + S(18) + S(44), h.rc.top + S(18) + S(44)};
            FillRound(dc, ic, S(12), RGB(224, 138, 60));
            Text(dc, L"\uE916", ic, g.fGlyphL, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            int tx = ic.right + S(14);
            Text(dc, L"Timer", {tx, h.rc.top + S(16), h.rc.right - S(18), h.rc.top + S(40)}, g.fHead, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            Text(dc, L"Ziffernräder und eigener Klingelton.", {tx, h.rc.top + S(40), h.rc.right - S(18), h.rc.top + S(60)}, g.fSmall, cDim,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            Text(dc, g.timerOn ? L"Hinzugefügt" : L"Nicht hinzugefügt", {h.rc.left + S(18), h.rc.bottom - S(50), h.rc.right - S(340), h.rc.bottom - S(14)}, g.fSmall,
                 g.timerOn ? RGB(110, 210, 140) : cDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            break;
        }
        if (h.idx == 2) {  // Musik-Sync
            FillRound(dc, h.rc, S(18), cCard);
            RECT ic = {h.rc.left + S(18), h.rc.top + S(18), h.rc.left + S(18) + S(44), h.rc.top + S(18) + S(44)};
            FillRound(dc, ic, S(12), RGB(66, 120, 214));
            Text(dc, L"", ic, g.fGlyphL, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            int tx = ic.right + S(14);
            Text(dc, L"Musik-Sync – Musik aufs Handy", {tx, h.rc.top + S(16), h.rc.right - S(18), h.rc.top + S(40)}, g.fHead, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            Text(dc, L"Songs, Cover und Playlists in deiner Cloud, damit eine Handy-App sie spielen kann.", {tx, h.rc.top + S(40), h.rc.right - S(18), h.rc.top + S(60)},
                 g.fSmall, cDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            Text(dc, L"Mit jedem Cloud-Ordner: Google Drive, OneDrive, MEGA, pCloud, Koofr, Dropbox … (fehlende Dienste lassen sich hier laden)",
                 {h.rc.left + S(18), h.rc.top + S(76), h.rc.right - S(18), h.rc.top + S(96)}, g.fSmall, cDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            Text(dc, L"Playlists und Favoriten stehen in HoverBar-Bibliothek.json, Cover im Unterordner Cover.",
                 {h.rc.left + S(18), h.rc.top + S(96), h.rc.right - S(18), h.rc.top + S(116)}, g.fSmall, cDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SyncStatus ss = Music_SyncStatus();
            int cs;
            std::wstring cloudMsg;
            {
                std::lock_guard<std::mutex> lk(sCloudMu);
                cs = sCloudState;
                cloudMsg = sCloudMsg;
            }
            std::wstring st = cs == 1 ? cloudMsg
                            : ss.state == 1 ? L"Verschiebe Songs … " + std::to_wstring(ss.done) + L" / " + std::to_wstring(ss.total)
                            : ss.state == 3 ? ss.msg
                            : g.syncOn ? L"Aktiv: " + g.syncDir + (Music_SyncInfo().empty() ? L"" : L" · " + Music_SyncInfo())
                            : (cs == 2 || cs == 3) ? cloudMsg
                            : g.syncNote.empty() ? L"Nicht eingerichtet" : g.syncNote;
            bool bad = cs == 3 || ss.state == 3;
            Text(dc, st, {h.rc.left + S(18), h.rc.bottom - S(92), h.rc.right - S(18), h.rc.bottom - S(58)}, g.fSmall,
                 bad ? RGB(255, 150, 150) : (g.syncOn || cs == 2) ? RGB(110, 210, 140) : cDim, DT_LEFT | DT_VCENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
            break;
        }
        FillRound(dc, h.rc, S(18), cCard);
        RECT ic = {h.rc.left + S(18), h.rc.top + S(18), h.rc.left + S(18) + S(44), h.rc.top + S(18) + S(44)};
        FillRound(dc, ic, S(12), RGB(46, 160, 96));
        Text(dc, L"\uEDA2", ic, g.fGlyphL, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        int tx = ic.right + S(14);
        Text(dc, L"WizTree – Speicher-Analyse", {tx, h.rc.top + S(16), h.rc.right - S(18), h.rc.top + S(40)}, g.fHead, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        Text(dc, L"Zeigt als Baumansicht, welche Apps und Ordner wie viel Platz belegen.", {tx, h.rc.top + S(40), h.rc.right - S(18), h.rc.top + S(60)},
             g.fSmall, cDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        Text(dc, L"Kostenlos für private Nutzung · von diskanalyzer.com · portable, ohne Installation",
             {h.rc.left + S(18), h.rc.top + S(76), h.rc.right - S(18), h.rc.top + S(96)}, g.fSmall, cDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        Text(dc, L"Danach: \u201ESpeicher\u201C links in der Leiste, Rechtsklick in \u201EAlle Apps\u201C \u2192 In WizTree anzeigen",
             {h.rc.left + S(18), h.rc.top + S(96), h.rc.right - S(18), h.rc.top + S(116)}, g.fSmall, cDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        PlugStatus ps = WizTree_Status();
        std::wstring ver = WizTree_Version();
        std::wstring st = ps.state == PLUG_DOWNLOADING ? L"Lädt herunter … " + (ps.percent >= 0 ? std::to_wstring(ps.percent) + L" %" : L"")
                        : ps.state == PLUG_WORKING || ps.state == PLUG_ERROR || ps.state == PLUG_DONE ? ps.msg
                        : WizTree_Exe().empty() ? L"Nicht installiert" : L"Installiert";
        if (!ver.empty() && ps.state != PLUG_WORKING && ps.state != PLUG_DOWNLOADING) st += L" · Version " + ver;
        Text(dc, st, {h.rc.left + S(18), h.rc.bottom - S(50), h.rc.right - S(340), h.rc.bottom - S(14)}, g.fSmall,
             ps.state == PLUG_ERROR ? RGB(255, 150, 150) : !WizTree_Exe().empty() ? RGB(110, 210, 140) : cDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        break;
    }
    case HT::PlugBtn: {
        PlugStatus ps = WizTree_Status();
        bool busy = ps.state == PLUG_WORKING || ps.state == PLUG_DOWNLOADING;
        bool have = !WizTree_Exe().empty();
        if (h.idx == 20 || h.idx == 21 || h.idx == 22) {  // Musik-Sync
            SyncStatus ss = Music_SyncStatus();
            if (h.idx == 22) {
                FillRound(dc, h.rc, S(10), hot ? cHover : cSearch);
                Text(dc, L"Handy verbinden", h.rc, g.fSmall, cText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else if (h.idx == 21) {
                FillRound(dc, h.rc, S(10), hot ? RGB(200, 60, 60) : cSearch);
                Text(dc, L"Ausschalten", h.rc, g.fSmall, hot ? RGB(255, 255, 255) : RGB(255, 150, 150), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                FillRound(dc, h.rc, S(10), ss.state == 1 ? cSearch : hot ? RGB(120, 180, 255) : cAccent);
                Text(dc, ss.state == 1 ? L"Bitte warten …" : g.syncOn ? L"Jetzt abgleichen" : L"Einrichten …", h.rc, g.fHead, RGB(255, 255, 255),
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            break;
        }
        if (h.idx == 12 || h.idx == 13) {  // Timer
            FillRound(dc, h.rc, S(10), h.idx == 13 ? (hot ? RGB(200, 60, 60) : cSearch) : hot ? RGB(120, 180, 255) : cAccent);
            Text(dc, h.idx == 13 ? L"Entfernen" : g.timerOn ? L"Timer öffnen" : L"Hinzufügen", h.rc, g.fHead, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            break;
        }
        if (h.idx == 10 || h.idx == 11) {  // Claude-Chat
            if (h.idx == 11) {
                FillRound(dc, h.rc, S(10), hot ? RGB(200, 60, 60) : cSearch);
                Text(dc, L"Entfernen", h.rc, g.fSmall, hot ? RGB(255, 255, 255) : RGB(255, 150, 150), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                FillRound(dc, h.rc, S(10), hot ? RGB(120, 180, 255) : cAccent);
                Text(dc, g.claudeOn ? L"Claude öffnen" : L"Hinzufügen", h.rc, g.fHead, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            break;
        }
        if (h.idx == 1) {
            FillRound(dc, h.rc, S(10), hot ? RGB(200, 60, 60) : cSearch);
            Text(dc, L"Entfernen", h.rc, g.fSmall, hot ? RGB(255, 255, 255) : RGB(255, 150, 150), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            break;
        }
        FillRound(dc, h.rc, S(10), busy ? cSearch : hot ? RGB(120, 180, 255) : cAccent);
        if (ps.state == PLUG_DOWNLOADING && ps.percent >= 0) {
            RECT pr = h.rc;
            pr.right = pr.left + (pr.right - pr.left) * ps.percent / 100;
            if (pr.right > pr.left + S(8)) FillRound(dc, pr, S(10), cAccent);
        }
        Text(dc, busy ? L"Bitte warten …" : have ? L"WizTree öffnen" : L"Installieren", h.rc, g.fHead, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case HT::DriveRow: {  // Laufwerk: Name, Belegung als Balken, frei / gesamt
        wchar_t root[4] = {(wchar_t)(L'A' + h.idx), L':', L'\\', 0};
        ULARGE_INTEGER freeU = {}, total = {}, freeAll = {};
        GetDiskFreeSpaceExW(root, &freeU, &total, &freeAll);
        wchar_t label[MAX_PATH] = {};
        GetVolumeInformationW(root, label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);
        FillRound(dc, h.rc, S(16), hot ? cCardHot : cCard);
        RECT ic = {h.rc.left + S(14), h.rc.top + S(14), h.rc.left + S(14) + S(40), h.rc.top + S(14) + S(40)};
        Text(dc, L"\uEDA2", ic, g.fGlyphL, cText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        int tx = ic.right + S(12), bx1 = h.rc.right - S(16) - S(150) - S(16);
        std::wstring name = std::wstring(label[0] ? label : L"Lokaler Datenträger") + L" (" + root[0] + L":)";
        Text(dc, name, {tx, h.rc.top + S(10), bx1, h.rc.top + S(34)}, g.fHead, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        double t = total.QuadPart / 1073741824.0, fr = freeAll.QuadPart / 1073741824.0, used = t > 0 ? 1 - fr / t : 0;
        RECT bar = {tx, h.rc.top + S(40), bx1, h.rc.top + S(48)};
        FillRound(dc, bar, S(4), cSearch);
        FillRound(dc, {bar.left, bar.top, bar.left + std::max(S(8), (int)((bar.right - bar.left) * used)), bar.bottom}, S(4),
                  used > 0.9 ? RGB(230, 80, 80) : cAccent);  // fast voll: rot
        wchar_t txt[96];
        swprintf(txt, 96, L"%.0f GB frei von %.0f GB", fr, t);
        Text(dc, txt, {tx, h.rc.top + S(52), bx1, h.rc.bottom - S(6)}, g.fSmall, used > 0.9 ? RGB(255, 150, 150) : cDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case HT::DriveOpen:
        FillRound(dc, h.rc, S(10), hot ? RGB(120, 180, 255) : cAccent);
        Text(dc, L"In WizTree öffnen", h.rc, g.fSmall, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    case HT::ChatNew:
        FillRound(dc, h.rc, S(10), hot ? RGB(120, 180, 255) : cAccent);
        Text(dc, L"+  Neuer Chat", h.rc, g.fSmall, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    case HT::StorageRefresh:
        FillRound(dc, h.rc, S(10), hot ? cHover : cSearch);
        Text(dc, L"\uE72C", {h.rc.left + S(14), h.rc.top, h.rc.left + S(36), h.rc.bottom}, g.fGlyph, cText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        Text(dc, L"Aktualisieren", {h.rc.left + S(36), h.rc.top, h.rc.right - S(8), h.rc.bottom}, g.fSmall, cText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    case HT::StorageApps:
        FillRound(dc, h.rc, S(12), hot ? cHover : cSearch);
        Text(dc, L"Größte Apps anzeigen  \u203A", h.rc, g.fText, cText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    case HT::FolderBtn:
        FillRound(dc, h.rc, S(10), hot ? cHover : cSearch);
        Text(dc, L"Ordner wählen …", h.rc, g.fSmall, RGB(250, 250, 252), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    case HT::UpdateBtn: {  // HoverBar: Update suchen / installieren / neu starten
        UpdateStatus us = Update_Status();
        bool busy = us.state == UPD_CHECKING || us.state == UPD_DOWNLOADING || us.state == UPD_VERIFYING;
        bool go = us.state == UPD_AVAILABLE || us.state == UPD_READY;
        FillRound(dc, h.rc, S(10), busy ? cSearch : go ? (hot ? RGB(120, 180, 255) : cAccent) : hot ? cHover : cSearch);
        if (us.state == UPD_DOWNLOADING && us.percent >= 0) {
            RECT pr = h.rc;
            pr.right = pr.left + (pr.right - pr.left) * us.percent / 100;
            if (pr.right > pr.left + S(8)) FillRound(dc, pr, S(10), cAccent);
        }
        std::wstring t = us.state == UPD_CHECKING ? L"Suche …" : us.state == UPD_DOWNLOADING ? L"Lädt …" : us.state == UPD_VERIFYING ? L"Prüfe …"
                         : us.state == UPD_AVAILABLE ? L"Installieren (" + us.latest + L")" : us.state == UPD_READY ? L"Jetzt neu starten"
                                                                                                                  : L"Nach Update suchen";
        Text(dc, t, h.rc, g.fSmall, RGB(250, 250, 252), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case HT::ToolBtn: {  // Herunterladen / nach Update suchen
        ToolStatus ts = Music_ToolStatus();
        bool busy = ts.state == TOOL_CHECKING || ts.state == TOOL_DOWNLOADING || ts.state == TOOL_VERIFYING;
        bool have = Music_ToolAvailable();
        FillRound(dc, h.rc, S(10), busy ? cSearch : !have ? (hot ? RGB(120, 180, 255) : cAccent) : hot ? cHover : cSearch);
        if (busy && ts.state == TOOL_DOWNLOADING && ts.percent >= 0) {  // Fortschritt im Knopf
            RECT pr = h.rc;
            pr.right = pr.left + (pr.right - pr.left) * ts.percent / 100;
            if (pr.right > pr.left + S(8)) FillRound(dc, pr, S(10), cAccent);
        }
        const wchar_t* t = busy ? (ts.state == TOOL_DOWNLOADING ? L"Lädt …" : L"Bitte warten …") : have ? L"Nach Update suchen" : L"Herunterladen";
        Text(dc, t, h.rc, g.fSmall, RGB(250, 250, 252), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case HT::CalNav: {
        FillRound(dc, h.rc, S(10), hot ? cHover : cSearch);
        const wchar_t* t = h.idx == 0 ? L"\uE76B" : h.idx == 1 ? L"\uE76C" : L"Heute";
        Text(dc, t, h.rc, h.idx == 2 ? g.fSmall : g.fGlyph, cText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case HT::CalHead: {
        static const wchar_t* wd[7] = {L"Mo", L"Di", L"Mi", L"Do", L"Fr", L"Sa", L"So"};
        int colW = (h.rc.right - h.rc.left) / 7;
        for (int k = 0; k < 7; k++)
            Text(dc, wd[k], {h.rc.left + k * colW, h.rc.top, h.rc.left + (k + 1) * colW, h.rc.bottom}, g.fSmall, k >= 5 ? RGB(150, 160, 200) : cDim,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case HT::CalDay: {
        int d = h.idx % 100;
        bool other = h.f == 1, today = h.idx == Today(), sel = h.idx == g.calSel;
        int cx = (h.rc.left + h.rc.right) / 2, cy = h.rc.top + S(18), r = S(15);
        if (sel && !today) FillRound(dc, {cx - r, cy - r, cx + r, cy + r}, r, cHover);
        else if (hot && !today) FillRound(dc, {cx - r, cy - r, cx + r, cy + r}, r, cCardHot);
        if (today) FillCircle(dc, cx, cy, r, cAccent);
        Text(dc, std::to_wstring(d), {cx - r, cy - r, cx + r, cy + r}, sel || today ? g.fHead : g.fText,
             today ? RGB(255, 255, 255) : other ? RGB(110, 110, 125) : cText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        int ne = 0;
        for (auto& e : g.events) ne += e.date == h.idx;
        for (int k = 0; k < std::min(ne, 3); k++)  // Punkte für Termine
            FillCircle(dc, cx - (std::min(ne, 3) - 1) * S(4) + k * S(8), cy + r + S(5), S(2), other ? RGB(110, 130, 170) : RGB(120, 180, 255));
        break;
    }
    case HT::CalInfo: {
        int n = (int)EventsOn(h.idx).size();
        Text(dc, DateText(h.idx), {h.rc.left + S(4), h.rc.top, h.rc.right, h.rc.bottom}, g.fHead, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        Text(dc, n == 0 ? L"keine Termine" : n == 1 ? L"1 Termin" : std::to_wstring(n) + L" Termine", {h.rc.left, h.rc.top, h.rc.right - S(4), h.rc.bottom},
             g.fSmall, cDim, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case HT::CalEvent: {
        if (h.idx >= (int)g.events.size()) break;
        auto& e = g.events[h.idx];
        FillRound(dc, h.rc, S(12), hot ? cCardHot : cCard);
        FillRound(dc, {h.rc.left + S(10), h.rc.top + S(9), h.rc.left + S(14), h.rc.bottom - S(9)}, S(2), cAccent);
        Text(dc, e.time.empty() ? L"ganztägig" : e.time, {h.rc.left + S(22), h.rc.top, h.rc.left + S(100), h.rc.bottom}, g.fSmall,
             RGB(120, 180, 255), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        Text(dc, e.title, {h.rc.left + S(104), h.rc.top, h.rc.right - S(12), h.rc.bottom}, g.fText, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        break;
    }
    case HT::CalAdd:
        FillRound(dc, h.rc, S(12), hot ? cHover : cSearch);
        Text(dc, L"+  Termin hinzufügen", h.rc, g.fText, cText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    case HT::RecentCard:
        FillRound(dc, h.rc, S(18), cCard);
        Text(dc, L"Zuletzt genutzt", {h.rc.left + S(16), h.rc.top + S(6), h.rc.right - S(16), h.rc.top + S(30)}, g.fHead, cText,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        break;
    case HT::RecentItem: {
        if (h.idx >= (int)g.recent.size()) break;
        auto& e = g.recent[h.idx];
        int cw = h.rc.right - h.rc.left, isz = S(48), iy = h.rc.top + S(6), ix = h.rc.left + (cw - isz) / 2;
        if (hot) FillRound(dc, {h.rc.left + S(3), h.rc.top + S(1), h.rc.right - S(3), h.rc.bottom - S(1)}, S(14), cHover);
        std::wstring name;
        if (e.link) {
            std::wstring fav = FaviconFile(e.host);
            HBITMAP b = GetFileAttributesW(fav.c_str()) != INVALID_FILE_ATTRIBUTES ? CoverBmp(fav, isz) : nullptr;
            if (b) g.iconQueue.push_back({-3, ix, iy, isz, CurrentClip(dc), g.drawFade, b});
            else {
                FillRound(dc, {ix, iy, ix + isz, iy + isz}, S(14), cAccent);
                Text(dc, e.name.substr(0, 1), {ix, iy, ix + isz, iy + isz}, g.fTitle, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            name = e.name;
        } else if (e.app >= 0 && e.app < (int)g.apps.size()) {
            DrawAppIcon(dc, e.app, ix, iy, isz);
            name = g.apps[e.app].name;
        }
        Text(dc, name, {h.rc.left + S(5), iy + isz + S(5), h.rc.right - S(5), h.rc.bottom - S(3)}, g.fSmall, cText,
             DT_CENTER | DT_END_ELLIPSIS | DT_SINGLELINE);
        break;
    }
    case HT::LinkChip: {  // kleine rechteckige Kachel: Website-Symbol + Name
        if (h.idx >= (int)g.links.size()) break;
        auto& l = g.links[h.idx];
        FillRound(dc, h.rc, S(8), hot ? cHover : cCard);
        int isz = S(16), ix = h.rc.left + S(10), iy = (h.rc.top + h.rc.bottom - isz) / 2;
        std::wstring fav = FaviconFile(l.host);
        HBITMAP b = GetFileAttributesW(fav.c_str()) != INVALID_FILE_ATTRIBUTES ? CoverBmp(fav, isz) : nullptr;
        if (b) g.iconQueue.push_back({-3, ix, iy, isz, CurrentClip(dc), g.drawFade, b});
        else {
            FillRound(dc, {ix, iy, ix + isz, iy + isz}, S(4), cAccent);
            Text(dc, l.name.substr(0, 1), {ix, iy, ix + isz, iy + isz}, g.fSmall, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        Text(dc, l.name, {ix + isz + S(6), h.rc.top, h.rc.right - S(8), h.rc.bottom}, g.fSmall, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        break;
    }
    case HT::LinkAdd:
        FillRound(dc, h.rc, S(8), hot ? cHover : cSearch);
        Text(dc, L"\uE710", h.rc, g.fGlyph, cDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    case HT::Fav: {
        int cw = h.rc.right - h.rc.left, isz = S(48), iy = h.rc.top + S(6);
        int ix = h.rc.left + (cw - isz) / 2;
        if (h.idx >= 0) {
            if (hot) FillRound(dc, {h.rc.left + S(3), h.rc.top + S(1), h.rc.right - S(3), h.rc.bottom - S(1)}, S(14), cHover);
            DrawAppIcon(dc, h.idx, ix, iy, isz);
            Text(dc, g.apps[h.idx].name, {h.rc.left + S(5), iy + isz + S(5), h.rc.right - S(5), h.rc.bottom - S(3)}, g.fSmall,
                 cText, DT_CENTER | DT_END_ELLIPSIS | DT_SINGLELINE);
        } else {
            FillRound(dc, {ix, iy, ix + isz, iy + isz}, S(16), hot ? cHover : cSearch);
            Text(dc, L"\uE710", {ix, iy, ix + isz, iy + isz}, g.fGlyph, cDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            Text(dc, L"frei", {h.rc.left + S(5), iy + isz + S(5), h.rc.right - S(5), h.rc.bottom - S(3)}, g.fSmall, cDim,
                 DT_CENTER | DT_SINGLELINE);
        }
        break;
    }
    case HT::FavSlot: {
        bool target = g.dragging && g.dropF == -3 && g.dropPos == h.pos;
        bool dragged = g.dragging && g.dragFromFav == h.pos;
        FillRound(dc, h.rc, S(16), target ? cHover : (h.idx >= 0 && !dragged) ? cCardHot : cSearch);
        int side = h.rc.right - h.rc.left;
        if (h.idx >= 0 && !dragged) {
            int isz = std::min(S(48), side - S(30));
            DrawAppIcon(dc, h.idx, h.rc.left + (side - isz) / 2, h.rc.top + S(8), isz);
            Text(dc, g.apps[h.idx].name, {h.rc.left + S(4), h.rc.top + S(8) + isz + S(3), h.rc.right - S(4), h.rc.bottom - S(2)},
                 g.fSmall, cText, DT_CENTER | DT_END_ELLIPSIS | DT_SINGLELINE);
        } else {
            Text(dc, std::to_wstring(h.pos + 1), h.rc, g.fTitle, target ? cText : RGB(110, 110, 122), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        break;
    }
    case HT::MusicBar: {
        {   // deckende Fläche in der gewählten Farbe (leicht aufgehellt), nicht durchsichtig
            int r = GetRValue(g.tint), gg = GetGValue(g.tint), b = GetBValue(g.tint);
            COLORREF solid = RGB(r + (255 - r) * 14 / 100, gg + (255 - gg) * 14 / 100, b + (255 - b) * 14 / 100);
            if (solid == cCard || solid == cCardHot || solid == cHover || solid == cSearch) solid = RGB(r + 1, gg, b);
            FillRound(dc, h.rc, S(22), solid);
        }
        int ic = S(kBarArt);
        RECT ir = {h.rc.left + S(14), h.rc.top + S(14), h.rc.left + S(14) + ic, h.rc.top + S(14) + ic};  // großes Cover
        int textL = ir.right + S(16);
        // Titel und Künstler oben mittig (links Platz für den Bibliothek-Knopf, rechts gleich viel für die Symmetrie)
        RECT t1 = {textL + S(44), h.rc.top + S(10), h.rc.right - S(14) - S(44), h.rc.top + S(33)};
        RECT t2 = {textL + S(44), h.rc.top + S(33), h.rc.right - S(14) - S(44), h.rc.top + S(51)};
        const UINT ctr = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
        if (g.pst.active) {
            int si = Music_Find(g.pst.songId);
            const Song* s = SongAt(si);
            DrawCover(dc, s ? s->cover : L"", ir.left, ir.top, ic);
            Text(dc, s ? s->title : L"", t1, g.fBarTitle, cText, ctr);
            Text(dc, s ? s->artist : L"", t2, g.fText, cDim, ctr);
            // Fortschritt: das Hauptelement, über die ganze Breite
            RECT track = {textL, h.rc.top + S(62), h.rc.right - S(14), h.rc.top + S(68)};
            bool hotBar = g.hot >= 0 && g.hot < (int)g.hits.size() && g.hits[g.hot].type == HT::MProgress;
            if (hotBar) { track.top -= S(1); track.bottom += S(1); }
            FillRound(dc, track, S(4), cCardHot);
            if (g.pst.dur > 0) {
                int fw = (int)((track.right - track.left) * std::min(1.0, g.pst.pos / g.pst.dur));
                FillRound(dc, {track.left, track.top, track.left + std::max(S(6), fw), track.bottom}, S(4), cAccent);
                if (hotBar) FillCircle(dc, track.left + fw, (track.top + track.bottom) / 2, S(7), RGB(250, 250, 252));
            }
            // Zeit unten rechts (unten links sitzt die Lautstärke, mittig die Knöpfe)
            RECT tr = {h.rc.right - S(14) - S(110), h.rc.top + S(92), h.rc.right - S(14), h.rc.top + S(114)};
            Text(dc, FmtTime(g.pst.pos) + L" / " + (g.pst.dur > 0 ? FmtTime(g.pst.dur) : std::wstring(L"–")), tr, g.fSmall, cDim,
                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            break;
        }
        bool sp = SpotifyMode();
        if (sp && g.media.has && g.mediaArt) {
            g.iconQueue.push_back({-2, ir.left, ir.top, ic, CurrentClip(dc), g.drawFade});  // Spotify-Albumbild
        } else {
            FillRound(dc, ir, S(16), cCardHot);
            Text(dc, L"\uE8D6", ir, g.fGlyph, cDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        bool any = !Music_Songs().empty();
        std::wstring l1 = sp ? (g.media.has && !g.media.title.empty() ? g.media.title : std::wstring(L"Spotify")) : L"Nichts läuft";
        std::wstring l2 = any ? L"Play drücken" : L"Noch keine Songs – über den Musik-Knopf hinzufügen";
        if (sp) l2 = g.media.has ? (g.media.artist.empty() ? std::wstring(L"Spotify") : g.media.artist) : L"Nicht geöffnet · Play startet Spotify";
        Text(dc, l1, t1, g.fBarTitle, cText, ctr);
        Text(dc, l2, t2, g.fText, cDim, ctr);
        RECT track = {textL, h.rc.top + S(63), h.rc.right - S(14), h.rc.top + S(67)};  // leerer Fortschritt
        FillRound(dc, track, S(4), cCardHot);
        break;
    }
    case HT::MediaPrev:
    case HT::MediaNext:
    case HT::MediaPlay:
    case HT::MediaPhone: {
        // ohne Windows-Sitzung (z. B. Handy-Musik über Bluetooth) senden die Knöpfe Medientasten -> immer aktiv
        bool spotify = !g.pst.active && SpotifyMode();
        bool enabled = g.pst.active || (h.type == HT::MediaPlay && (spotify || !Music_Songs().empty())) ||
                       (spotify && g.media.has && (h.type == HT::MediaPrev ? g.media.canPrev : g.media.canNext));
        if (h.type == HT::MediaPlay) {
            int r = (h.rc.right - h.rc.left) / 2;
            FillCircle(dc, (h.rc.left + h.rc.right) / 2, (h.rc.top + h.rc.bottom) / 2, r - S(1),
                       enabled ? (hot ? RGB(255, 255, 255) : RGB(235, 235, 240)) : RGB(80, 80, 90));
            // ohne Sitzung ist der Zustand unbekannt: Play-Symbol (wirkt als Play/Pause)
            Text(dc, (g.pst.active ? g.pst.playing : spotify && g.media.playing) ? L"\uE769" : L"\uE768", h.rc, g.fGlyph, RGB(20, 20, 26),
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            break;
        }
        if (hot && enabled) FillRound(dc, h.rc, h.rc.bottom - h.rc.top, cCardHot);
        const wchar_t* gl = h.type == HT::MediaPrev ? L"\uE892" : h.type == HT::MediaNext ? L"\uE893" : L"\uE8EA";
        COLORREF col = !enabled ? RGB(85, 85, 95) : (h.type == HT::MediaPhone && g.phoneConnected) ? RGB(120, 180, 255) : cText;
        Text(dc, gl, h.rc, g.fGlyph, col, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (h.type == HT::MediaPhone && g.phoneConnected) FillCircle(dc, h.rc.right - S(9), h.rc.top + S(9), S(3), cAccent);
        break;
    }
    // ---------- Musik
    case HT::MusicJob: {
        if (h.idx >= (int)g.jobsView.size()) break;
        const DownloadJob& j = g.jobsView[h.idx];
        FillRound(dc, h.rc, S(14), cCard);
        RECT t1 = {h.rc.left + S(14), h.rc.top + S(4), h.rc.right - S(70), h.rc.top + S(28)};
        Text(dc, j.title, t1, g.fText, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        static const wchar_t* st[] = {L"Wartet …", L"Lädt herunter", L"Wandelt in MP3 um", L"Sucht Cover", L"Fertig"};
        std::wstring s = j.stage < 0 ? L"Fehler: " + j.error : st[std::min(4, j.stage)];
        RECT sr = {h.rc.right - S(200), h.rc.top + S(4), h.rc.right - S(14), h.rc.top + S(28)};
        Text(dc, j.stage == 1 || j.stage == 2 ? std::to_wstring(j.percent) + L" %" : L"", sr, g.fSmall, cDim, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        int barY = h.rc.top + S(32);
        RECT track = {h.rc.left + S(14), barY, h.rc.right - S(14), barY + S(4)};
        if (j.stage >= 1 && j.stage <= 3) {
            FillRound(dc, track, S(4), cSearch);
            float f = j.stage == 1 ? j.percent / 200.f : j.stage == 2 ? 0.5f + j.percent / 250.f : 0.95f;
            FillRound(dc, {track.left, track.top, track.left + std::max(S(4), (int)((track.right - track.left) * f)), track.bottom}, S(4), cAccent);
        }
        RECT lr = {h.rc.left + S(14), barY + S(4), h.rc.right - S(14), h.rc.bottom - S(2)};
        Text(dc, s, lr, g.fSmall, j.stage < 0 ? RGB(255, 140, 140) : cDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        break;
    }
    case HT::MusicEmpty: {
        FillRound(dc, h.rc, S(18), cCard);
        RECT a = {h.rc.left + S(16), h.rc.top + S(14), h.rc.right - S(16), h.rc.top + S(40)};
        RECT b = {h.rc.left + S(16), h.rc.top + S(42), h.rc.right - S(16), h.rc.bottom - S(10)};
        Text(dc, L"Noch keine Songs", a, g.fHead, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        Text(dc, Music_ToolAvailable() ? L"Füge oben einen YouTube-Link ein und drück Enter. Der Song wird als MP3 gespeichert und bekommt automatisch ein Cover."
                                       : L"yt-dlp fehlt noch. Füge oben einen Link ein und drück Enter: HoverBar fragt dann, ob es yt-dlp herunterladen soll (oder Zahnrad → Funktionen → yt-dlp).",
             b, g.fSmall, cDim, DT_LEFT | DT_WORDBREAK);
        break;
    }
    case HT::MCard: {
        FillRound(dc, h.rc, S(20), hot ? cCardHot : cCard);
        int ip = S(14);
        RECT nr = {h.rc.left + ip + S(4), h.rc.top + ip - S(4), h.rc.right - ip - S(110), h.rc.top + ip + S(26)};
        Text(dc, PlaylistName(h.idx), nr, g.fHead, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        size_t n = MusicListItems(h.idx).size();
        RECT cr2 = {h.rc.right - ip - S(130), nr.top, h.rc.right - ip - S(4), nr.bottom};
        Text(dc, std::to_wstring(n) + (n == 1 ? L" Song  ›" : L" Songs  ›"), cr2, g.fSmall, cDim, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        if (!n) {
            RECT er = {h.rc.left + ip + S(4), nr.bottom + S(4), h.rc.right - ip, h.rc.bottom - S(6)};
            Text(dc, L"Leer – Songs in den Einstellungen (Zahnrad → Musik) hinzufügen", er, g.fSmall, cDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        break;
    }
    case HT::MTile: {
        const Song* s = SongAt(h.idx);
        if (!s) break;
        bool playing = g.pst.active && g.pst.songId == s->id;
        if (hot || playing) FillRound(dc, {h.rc.left + S(3), h.rc.top + S(1), h.rc.right - S(3), h.rc.bottom - S(1)}, S(14), hot ? cHover : cCardHot);
        int cw = h.rc.right - h.rc.left, isz = S(48), iy = h.rc.top + S(6);
        DrawCover(dc, s->cover, h.rc.left + (cw - isz) / 2, iy, isz);
        Text(dc, s->title, {h.rc.left + S(5), iy + isz + S(5), h.rc.right - S(5), h.rc.bottom - S(3)}, g.fSmall,
             playing ? RGB(140, 190, 255) : cText, DT_CENTER | DT_END_ELLIPSIS | DT_SINGLELINE);
        break;
    }
    case HT::MMore: {
        if (hot) FillRound(dc, {h.rc.left + S(3), h.rc.top + S(1), h.rc.right - S(3), h.rc.bottom - S(1)}, S(14), cHover);
        int cw = h.rc.right - h.rc.left, isz = S(48), iy = h.rc.top + S(6);
        RECT b = {h.rc.left + (cw - isz) / 2, iy, h.rc.left + (cw + isz) / 2, iy + isz};
        FillRound(dc, b, S(16), cCardHot);
        Text(dc, L"+" + std::to_wstring(h.n), b, g.fHead, cText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        Text(dc, L"alle anzeigen", {h.rc.left + S(4), iy + isz + S(5), h.rc.right - S(4), h.rc.bottom - S(3)}, g.fSmall, cDim, DT_CENTER | DT_SINGLELINE);
        break;
    }
    case HT::MReload:
        FillRound(dc, h.rc, S(22), hot ? cCardHot : cCard);
        Text(dc, L"\uE72C", h.rc, g.fGlyph, cText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (g.reload) {  // Balken darunter: läuft durch, solange noch Dateien ankommen; voll = alles eingelesen
            RECT bar = {h.rc.left + S(2), h.rc.bottom + S(3), h.rc.right - S(2), h.rc.bottom + S(7)};
            FillRound(dc, bar, S(4), cSearch);
            RECT fill = bar;
            if (g.reload == 1) {
                int w = bar.right - bar.left, seg = w / 2, x = bar.left - seg + (g.reloadTick * w / 10) % (w + seg);
                fill.left = std::max((int)bar.left, x);
                fill.right = std::min((int)bar.right, x + seg);
            }
            if (fill.right > fill.left) FillRound(dc, fill, S(4), cAccent);
        }
        break;
    case HT::MSort:
        FillRound(dc, h.rc, S(22), hot ? cCardHot : cCard);
        Text(dc, g.sortPlays ? L"Meistgespielt" : L"Nach Titel", h.rc, g.fText, cText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    case HT::MPlayAll: {
        FillRound(dc, h.rc, S(22), hot ? RGB(120, 180, 255) : cAccent);
        {   // Play-Symbol (Symbolschrift) + Text, zusammen mittig
            int cx = (h.rc.left + h.rc.right) / 2;
            Text(dc, L"", {cx - S(78), h.rc.top, cx - S(54), h.rc.bottom}, g.fGlyph, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            Text(dc, L"Alle abspielen", {cx - S(48), h.rc.top, h.rc.right - S(8), h.rc.bottom}, g.fHead, RGB(255, 255, 255), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        break;
    }
    case HT::MRow:
    case HT::PlSongRow:
    case HT::AllSongRow: {
        const Song* s = SongAt(h.idx);
        if (!s) break;
        bool playing = g.pst.active && g.pst.songId == s->id;
        bool dragged = g.dragging && g.dragMusic && h.idx == g.dragApp && (h.type != HT::PlSongRow || h.pos == g.dragFromPos);
        if (dragged) FillRound(dc, h.rc, S(10), cCardHot);
        else if (hot && !g.dragging) FillRound(dc, h.rc, S(10), cHover);
        if (g.songLines && h.type == HT::MRow) {  // kaum sichtbar, in der Lücke unter der Zeile
            float fade = g.drawFade;
            g.drawFade = fade * 0.09f;
            FillRound(dc, {h.rc.left + S(12), h.rc.bottom + S(1), h.rc.right - S(12), h.rc.bottom + S(1) + std::max(1, S(1))}, 0, RGB(255, 255, 255));
            g.drawFade = fade;
        }
        int rh = h.rc.bottom - h.rc.top, isz = h.type == HT::MRow ? rh - S(12) : S(32);
        DrawCover(dc, s->cover, h.rc.left + S(8), h.rc.top + (rh - isz) / 2, isz);
        int tx = h.rc.left + S(8) + isz + S(12);
        int right = h.type == HT::MRow ? h.rc.right - S(90) : h.type == HT::AllSongRow ? h.rc.right - S(72) : h.rc.right - S(8);
        RECT n1 = {tx, h.rc.top + S(2), right, (h.rc.top + h.rc.bottom) / 2 + S(2)};
        RECT n2 = {tx, (h.rc.top + h.rc.bottom) / 2 + S(2), right, h.rc.bottom - S(2)};
        Text(dc, s->title, n1, h.type == HT::MRow ? g.fHead : g.fText, playing ? RGB(140, 190, 255) : dragged ? cDim : cText,
             DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
        std::wstring sub = s->artist;
        if (h.type == HT::AllSongRow) {
            int inPl = 0;
            for (auto& v : g.plItems)
                for (int i : v)
                    if (i == h.idx) { inPl++; break; }
            sub += (sub.empty() ? L"" : L" · ") + (inPl ? std::to_wstring(inPl) + (inPl == 1 ? L" Playlist" : L" Playlists") : L"in keiner Playlist");
        }
        Text(dc, sub, n2, g.fSmall, cDim, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (h.type == HT::MRow && s->duration > 0)
            Text(dc, FmtTime(s->duration), {h.rc.right - S(60), h.rc.top, h.rc.right - S(10), h.rc.bottom}, g.fSmall, cDim, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        if (h.type == HT::MRow && IsFav(s->id))  // kleines Herz bei Favoriten
            Text(dc, L"", {h.rc.right - S(88), h.rc.top, h.rc.right - S(60), h.rc.bottom}, g.fGlyph, RGB(255, 90, 120), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case HT::AllSongAdd:
    case HT::AllSongEdit:
        if (hot) FillRound(dc, h.rc, h.rc.bottom - h.rc.top, cHover);
        Text(dc, h.type == HT::AllSongAdd ? L"" : L"", h.rc, g.fGlyph, hot ? cText : cDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    case HT::PlHead: {
        bool target = g.dragging && g.dragMusic && g.dropF == h.f;
        RECT nr = {h.rc.left + S(12), h.rc.top, h.rc.right - S(96), h.rc.bottom};
        const std::wstring& name = Music_Playlists()[h.f].name;
        SelectObject(dc, g.fHead);
        SIZE ext = {};
        GetTextExtentPoint32W(dc, name.c_str(), (int)name.size(), &ext);
        Text(dc, name, nr, g.fHead, target ? RGB(140, 190, 255) : cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        Text(dc, std::to_wstring(h.n), {nr.left + ext.cx + S(8), nr.top, nr.right, nr.bottom}, g.fSmall, cDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case HT::PlUp:
    case HT::PlDown:
    case HT::PlDelete: {
        bool up = h.type == HT::PlUp, down = h.type == HT::PlDown;
        bool enabled = !(up && h.pos <= 0) && !(down && h.pos >= h.n - 1);
        DrawGlyphButton(dc, h.rc, up ? L"" : down ? L"" : L"", hot, enabled);
        break;
    }
    case HT::PlDrop: {
        bool target = g.dragging && g.dragMusic && g.dropF == h.f;
        FillRound(dc, h.rc, S(10), target ? cHover : cSearch);
        Text(dc, L"Leer – Songs hierher ziehen", h.rc, g.fSmall, cDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case HT::MAlbum: {
        FillRound(dc, h.rc, S(20), hot ? cCardHot : cCard);
        const auto& items = MusicListItems(h.idx);
        int big = (h.rc.bottom - h.rc.top) - S(16);
        std::wstring cover;
        for (int i : items)
            if (!Music_Songs()[i].cover.empty()) { cover = Music_Songs()[i].cover; break; }
        DrawCover(dc, cover, h.rc.left + S(8), h.rc.top + S(8), big);
        int tx = h.rc.left + S(8) + big + S(18);
        RECT n1 = {tx, h.rc.top + S(8), h.rc.right - S(16), (h.rc.top + h.rc.bottom) / 2 + S(2)};
        RECT n2 = {tx, (h.rc.top + h.rc.bottom) / 2 + S(4), h.rc.right - S(16), h.rc.bottom - S(8)};
        Text(dc, PlaylistName(h.idx), n1, g.musicSize == 2 ? g.fTitle : g.fHead, cText, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
        int total = 0;
        for (int i : items) total += Music_Songs()[i].duration;
        std::wstring sub = std::to_wstring(items.size()) + (items.size() == 1 ? L" Song" : L" Songs") +
                           (total > 0 ? L"  ·  " + std::to_wstring((total + 59) / 60) + L" Min." : L"") + (h.idx < 0 ? L"" : L"  ·  Playlist");
        Text(dc, sub, n2, g.musicSize == 2 ? g.fText : g.fSmall, cDim, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        Text(dc, L"\uE76C", {h.rc.right - S(40), h.rc.top, h.rc.right - S(12), h.rc.bottom}, g.fGlyph, cDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case HT::BarVol: {  // Lautstärke in der Musikleiste (ohne Prozentzahl)
        int cy = (h.rc.top + h.rc.bottom) / 2, kr = S(5);
        int x0 = h.rc.left + S(9), x1 = h.rc.right - S(9);
        int kx = x0 + (x1 - x0) * g.volume / 100;
        FillRound(dc, {x0, cy - S(1), x1, cy + S(2)}, S(2), cCardHot);  // dünn
        FillRound(dc, {x0, cy - S(1), std::max(x0 + S(3), kx), cy + S(2)}, S(2), RGB(200, 202, 210));
        FillCircle(dc, kx, cy, (hot || g.sliderDrag == 3) ? kr + S(1) : kr, RGB(250, 250, 252));
        break;
    }
    case HT::VolIcon:
        if (hot) FillRound(dc, h.rc, h.rc.bottom - h.rc.top, cHover);
        Text(dc, g.volume == 0 ? L"\uE74F" : g.volume < 34 ? L"\uE993" : g.volume < 67 ? L"\uE994" : L"\uE995", h.rc, g.fGlyph, cDim,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    case HT::MSize: {
        static const wchar_t* names[] = {L"Klein", L"Mittel", L"Groß"};
        bool sel = g.musicSize == h.idx;
        RECT r = {h.rc.left + S(2), h.rc.top, h.rc.right - S(2), h.rc.bottom};
        FillRound(dc, r, S(10), sel ? cHover : hot ? cCardHot : cSearch);
        Text(dc, names[h.idx], r, sel ? g.fHead : g.fSmall, sel ? cText : cDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case HT::MusicManage:
        if (hot) FillRound(dc, h.rc, S(10), cHover);
        Text(dc, L"\uE713", h.rc, g.fGlyph, cText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    case HT::PlayMode: {  // Zufall / Wiederholen: Symbol zeigt den Modus, blau wenn etwas aktiv ist
        bool active = g.shuffle || g.repeat;
        if (hot) FillRound(dc, h.rc, S(14), cCardHot);
        const wchar_t* gl = g.repeat == 1 ? L"\uE8ED" : g.shuffle ? L"\uE8B1" : g.repeat == 2 ? L"\uE8EE" : L"\uE8B1";
        Text(dc, gl, h.rc, g.fGlyphL, active ? RGB(120, 180, 255) : cDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (g.shuffle && g.repeat == 2) FillCircle(dc, h.rc.right - S(7), h.rc.top + S(7), S(3), cAccent);  // zusätzlich Playlist wdh.
        break;
    }
    // ---------- Läuft gerade
    case HT::BarCover:
        if (hot) FillRound(dc, {h.rc.right - S(30), h.rc.top + S(4), h.rc.right - S(4), h.rc.top + S(30)}, S(13), RGB(20, 20, 26));
        if (hot) Text(dc, L"", {h.rc.right - S(30), h.rc.top + S(4), h.rc.right - S(4), h.rc.top + S(30)}, g.fSmall, cText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    case HT::NpCover: {
        int cs = h.rc.right - h.rc.left;
        const Song* s = g.pst.active ? SongAt(Music_Find(g.pst.songId)) : nullptr;
        bool sp = !g.pst.active && SpotifyMode();
        if (s) DrawCover(dc, s->cover, h.rc.left, h.rc.top, cs);
        else if (sp && g.media.has && g.mediaArt) g.iconQueue.push_back({-2, h.rc.left, h.rc.top, cs, CurrentClip(dc), g.drawFade});
        else {
            FillRound(dc, h.rc, S(28), cCardHot);
            Text(dc, L"", h.rc, g.fGlyphL, cDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        std::wstring l1 = s ? s->title : sp && g.media.has ? g.media.title : L"Nichts läuft";
        std::wstring l2 = s ? s->artist : sp && g.media.has ? g.media.artist : L"Starte einen Song in der Bibliothek";
        RECT t1 = {sNpL, h.rc.bottom + S(16), sNpR, h.rc.bottom + S(48)};
        RECT t2 = {t1.left, t1.bottom + S(2), t1.right, t1.bottom + S(26)};
        Text(dc, l1, t1, g.fNpTitle, cText, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        Text(dc, l2, t2, g.fText, cDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        break;
    }
    case HT::MProgress: {  // nur in "Läuft gerade" (in der Musikleiste zeichnet die Leiste selbst)
        if (g.view != View::NowPlaying) break;
        int cy = (h.rc.top + h.rc.bottom) / 2;
        RECT track = {h.rc.left, cy - S(3), h.rc.right, cy + S(3)};
        if (hot) { track.top -= S(1); track.bottom += S(1); }
        FillRound(dc, track, S(4), cCardHot);
        if (g.pst.dur > 0) {
            int fw = (int)((track.right - track.left) * std::min(1.0, g.pst.pos / g.pst.dur));
            FillRound(dc, {track.left, track.top, track.left + std::max(S(6), fw), track.bottom}, S(4), cAccent);
            FillCircle(dc, track.left + fw, cy, hot ? S(8) : S(6), RGB(250, 250, 252));
        }
        RECT tl = {h.rc.left, h.rc.bottom + S(2), h.rc.left + S(80), h.rc.bottom + S(22)};
        RECT tr = {h.rc.right - S(80), tl.top, h.rc.right, tl.bottom};
        Text(dc, FmtTime(g.pst.pos), tl, g.fSmall, cDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        Text(dc, g.pst.dur > 0 ? FmtTime(g.pst.dur) : std::wstring(L"–"), tr, g.fSmall, cDim, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case HT::NpLyrics: {
        g.lyrRects.clear();
        if (!g.pst.active) break;
        RECT r = h.rc;
        if (g.lyr.empty()) {
            RECT a = {r.left, r.top + S(20), r.right, r.top + S(50)};
            Text(dc, g.lyrState == 1 ? L"Songtext wird gesucht …" : L"Kein Songtext gefunden", a, g.fText, cDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if (g.lyrState == 2) {
                RECT b2 = {r.left + S(20), a.bottom, r.right - S(20), a.bottom + S(40)};
                Text(dc, L"Tipp: eine .lrc-Datei mit gleichem Namen neben die MP3 legen", b2, g.fSmall, cDim, DT_CENTER | DT_WORDBREAK);
            }
            break;
        }
        int lw = r.right - r.left - S(40);
        if (g.lyrW != lw || g.lyrH.size() != g.lyr.size()) {  // Zeilenhöhen (mit Umbruch) einmal messen
            g.lyrW = lw;
            g.lyrH.clear();
            HDC sdc = GetDC(nullptr);
            HGDIOBJ of = SelectObject(sdc, g.fLyric);
            for (auto& l : g.lyr) {
                RECT m = {0, 0, lw, 0};
                std::wstring t = l.text.empty() ? L"♪" : l.text;
                DrawTextW(sdc, t.c_str(), -1, &m, DT_CALCRECT | DT_WORDBREAK | DT_CENTER | DT_NOPREFIX);
                g.lyrH.push_back(std::max<int>(m.bottom, S(22)));
            }
            SelectObject(sdc, of);
            ReleaseDC(nullptr, sdc);
        }
        int gap = S(12), n = (int)g.lyr.size();
        std::vector<int> tops(n);
        int acc = 0;
        for (int i = 0; i < n; i++) { tops[i] = acc; acc += g.lyrH[i] + gap; }
        int viewH = r.bottom - r.top;
        int cur = LyricIndex(g.pst.pos);
        double target;
        if (g.lyrSynced) {  // aktuelle Zeile etwas über der Mitte halten
            int k = std::max(0, cur);
            target = tops[k] + g.lyrH[k] / 2.0 - viewH * 0.36;
        } else {            // ohne Zeitstempel: gleichmäßig mit dem Fortschritt mitlaufen
            target = g.pst.dur > 0 ? (acc - viewH * 0.6) * std::min(1.0, g.pst.pos / g.pst.dur) - viewH * 0.2 : 0;
        }
        target = std::max(target, -(double)viewH * 0.36 + (g.lyrSynced ? g.lyrH[0] / 2.0 : 0));
        if (!g.lyrSynced) target = std::max(0.0, target);
        g.lyrTotal = acc;
        g.lyrViewH = viewH;
        if (g.lyrManual) target = g.lyrUser;  // von Hand gescrollt: dort bleiben (2 s nach dem letzten Drehen geht es zurück)
        g.lyrTarget = target;
        if (std::fabs(g.lyrTarget - g.lyrScroll) > 0.5 && !g.lyrAnim) { g.lyrAnim = true; SetTimer(g.hwnd, T_LYR, 15, nullptr); }
        // beim Suchen: die Zeile auf Höhe der Lesemarke hervorheben (dorthin springt ein Klick bequem)
        int pick = -1;
        if (g.lyrManual && g.lyrSynced) {
            double mark = g.lyrScroll + viewH * 0.36, best = 1e9;
            for (int i = 0; i < n; i++) {
                double d = std::fabs(tops[i] + g.lyrH[i] / 2.0 - mark);
                if (d < best) { best = d; pick = i; }
            }
        }
        SaveDC(dc);
        IntersectClipRect(dc, r.left, r.top, r.right, r.bottom);
        int fadeZone = S(56);
        for (int i = 0; i < n; i++) {
            int y = r.top + tops[i] - (int)g.lyrScroll;
            if (y + g.lyrH[i] < r.top || y > r.bottom) continue;
            RECT lr = {r.left + S(20), y, r.right - S(20), y + g.lyrH[i]};
            int mid = y + g.lyrH[i] / 2;
            float edge = std::min(1.f, std::max(0.f, std::min(mid - r.top, r.bottom - mid) / (float)fadeZone));
            int ref = pick >= 0 ? pick : cur;
            float f = !g.lyrSynced ? 0.85f : i == ref ? 1.f : std::max(0.3f, 0.6f - 0.07f * std::abs(i - ref));
            bool over = hot && g.lyrSynced && PtInRect(&lr, g.mousePt);
            if (over) f = 1.f;
            g.drawFade = f * (0.15f + 0.85f * edge) * g.npAlpha;
            if (over) {  // Zeile unter der Maus: Fläche + Zeitpunkt, Klick springt dorthin
                FillRound(dc, {lr.left - S(8), lr.top - S(5), lr.right + S(8), lr.bottom + S(5)}, S(12), cHover);
                Text(dc, FmtTime(g.lyr[i].t), {lr.right - S(56), lr.top, lr.right, lr.top + S(24)}, g.fSmall, cDim, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            }
            bool now = g.lyrSynced && i == cur;
            COLORREF col = i == pick && !now ? RGB(140, 190, 255) : now ? RGB(255, 255, 255) : RGB(190, 192, 204);
            Text(dc, g.lyr[i].text.empty() ? L"♪" : g.lyr[i].text, lr, g.fLyric, col, DT_CENTER | DT_WORDBREAK);
            if (g.lyrSynced) g.lyrRects.push_back({lr, g.lyr[i].t});
        }
        g.drawFade = 1.f;
        RestoreDC(dc, -1);
        break;
    }
    case HT::LibTab: {  // Bibliothek: Favoriten / Playlists / Zuletzt gespielt
        bool sel = g.libTab == h.idx;
        FillRound(dc, h.rc, S(16), sel ? cAccent : hot ? cCardHot : cCard);
        static const wchar_t* gl[] = {L"", L"", L"", L""};
        static const wchar_t* nm[] = {L"", L"Favoriten", L"Playlists", L"Zuletzt"};
        size_t n = h.idx == 1 ? g.mFav.size() : h.idx == 2 ? Music_Playlists().size() : g.mRecent.size();
        COLORREF ic = sel ? RGB(255, 255, 255) : h.idx == 1 ? RGB(255, 100, 130) : cText;
        int hh = h.rc.bottom - h.rc.top;
        if (g.tileCovers) {  // blass rechts: zuletzt hinzugefügter Favorit / neueste Playlist / zuletzt gehört
            const Song* c = nullptr;
            if (h.idx == 1 && !g.mFav.empty()) c = SongAt(g.mFav.front());
            else if (h.idx == 2 && !g.plItems.empty() && !g.plItems.back().empty()) c = SongAt(g.plItems.back().back());
            else if (h.idx == 3 && !g.mRecent.empty()) c = SongAt(g.mRecent.front());
            if (c && !c->cover.empty()) {
                float fade = g.drawFade;
                g.drawFade = fade * 0.35f;
                DrawCover(dc, c->cover, h.rc.right - hh, h.rc.top, hh);
                g.drawFade = fade;
            }
        }
        RECT gr = {h.rc.left + S(10), h.rc.top, h.rc.left + S(10) + S(30), h.rc.bottom};
        Text(dc, gl[h.idx], gr, g.fGlyph, ic, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT t1 = {gr.right + S(6), h.rc.top + S(8), h.rc.right - S(6), h.rc.top + hh / 2 + S(2)};
        RECT t2 = {t1.left, t1.bottom, t1.right, h.rc.bottom - S(8)};
        Text(dc, nm[h.idx], t1, g.fHead, sel ? RGB(255, 255, 255) : cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        Text(dc, std::to_wstring(n), t2, g.fSmall, sel ? RGB(225, 235, 255) : cDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case HT::FavBtn: {  // Herz: laufender Song als Favorit
        bool fav = g.pst.active && IsFav(g.pst.songId);
        if (hot) FillRound(dc, h.rc, S(14), cCardHot);
        Text(dc, fav ? L"" : L"", h.rc, g.fGlyphL, fav ? RGB(255, 90, 120) : g.pst.active ? cText : RGB(85, 85, 95),
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case HT::MusicOpen:  // Bibliothek (größer, mit großem Symbol)
        if (hot) FillRound(dc, h.rc, S(14), cCardHot);
        Text(dc, L"", h.rc, g.fGlyphL, MusicView() ? RGB(120, 180, 255) : cText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    // ---------- Song bearbeiten
    case HT::EditCover: {
        int i = Music_Find(g.editId);
        DrawCover(dc, i >= 0 ? Music_Songs()[i].cover : L"", h.rc.left, h.rc.top, h.rc.right - h.rc.left);
        break;
    }
    case HT::EditBtn: {
        static const wchar_t* names[] = {L"Bild vom PC …", L"Cover im Web suchen", L"Speichern", L"Song löschen",
                                         L"▶  Anfang", L"▶  Ende", L"Nur Auswahl behalten", L"Auswahl entfernen", L"↶  Rückgängig"};
        bool trimmed = g.trimA > 0.05 || g.trimB < g.trimLen - 0.05;
        bool off = (h.idx == 6 || h.idx == 7) && !trimmed;
        if (h.idx == 8) {
            int n = Music_UndoCount(g.editId);
            off = n == 0;
        }
        COLORREF bg = h.idx == 2 ? (hot ? RGB(120, 180, 255) : cAccent) : hot && !off ? cHover : cSearch;
        FillRound(dc, h.rc, S(12), bg);
        Text(dc, names[h.idx], h.rc, h.idx == 2 ? g.fHead : g.fText,
             off ? RGB(110, 110, 120) : h.idx == 3 || h.idx == 7 ? RGB(255, 150, 150) : RGB(250, 250, 252),
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        break;
    }
    case HT::EditInfo:
        if (h.idx >= 10) {  // Bibliothek: leere Liste
            FillRound(dc, h.rc, S(14), cCardHot);
            RECT tr = h.rc;
            if (h.idx == 13) { tr.left += S(14); tr.right -= S(14); }
            Text(dc, h.idx == 10 ? L"Noch keine Playlist – über das Zahnrad oben rechts anlegen"
                     : h.idx == 11 ? L"Noch keine Favoriten – tippe beim Song aufs Herz"
                     : h.idx == 13 ? L"Keine Cover gefunden. Prüfe die Internetverbindung oder ändere Titel und Künstler unten (z. B. ohne Zusätze wie „Official Video“) und such nochmal."
                                   : L"Noch nichts gespielt",
                 tr, g.fSmall, cDim, h.idx == 13 ? (DT_CENTER | DT_VCENTER | DT_WORDBREAK) : (DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS));
        } else if (h.pane == 1) {
            FillRound(dc, h.rc, S(14), cCardHot);
            Text(dc, L"Noch keine Playlist – unten eine anlegen", h.rc, g.fSmall, cDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            Text(dc, L"Suche Cover …", h.rc, g.fText, cDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        break;
    case HT::CoverPick: {
        int px = h.rc.right - h.rc.left;
        if (hot) FillRound(dc, {h.rc.left - S(3), h.rc.top - S(3), h.rc.right + S(3), h.rc.bottom + S(3)}, S(16), cAccent);
        if (h.idx < (int)g.coverResults.size()) DrawCover(dc, g.coverResults[h.idx], h.rc.left, h.rc.top, px);
        break;
    }
    case HT::EditLabel:
        if (h.idx == 2) {
            Text(dc, L"Schneiden  ·  Länge " + FmtMs(g.trimLen) + L"  ·  Pfeiltasten schieben die Linie (Umschalt = 100 ms)",
                 {h.rc.left + S(4), h.rc.top, h.rc.right, h.rc.bottom}, g.fSmall, cDim, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
            break;
        }
        if (h.idx == 3) {  // genaue Werte der beiden Linien
            RECT l = {h.rc.left + S(4), h.rc.top, h.rc.right, h.rc.bottom};
            Text(dc, L"Anfang  " + FmtMs(g.trimA), l, g.fText, g.trimSel == 1 ? RGB(120, 180, 255) : cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            Text(dc, L"Auswahl  " + FmtMs(g.trimB - g.trimA), l, g.fSmall, cDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            RECT r = {h.rc.left, h.rc.top, h.rc.right - S(4), h.rc.bottom};
            Text(dc, L"Ende  " + FmtMs(g.trimB), r, g.fText, g.trimSel == 2 ? RGB(120, 180, 255) : cText, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            break;
        }
        Text(dc, h.idx == 0 ? L"Titel" : L"Künstler", {h.rc.left + S(4), h.rc.top, h.rc.right, h.rc.bottom}, g.fSmall, cDim, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
        break;
    case HT::EditField: {
        bool active = g.editField == h.idx;
        DrawField(dc, h.rc, h.idx == 1 ? g.editTitle : g.editArtist, h.idx == 1 ? L"Titel" : L"Künstler", active);
        if (active) {  // aktives Feld mit Rahmen hervorheben
            FillRound(dc, {h.rc.left, h.rc.bottom - S(2), h.rc.right, h.rc.bottom}, S(2), cAccent);
        }
        break;
    }
    // ---------- Ordner-Seite
    case HT::Pane: {
        bool removeTarget = h.idx == 1 && g.dragging && g.dropF == -2;
        FillRound(dc, h.rc, S(18), removeTarget ? cHover : cCard);
        break;
    }
    case HT::PaneTitle: {
        RECT nr = {h.rc.left + S(16), h.rc.top + S(4), h.rc.right - S(12), h.rc.bottom};
        if (h.idx == 2) {
            Text(dc, L"Playlists", nr, g.fHead, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        } else if (h.idx == 3) {
            if (g.dragging && g.dropF == -2)
                Text(dc, L"Loslassen: aus der Playlist nehmen", nr, g.fHead, RGB(140, 190, 255), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            else {
                Text(dc, L"Alle Songs", nr, g.fHead, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                Text(dc, std::to_wstring(Music_Songs().size()), nr, g.fSmall, cDim, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            }
        } else if (h.idx == 0) {
            Text(dc, L"Ordner", nr, g.fHead, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        } else if (g.dragging && g.dropF == -2) {
            Text(dc, L"Loslassen: aus dem Ordner nehmen", nr, g.fHead, RGB(140, 190, 255), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
            Text(dc, L"Alle Apps", nr, g.fHead, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            Text(dc, std::to_wstring(g.allList.size()), nr, g.fSmall, cDim, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        break;
    }
    case HT::SetFolderHead: {
        bool target = g.dragging && g.dropF == h.f;
        RECT nr = {h.rc.left + S(12), h.rc.top, h.rc.right - S(124), h.rc.bottom};
        const std::wstring& name = g.allFolders[h.f].name;
        SelectObject(dc, g.fHead);
        SIZE ext = {};
        GetTextExtentPoint32W(dc, name.c_str(), (int)name.size(), &ext);
        Text(dc, name, nr, g.fHead, target ? RGB(140, 190, 255) : cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT cr2 = {nr.left + ext.cx + S(8), nr.top, nr.right, nr.bottom};
        Text(dc, std::to_wstring(h.n), cr2, g.fSmall, cDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case HT::SetFolderDelete:
        DrawGlyphButton(dc, h.rc, L"\uE74D", hot, true);
        break;
    case HT::SetFolderGame: {  // Spielmodus an/aus (Controller-Symbol, blau = an)
        bool on = h.f < (int)g.allFolders.size() && FolderGameMode(g.allFolders[h.f].name);
        if (on) FillRound(dc, h.rc, S(8), cAccent);
        else if (hot) FillRound(dc, h.rc, S(8), cHover);
        Text(dc, L"\uE7FC", h.rc, g.fGlyph, on ? RGB(255, 255, 255) : cDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case HT::SetFolderUp:
    case HT::SetFolderDown:
    case HT::SetFolderMenu: {
        bool up = h.type == HT::SetFolderUp, down = h.type == HT::SetFolderDown;
        bool enabled = !(up && h.pos <= 0) && !(down && h.pos >= h.n - 1);
        DrawGlyphButton(dc, h.rc, up ? L"" : down ? L"" : L"", hot, enabled);
        break;
    }
    case HT::SetAppRow:
    case HT::AllAppRow: {
        bool all = h.type == HT::AllAppRow;
        const App& a = g.apps[h.idx];
        bool dragged = g.dragging && h.idx == g.dragApp;
        if (dragged) FillRound(dc, h.rc, S(10), cCardHot);
        else if (hot && !g.dragging) FillRound(dc, h.rc, S(10), cHover);
        int isz = S(28), rh = h.rc.bottom - h.rc.top;
        DrawAppIcon(dc, h.idx, h.rc.left + S(8), h.rc.top + (rh - isz) / 2, isz);
        int tx = h.rc.left + S(8) + isz + S(10);
        COLORREF tc = dragged || a.hidden ? cDim : cText;
        if (all) {
            RECT n1 = {tx, h.rc.top + S(3), h.rc.right - S(40), (h.rc.top + h.rc.bottom) / 2 + S(3)};
            RECT n2 = {tx, (h.rc.top + h.rc.bottom) / 2 + S(1), h.rc.right - S(40), h.rc.bottom - S(2)};
            Text(dc, a.name, n1, g.fText, tc, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
            std::wstring sub = a.hidden ? L"ausgeblendet" : a.folder.empty() ? L"in keinem Ordner" : a.folder;
            Text(dc, sub, n2, g.fSmall, cDim, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
            RECT nr = {tx, h.rc.top, h.rc.right - S(8), h.rc.bottom};
            Text(dc, a.name, nr, g.fText, tc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        break;
    }
    case HT::AllAppAdd:
        if (hot) FillRound(dc, h.rc, h.rc.bottom - h.rc.top, cHover);
        Text(dc, L"", h.rc, g.fGlyph, hot ? cText : cDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    case HT::SetDrop: {
        bool target = g.dragging && g.dropF == h.f;
        FillRound(dc, h.rc, S(10), target ? cHover : cSearch);
        Text(dc, L"Leer – Apps hierher ziehen", h.rc, g.fSmall, cDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case HT::NewFolder:
        if (g.creatingFolder) {
            DrawField(dc, h.rc, g.query, h.idx == 1 ? L"Playlist-Name, dann Enter" : L"Ordnername, dann Enter", true);
        } else {
            FillRound(dc, h.rc, S(12), hot ? cHover : cCardHot);
            Text(dc, h.idx == 1 ? L"+  Neue Playlist" : L"+  Neuer Ordner", h.rc, g.fText, cText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        break;
    case HT::SearchR:
        DrawField(dc, h.rc, g.creatingFolder ? L"" : g.query, h.idx == 1 ? L"Songs suchen" : L"Apps suchen", !g.creatingFolder);
        break;
    default:
        break;
    }
}

static void DrawScene(HDC dc, int W, int H) {
    g.iconQueue.clear();
    g.activeField = {};
    g.shapes.clear();
    g.texts.clear();
    RECT cr = {0, 0, W, H};
    SetBkMode(dc, TRANSPARENT);
    FillRect(dc, &cr, (HBRUSH)GetStockObject(BLACK_BRUSH));
    W += RailW();
    int pad = S(18) + RailW();

    // Kopfzeile
    std::wstring title = HeaderTitle();
    for (int i = 0; i < (int)g.hits.size(); i++) {
        auto& h = g.hits[i];
        if (h.type == HT::Bluetooth) {
            bool conn = false;
            for (auto& d : g.btDevices) conn |= d.connected;
            if (i == g.hot) FillRound(dc, h.rc, S(10), cHover);
            Text(dc, L"\uE702", h.rc, g.fGlyph, conn ? RGB(120, 180, 255) : g.btRadio ? cText : cDim,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if (conn) FillCircle(dc, h.rc.right - S(8), h.rc.top + S(8), S(3), cAccent);  // Punkt: Gerät verbunden
            continue;
        }
        if (h.type == HT::Wifi) {  // WLAN-Stärke (helle Balken über gedimmtem Vollsymbol), LAN oder kein Netz
            if (i == g.hot) FillRound(dc, h.rc, S(10), cHover);
            if (g.net == 2) {
                Text(dc, L"", h.rc, g.fGlyph, g.netInternet ? cText : cDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else if (g.net == 1) {
                static const wchar_t* lv[5] = {L"", L"", L"", L"", L""};
                Text(dc, lv[std::max(0, std::min(4, g.netBars))], h.rc, g.fGlyph, g.netInternet ? cText : cDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                Text(dc, L"", h.rc, g.fGlyph, cDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            continue;
        }
        if (h.type == HT::Battery) {  // Akku: Symbol + Prozent
            if (i == g.hot) FillRound(dc, h.rc, S(10), cHover);
            int lvl = std::max(0, std::min(10, (g.batt + 5) / 10));
            wchar_t gl[2] = {0, 0};
            gl[0] = g.battCharging ? (lvl >= 9 ? 0xEA93 : (wchar_t)(0xE85A + lvl)) : (lvl >= 10 ? 0xE83F : (wchar_t)(0xE850 + lvl));
            COLORREF col = g.battCharging ? RGB(120, 220, 140) : g.batt <= 15 ? RGB(255, 120, 110) : cText;
            RECT gr = {h.rc.left + S(4), h.rc.top, h.rc.left + S(32), h.rc.bottom};
            Text(dc, gl, gr, g.fGlyph, col, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            Text(dc, std::to_wstring(g.batt) + L"%", {gr.right + S(2), h.rc.top, h.rc.right, h.rc.bottom}, g.fSmall, cText,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            continue;
        }
        if (h.type == HT::NavBar) {  // Navigationsleiste: schmale Glas-Karte
            FillRound(dc, h.rc, S(16), cCard);
            continue;
        }
        if (h.type == HT::Nav) {  // Home / Musik / Kalender, die aktuelle Seite hervorgehoben
            bool sel = NavPage() == h.idx;
            if (sel) FillRound(dc, h.rc, S(12), cAccent);
            else if (i == g.hot) FillRound(dc, h.rc, S(12), cHover);
            static const wchar_t* gl[6] = {L"\uE80F", L"\uEC4F", L"\uE787", L"\uEDA2", L"\uE8BD", L"\uE916"};
            if (h.idx == 4) {  // Claude: das echte Claude-Symbol (von claude.ai), bis es geladen ist ein Sprechblasen-Symbol
                int px = S(28);
                if (HBITMAP cb = ClaudeIcon(px)) {
                    g.iconQueue.push_back({-3, (h.rc.left + h.rc.right - px) / 2, (h.rc.top + h.rc.bottom - px) / 2, px, CurrentClip(dc), 1.f, cb});
                    continue;
                }
            }
            Text(dc, gl[h.idx], h.rc, g.fGlyphL, sel ? RGB(255, 255, 255) : cText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            continue;
        }
        if (h.type == HT::TitleBtn) {  // "Apps" ist anklickbar: leichte Fläche beim Darüberfahren
            if (i == g.hot) FillRound(dc, h.rc, S(10), cHover);
            continue;
        }
        if (h.type == HT::Logo) {  // App-Logo "[<]"
            int px = S(26);
            if (!g.logoBmp || g.logoPx != px) {
                if (g.logoBmp) DeleteObject(g.logoBmp);
                g.logoBmp = MakeLogoBitmap(px);
                g.logoPx = px;
            }
            if (g.logoBmp)
                g.iconQueue.push_back({-3, (h.rc.left + h.rc.right - px) / 2, (h.rc.top + h.rc.bottom - px) / 2, px, CurrentClip(dc), 1.f, g.logoBmp});
            continue;
        }
        if (h.type != HT::Back && h.type != HT::Menu && h.type != HT::Gear) continue;
        if (i == g.hot) FillRound(dc, h.rc, S(10), cHover);
        if (h.type == HT::Gear) Text(dc, L"", h.rc, g.fGlyph, cText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        else Text(dc, h.type == HT::Back ? L"‹" : L"···", h.rc, g.fTitle, cText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    Text(dc, title, {pad + S(36), S(14), g.headerRight - S(6), S(48)}, g.fTitle, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (g.view == View::Settings) {
        FillRound(dc, {pad, S(58), W - pad, S(94)}, S(12), cSearch);  // Reiter-Hintergrund
    } else {
        if (g.view == View::Music) {
            DrawField(dc, {pad, S(58), W - pad, S(94)}, g.query,
                      Music_ToolAvailable() ? L"YouTube-Link einfügen (Strg+V), dann Enter" : L"YouTube-Link einfügen, dann Enter (yt-dlp wird dann geladen)", true);
        } else if (g.view == View::MusicList) {
            size_t n = MusicListItems(g.curPlaylist).size();
            Text(dc, std::to_wstring(n) + (n == 1 ? L" Song" : L" Songs"), {pad + S(4), S(58), W - pad, S(94)}, g.fText, cDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        } else if (g.view == View::MusicSettings) {
            Text(dc, L"Größe der Alben und Songs:", {pad + S(4), S(58), W - pad - 3 * S(84) - S(10), S(94)},
                 g.fSmall, cDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else if (g.view == View::Chat) {
            std::wstring st = Chat_Status();
            Text(dc, st.empty() ? L"claude.ai – angemeldet bleibst du dauerhaft" : st, {pad + S(4), S(58), W - pad - S(140), S(94)}, g.fSmall, cDim,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            FillRound(dc, g.chatRect, S(12), cSearch);  // sichtbar, bis das Chat-Fenster darüber liegt
        } else if (g.view == View::Timer) {
            Text(dc, g.timerRinging ? L"Klingelt" : g.timerEnd ? L"Läuft" : g.timerLeft ? L"Angehalten" : L"Dauer wählen: ziehen oder Mausrad",
                 {pad + S(4), S(58), W - pad, S(94)}, g.fSmall, cDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else if (g.view == View::Storage) {
            std::wstring st =g.storageStamp.empty() ? L"Belegung deiner Laufwerke" : L"Aktualisiert um " + g.storageStamp + L" · App-Größen werden neu gemessen";
            Text(dc, st, {pad + S(4), S(58), W - pad - S(160), S(94)}, g.fSmall, cDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else if (g.view == View::Calendar) {
            Text(dc, std::wstring(kMonths[g.calM - 1]) + L" " + std::to_wstring(g.calY), {pad + S(4), S(58), W - pad - 2 * S(34) - S(96), S(94)},
                 g.fBarTitle, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else if (g.view == View::AllApps) {
            int known = 0;
            for (auto& e : g.appSizes) known += e.second > 0;
            wchar_t sum[96];
            swprintf(sum, 96, L"%d Apps · zusammen %.1f GB", (int)g.appSizes.size(), g.appTotal / 1073741824.0);
            Text(dc, sum, {pad + S(4), S(58), W - pad - 2 * S(76) - S(10), S(94)}, g.fSmall, cDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else if (g.view != View::SongEdit && g.view != View::NowPlaying) {
            const wchar_t* ph = g.namingFor >= 0 ? L"Name des neuen Ordners, dann Enter" : L"Apps durchsuchen";
            DrawField(dc, {pad, S(58), W - pad, S(94)}, g.query, ph, true);
        }
    }

    int top = ContentTop();
    if (!g.loaded && g.view != View::Settings && !MusicView() && g.view != View::Timer) {
        Text(dc, L"Apps werden geladen …", {0, top + S(40), W, top + S(80)}, g.fText, cDim, DT_CENTER | DT_SINGLELINE);
    } else if (g.view == View::Search && g.results.empty() && g.namingFor < 0) {
        Text(dc, L"Keine Treffer", {0, top + S(40), W, top + S(80)}, g.fText, cDim, DT_CENTER | DT_SINGLELINE);
    }

    int cardHot = -1;
    if (g.hot >= 0 && g.hits[g.hot].type == HT::Card) cardHot = g.hits[g.hot].idx;

    // Ordner-Karten, die oben oder unten angeschnitten sind
    // Deckkraft = 2 * (sichtbarer Anteil - 50 %): halb sichtbar -> ganz unsichtbar, ganz sichtbar -> voll
    std::vector<std::pair<RECT, float>> partial;
    if (g.view == View::Folders)
        for (auto& h : g.hits)
            if ((h.type == HT::Card || (h.type == HT::SetCard && h.pane == 0)) &&
                (h.rc.top < ContentTop() - 1 || h.rc.bottom > ContentBottom() + 1)) {
                int vis = std::min<int>(h.rc.bottom, ContentBottom()) - std::max<int>(h.rc.top, ContentTop());
                float frac = std::max(0, vis) / (float)std::max(1L, h.rc.bottom - h.rc.top);
                partial.push_back({h.rc, std::max(0.f, std::min(1.f, 2.f * (frac - 0.5f)))});
            }

    for (int i = 0; i < (int)g.hits.size(); i++) {
        auto& h = g.hits[i];
        if (h.type == HT::Back || h.type == HT::Menu || h.type == HT::Gear || h.type == HT::Bluetooth || h.type == HT::Logo ||
            h.type == HT::Wifi || h.type == HT::Battery || h.type == HT::TitleBtn || h.type == HT::NavBar || h.type == HT::Nav)
            continue;
        bool clipped = !IsHeaderHit(h.type) && !(OrdnerPage() && h.pane == 0);
        RECT c = PaneClip(h.pane);
        if (clipped && (h.rc.bottom < c.top || h.rc.top > c.bottom)) continue;
        g.drawFade = 1.f;
        for (auto& pr : partial) {
            if (IsHeaderHit(h.type)) break;  // Musik-Leiste und Knöpfe sind nie angeschnitten
            POINT mid = {(h.rc.left + h.rc.right) / 2, (h.rc.top + h.rc.bottom) / 2};
            if (PtInRect(&pr.first, mid)) { g.drawFade = pr.second; break; }
        }
        if (g.view == View::NowPlaying) g.drawFade = g.npAlpha;  // beim Einklappen ausblenden
        if (g.drawFade <= 0.003f) continue;  // ganz ausgeblendet: gar nicht zeichnen
        SaveDC(dc);
        if (clipped) IntersectClipRect(dc, c.left, c.top, c.right, c.bottom);
        DrawHit(dc, h, i == g.hot, cardHot);
        RestoreDC(dc, -1);
    }
    g.drawFade = 1.f;

    // Einfügelinie beim Ziehen (linke Ordnerliste)
    if (g.dragging && g.dropF >= 0 && g.dropLineY >= 0) {
        SaveDC(dc);
        RECT c = PaneClip(1);
        IntersectClipRect(dc, c.left, c.top, c.right, c.bottom);
        FillRound(dc, {pad + S(16), g.dropLineY - S(2), g.paneMid - S(20), g.dropLineY + S(1)}, S(3), cAccent);
        RestoreDC(dc, -1);
    }
}

// ---------------------------------------------------------------- Hintergrund: Bild, Glas, Farbe

// Bild mit WIC laden und füllend (Seitenverhältnis bleibt) auf W x H zuschneiden
static bool LoadImageCover(const std::wstring& path, int W, int H, std::vector<uint32_t>& out) {
    out.clear();
    bool ok = false;
    IWICImagingFactory* f = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) {
        IWICBitmapDecoder* dec = nullptr;
        if (SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) {
            IWICBitmapFrameDecode* fr = nullptr;
            if (SUCCEEDED(dec->GetFrame(0, &fr))) {
                UINT iw = 0, ih = 0;
                fr->GetSize(&iw, &ih);
                if (iw && ih) {
                    double sc = std::max((double)W / iw, (double)H / ih);
                    UINT sw = std::max<UINT>((UINT)W, (UINT)std::ceil(iw * sc));
                    UINT sh = std::max<UINT>((UINT)H, (UINT)std::ceil(ih * sc));
                    IWICBitmapScaler* scl = nullptr;
                    IWICFormatConverter* cv = nullptr;
                    if (SUCCEEDED(f->CreateBitmapScaler(&scl)) &&
                        SUCCEEDED(scl->Initialize(fr, sw, sh, WICBitmapInterpolationModeFant)) &&
                        SUCCEEDED(f->CreateFormatConverter(&cv)) &&
                        SUCCEEDED(cv->Initialize(scl, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                                 WICBitmapPaletteTypeCustom))) {
                        out.resize((size_t)W * H);
                        WICRect rc = {(INT)(sw - W) / 2, (INT)(sh - H) / 2, W, H};
                        ok = SUCCEEDED(cv->CopyPixels(&rc, W * 4, (UINT)(out.size() * 4), (BYTE*)out.data()));
                    }
                    if (cv) cv->Release();
                    if (scl) scl->Release();
                }
                fr->Release();
            }
            dec->Release();
        }
        f->Release();
    }
    if (!ok) std::vector<uint32_t>().swap(out);
    return ok;
}

// Bild für den flachen Modus in Fenstergröße (zwischengespeichert)
static bool LoadBackgroundImage(int W, int H) {
    if (g.imgPath.empty()) {
        if (!g.img.empty()) std::vector<uint32_t>().swap(g.img);
        g.imgLoaded.clear();
        return false;
    }
    if (g.imgLoaded == g.imgPath && g.imgW == W && g.imgH == H) return !g.img.empty();
    g.imgLoaded = g.imgPath;
    g.imgW = W;
    g.imgH = H;
    return LoadImageCover(g.imgPath, W, H, g.img);
}

// Glas: weichzeichnen (dreimal Kastenfilter ≈ Gauß) auf dem verkleinerten Hintergrund
static void BlurBackdrop() {
    g.bdVersion++;
    g.bdBlur = g.bdRaw;
    int W = g.bdW, H = g.bdH;
    if (W <= 0 || H <= 0) return;
    int r = 1 + g.blurAmount * 9 / 100;  // in verkleinerten Pixeln (×4 auf dem Bildschirm)
    std::vector<uint32_t> tmp(g.bdBlur.size());
    auto pass = [&](const std::vector<uint32_t>& src, std::vector<uint32_t>& dst, bool horiz) {
        int len = horiz ? W : H, lines = horiz ? H : W;
        for (int l = 0; l < lines; l++) {
            int sr = 0, sg = 0, sb = 0;
            auto at = [&](int i) -> uint32_t {
                i = std::max(0, std::min(len - 1, i));
                return horiz ? src[(size_t)l * W + i] : src[(size_t)i * W + l];
            };
            for (int i = -r; i <= r; i++) {
                uint32_t p = at(i);
                sr += (p >> 16) & 255; sg += (p >> 8) & 255; sb += p & 255;
            }
            int n = 2 * r + 1;
            for (int i = 0; i < len; i++) {
                uint32_t v = 0xFF000000u | ((uint32_t)(sr / n) << 16) | ((uint32_t)(sg / n) << 8) | (uint32_t)(sb / n);
                if (horiz) dst[(size_t)l * W + i] = v;
                else dst[(size_t)i * W + l] = v;
                uint32_t a = at(i - r), b = at(i + r + 1);
                sr += ((b >> 16) & 255) - ((a >> 16) & 255);
                sg += ((b >> 8) & 255) - ((a >> 8) & 255);
                sb += (b & 255) - (a & 255);
            }
        }
    };
    for (int k = 0; k < 3; k++) {
        pass(g.bdBlur, tmp, true);
        pass(tmp, g.bdBlur, false);
    }
}

// Glas: den Bildschirm hinter der späteren Leiste aufnehmen (die Leiste ist dabei noch unsichtbar)
static void CaptureBackdrop(int x, int y, int W, int H) {
    std::vector<uint32_t>().swap(g.bdRaw);
    std::vector<uint32_t>().swap(g.bdBlur);
    g.bdW = g.bdH = 0;
    if (!g.blur || W <= 0 || H <= 0) return;
    g.capX = x;
    g.capY = y;
    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB};
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp) {
        HGDIOBJ o = SelectObject(dc, bmp);
        BitBlt(dc, 0, 0, W, H, screen, x, y, SRCCOPY);
        GdiFlush();
        const uint32_t* px = (const uint32_t*)bits;
        std::vector<uint32_t> img;
        float k = 0;
        if (!g.imgPath.empty() && LoadImageCover(g.imgPath, W, H, img)) k = g.imgStrength / 100.f;
        // verkleinern (Mittelwert aus 4×4) – spart Speicher und macht das Weichzeichnen billig
        int F = State::bdFactor;
        g.bdW = std::max(1, W / F);
        g.bdH = std::max(1, H / F);
        g.bdRaw.resize((size_t)g.bdW * g.bdH);
        for (int by = 0; by < g.bdH; by++)
            for (int bx = 0; bx < g.bdW; bx++) {
                float r = 0, gg = 0, b = 0;
                for (int yy = 0; yy < F; yy++)
                    for (int xx = 0; xx < F; xx++) {
                        size_t i = (size_t)(by * F + yy) * W + (bx * F + xx);
                        uint32_t p = px[i];
                        float pr = (float)((p >> 16) & 255), pg = (float)((p >> 8) & 255), pb = (float)(p & 255);
                        if (k > 0) {
                            uint32_t q = img[i];
                            pr = pr * (1 - k) + ((q >> 16) & 255) * k;
                            pg = pg * (1 - k) + ((q >> 8) & 255) * k;
                            pb = pb * (1 - k) + (q & 255) * k;
                        }
                        r += pr; gg += pg; b += pb;
                    }
                float n = (float)(F * F);
                r /= n; gg /= n; b /= n;
                // Farben etwas kräftiger, wie bei Apple-Glas
                float l = 0.299f * r + 0.587f * gg + 0.114f * b;
                const float sat = 1.45f;
                r = l + (r - l) * sat; gg = l + (gg - l) * sat; b = l + (b - l) * sat;
                auto cl = [](float v) { return (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v); };
                g.bdRaw[(size_t)by * g.bdW + bx] = 0xFF000000u | (cl(r) << 16) | (cl(gg) << 8) | cl(b);
            }
        SelectObject(dc, o);
        DeleteObject(bmp);
    }
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    BlurBackdrop();
}

// bilineares Abtasten des weichgezeichneten Hintergrunds (Bildschirmkoordinaten)
static inline void SampleBackdrop(float sx, float sy, float& r, float& gg, float& b) {
    float fx = (sx - g.capX) / State::bdFactor - 0.5f, fy = (sy - g.capY) / State::bdFactor - 0.5f;
    fx = std::max(0.f, std::min(fx, (float)g.bdW - 1.001f));
    fy = std::max(0.f, std::min(fy, (float)g.bdH - 1.001f));
    int x0 = (int)fx, y0 = (int)fy;
    int x1 = std::min(x0 + 1, g.bdW - 1), y1 = std::min(y0 + 1, g.bdH - 1);
    float ax = fx - x0, ay = fy - y0;
    const uint32_t* p = g.bdBlur.data();
    uint32_t a = p[(size_t)y0 * g.bdW + x0], bb = p[(size_t)y0 * g.bdW + x1];
    uint32_t c = p[(size_t)y1 * g.bdW + x0], d = p[(size_t)y1 * g.bdW + x1];
    auto ch = [&](int s) {
        float top = ((a >> s) & 255) * (1 - ax) + ((bb >> s) & 255) * ax;
        float bot = ((c >> s) & 255) * (1 - ax) + ((d >> s) & 255) * ax;
        return top * (1 - ay) + bot * ay;
    };
    r = ch(16);
    gg = ch(8);
    b = ch(0);
}

// Abstand eines Pixels zum Rand eines abgerundeten Rechtecks (positiv = innen)
static float EdgeDist(int x, int y, int W, int H, float R) {
    if ((x < R || x >= W - R) && (y < R || y >= H - R)) {
        float cx = x < R ? R : W - R, cy = y < R ? R : H - R;
        float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
        return R - std::sqrt(dx * dx + dy * dy);
    }
    return std::min(std::min(x + 0.5f, y + 0.5f), std::min(W - x - 0.5f, H - y - 0.5f));
}

// Abdeckung eines Pixels durch ein abgerundetes Rechteck (vorzeichenbehafteter Abstand, weich über 1 px)
static inline float ShapeDist(float px, float py, const State::Shape& s) {
    float cx = (s.r.left + s.r.right) * 0.5f, cy = (s.r.top + s.r.bottom) * 0.5f;
    float hw = (s.r.right - s.r.left) * 0.5f, hh = (s.r.bottom - s.r.top) * 0.5f;
    float qx = std::fabs(px - cx) - (hw - s.rad), qy = std::fabs(py - cy) - (hh - s.rad);
    float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - s.rad;
}

// Feste Gruppe von Hilfs-Threads für das Zeichnen. Sie werden einmal beim ersten Bild gestartet,
// warten dazwischen schlafend und werden beim Schließen der Leiste beendet (kein Arbeitsspeicher im Ruhezustand).
// Früher wurden pro Bild neue Threads erzeugt – bei vielen Bildern pro Sekunde führte das zu Abstürzen.
static std::vector<std::thread> sPool;
static std::mutex sPoolM;
static std::condition_variable sPoolCv, sPoolDone;
static std::function<void(int, int)> sPoolJob;
static int sPoolGen = 0, sPoolPending = 0, sPoolN = 1;
static bool sPoolQuit = false;
static void Pool_Stop() {
    {
        std::lock_guard<std::mutex> lk(sPoolM);
        sPoolQuit = true;
    }
    sPoolCv.notify_all();
    for (auto& t : sPool) t.join();
    sPool.clear();
    sPoolQuit = false;
}
static void Pool_Run(const std::function<void(int, int)>& job) {
    if (sPool.empty()) {
        int n = std::max(1, std::min(8, (int)std::thread::hardware_concurrency()));
        sPoolN = n;
        for (int i = 1; i < n; i++)
            sPool.emplace_back([i, start = sPoolGen] {
                int seen = start;  // erst auf den nächsten Auftrag warten
                for (;;) {
                    std::function<void(int, int)> j;
                    {
                        std::unique_lock<std::mutex> lk(sPoolM);
                        sPoolCv.wait(lk, [&] { return sPoolQuit || sPoolGen != seen; });
                        if (sPoolQuit) return;
                        seen = sPoolGen;
                        j = sPoolJob;
                    }
                    j(i, sPoolN);
                    std::lock_guard<std::mutex> lk(sPoolM);
                    if (--sPoolPending == 0) sPoolDone.notify_one();
                }
            });
    }
    if (sPool.empty()) { job(0, 1); return; }
    {
        std::lock_guard<std::mutex> lk(sPoolM);
        sPoolJob = job;
        sPoolPending = (int)sPool.size();
        sPoolGen++;
    }
    sPoolCv.notify_all();
    job(0, sPoolN);  // eigener Anteil
    std::unique_lock<std::mutex> lk(sPoolM);
    sPoolDone.wait(lk, [] { return sPoolPending == 0; });
}

// Hintergrund (Glas bzw. Farbe/Bild) und alle Flächen kantengeglättet in g.bits (BGRA, vormultipliziert)
static void Compose(int W, int H) {
    bool glass = g.blur && !g.bdBlur.empty();
    bool hasImg = !glass && LoadBackgroundImage(W, H);
    float k = g.imgStrength / 100.f;
    float tr = GetRValue(g.tint), tg = GetGValue(g.tint), tb = GetBValue(g.tint);
    float tintA = 0.10f + 0.80f * (g.bgAlpha - 40) / 215.f;
    const float R = (float)S(18);
    const float rim = (float)S(26), bend = (float)S(22);
    const float Lx = -0.7071f, Ly = -0.7071f;

    // Grundebene nur neu berechnen, wenn sich Größe oder Aussehen geändert hat
    wchar_t keyBuf[160];
    wsprintfW(keyBuf, L"%d|%d|%d|%d|%d|%d|%d|%d|%d|%u|%d", W, H, glass, hasImg, g.bgAlpha, g.imgStrength, g.blurAmount,
              g.xShown, g.y, (unsigned)g.tint, g.bdVersion);
    if (g.baseKey != keyBuf || g.basePx.size() != (size_t)W * H) {
    g.baseKey = keyBuf;
    g.basePx.resize((size_t)W * H);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float r, gg, b, a;
            float e = EdgeDist(x, y, W, H, R);
            if (glass) {
                float dl = x + 0.5f, dr = W - x - 0.5f, dt = y + 0.5f, db = H - y - 0.5f;
                float nx = 0, ny = 0;
                if (dl < rim) nx += (1 - dl / rim) * (1 - dl / rim);
                if (dr < rim) nx -= (1 - dr / rim) * (1 - dr / rim);
                if (dt < rim) ny += (1 - dt / rim) * (1 - dt / rim);
                if (db < rim) ny -= (1 - db / rim) * (1 - db / rim);
                SampleBackdrop((float)(g.xShown + x) + nx * bend, (float)(g.y + y) + ny * bend, r, gg, b);
                r = r * (1 - tintA) + tr * tintA;
                gg = gg * (1 - tintA) + tg * tintA;
                b = b * (1 - tintA) + tb * tintA;
                float len = std::sqrt(nx * nx + ny * ny);
                float lf = len > 0.0001f ? std::fabs((-nx / len) * Lx + (-ny / len) * Ly) : 0.5f;
                float spec = 0;
                if (e < 1.6f) spec = 0.12f + 0.55f * lf;
                else if (e < S(12)) spec = (0.03f + 0.14f * lf) * (1 - (e - 1.6f) / (S(12) - 1.6f));
                r += (255 - r) * spec; gg += (255 - gg) * spec; b += (255 - b) * spec;
                a = 255;
            } else {
                r = tr; gg = tg; b = tb;
                if (hasImg) {
                    uint32_t px = g.img[(size_t)y * W + x];
                    r = r * (1 - k) + ((px >> 16) & 255) * k;
                    gg = gg * (1 - k) + ((px >> 8) & 255) * k;
                    b = b * (1 - k) + (px & 255) * k;
                }
                a = (float)g.bgAlpha;
            }
            float cov = e + 0.5f;
            cov = cov < 0 ? 0 : cov > 1 ? 1 : cov;
            a *= cov;
            auto pm = [&](float v) { return (uint32_t)(std::min(255.f, v) * a / 255.f + 0.5f); };
            g.basePx[(size_t)y * W + x] = ((uint32_t)(a + 0.5f) << 24) | (pm(r) << 16) | (pm(gg) << 8) | pm(b);
        }
    }
    }
    // Flächen in Zeichenreihenfolge – direkt auf den vormultiplizierten Pixeln.
    // Das Bild wird in waagerechte Streifen geteilt, die mehrere Prozessorkerne gleichzeitig berechnen.
    struct Ov { float white, dark, boost; };
    const Ov ov[] = {{0.10f, 0, 0.25f}, {0.15f, 0, 0.30f}, {0.22f, 0, 0.35f}, {0, 0.25f, 0.30f}};
    auto stripe = [&](int yA, int yB) {
    // Grundebene ins Ausgabebild (vormultipliziert, schnell kopiert)
    memcpy(g.bits + (size_t)yA * W * 4, g.basePx.data() + (size_t)yA * W, (size_t)(yB - yA) * W * 4);
    for (auto& s : g.shapes) {
        RECT box;
        if (!IntersectRect(&box, &s.r, &s.clip)) continue;
        box.left = std::max(0L, box.left - 1); box.top = std::max(0L, box.top - 1);
        box.right = std::min((LONG)W, box.right + 1); box.bottom = std::min((LONG)H, box.bottom + 1);
        box.left = std::max(box.left, s.clip.left); box.right = std::min(box.right, s.clip.right);
        box.top = std::max(box.top, s.clip.top); box.bottom = std::min(box.bottom, s.clip.bottom);
        box.top = std::max(box.top, (LONG)yA); box.bottom = std::min(box.bottom, (LONG)yB);
        if (box.top >= box.bottom) continue;
        float sh = (float)(s.r.bottom - s.r.top);
        float sr = GetRValue(s.color), sg = GetGValue(s.color), sb = GetBValue(s.color);
        // Innenbereich: volle Deckung, keine Kante -> ohne Abstandsrechnung
        float m = std::max(s.rad, 2.f) + 1.5f;
        int inL = (int)std::ceil(s.r.left + m), inR = (int)(s.r.right - m), inT = (int)std::ceil(s.r.top + m), inB = (int)(s.r.bottom - m);
        bool solid = s.kind == State::SH_SOLID;
        Ov o = solid ? Ov{0, 0, 0} : ov[s.kind];
        for (int y = box.top; y < box.bottom; y++) {
            bool rowInner = y >= inT && y < inB;
            float vf = 1 - 2 * (y + 0.5f - s.r.top) / std::max(1.f, sh);  // +1 oben, -1 unten
            BYTE* p = g.bits + ((size_t)y * W + box.left) * 4;
            for (int x = box.left; x < box.right; x++, p += 4) {
                int A = p[3];
                if (!A) continue;  // außerhalb der Fensterecken
                bool inner = rowInner && x >= inL && x < inR;
                float d, cov;
                if (inner) {
                    d = -3.f;
                    cov = s.fade;
                } else {
                    d = ShapeDist(x + 0.5f, y + 0.5f, s);
                    cov = 0.5f - d;
                    if (cov <= 0) continue;
                    if (cov > 1) cov = 1;
                    cov *= s.fade;  // erst begrenzen, dann abschwächen
                }
                float B = p[0], G = p[1], Rr = p[2], fA = (float)A;
                float nB, nG, nR, nA;
                if (solid) {
                    nB = sb; nG = sg; nR = sr; nA = 255;
                } else {
                    float w = o.white, dk = o.dark;
                    if (glass && !inner) {
                        // Glaskante: oben Licht, unten leichter Schatten (Felder: eingelassen)
                        float band = std::max(0.f, std::min(1.f, d + 2.0f));
                        if (s.kind == State::SH_SEARCH) dk += band * std::max(0.f, vf) * 0.45f;
                        else {
                            w += band * std::max(0.f, vf) * 0.35f;
                            dk += band * std::max(0.f, -vf) * 0.15f;
                        }
                    }
                    // aufhellen/abdunkeln im vormultiplizierten Raum: P' = P + (A - P) * w
                    nB = (B + (fA - B) * w) * (1 - dk);
                    nG = (G + (fA - G) * w) * (1 - dk);
                    nR = (Rr + (fA - Rr) * w) * (1 - dk);
                    nA = fA;
                    if (!glass && o.boost > 0) {  // flacher Modus: Flächen etwas deckender
                        nA = fA + (255 - fA) * o.boost;
                        float sc = nA / fA;
                        nB *= sc; nG *= sc; nR *= sc;
                    }
                }
                p[0] = (BYTE)std::min(255.f, B + (nB - B) * cov + 0.5f);
                p[1] = (BYTE)std::min(255.f, G + (nG - G) * cov + 0.5f);
                p[2] = (BYTE)std::min(255.f, Rr + (nR - Rr) * cov + 0.5f);
                p[3] = (BYTE)std::min(255.f, fA + (nA - fA) * cov + 0.5f);
            }
        }
    }
    };
    if (H < 64) { stripe(0, H); return; }
    Pool_Run([&](int i, int n) { stripe(H * i / n, H * (i + 1) / n); });
}
// Texte aus der Maske einfärben: ClearType-Abdeckung je Farbkanal -> scharfe Schrift auf jedem Untergrund
static void ApplyTextRows(int W, int H, int yA, int yB);
static void ApplyText(int W, int H) {
    // in waagerechten Streifen parallel (die Reihenfolge der Texte bleibt je Streifen erhalten)
    if (H < 64) { ApplyTextRows(W, H, 0, H); return; }
    Pool_Run([&](int i, int n) { ApplyTextRows(W, H, H * i / n, H * (i + 1) / n); });
}
static void ApplyTextRows(int W, int H, int yA, int yB) {
    for (auto& t : g.texts) {
        int cr = GetRValue(t.color), cg = GetGValue(t.color), cb = GetBValue(t.color);
        int tf = (int)(t.fade * 256);
        for (int y = std::max((LONG)yA, t.r.top); y < std::min((LONG)std::min(H, yB), t.r.bottom); y++) {
            for (int x = std::max(0L, t.r.left); x < std::min((LONG)W, t.r.right); x++) {
                size_t i = ((size_t)y * W + x) * 4;
                BYTE* m = g.maskBits + i;
                int mb = m[0] * tf >> 8, mg = m[1] * tf >> 8, mr = m[2] * tf >> 8;
                if (!(mb | mg | mr)) continue;
                BYTE* d = g.bits + i;
                int ma = std::max(mb, std::max(mg, mr));
                d[0] = (BYTE)((cb * mb + d[0] * (255 - mb)) / 255);
                d[1] = (BYTE)((cg * mg + d[1] * (255 - mg)) / 255);
                d[2] = (BYTE)((cr * mr + d[2] * (255 - mr)) / 255);
                d[3] = (BYTE)(ma + d[3] * (255 - ma) / 255);
                m[0] = m[1] = m[2] = 0;  // nicht doppelt anwenden, falls Bereiche überlappen
            }
        }
    }
}
// Deckendes Kästchen mit Text (und optional Symbol) über alles legen: Kurzinfo, gezogene App
static void DrawFloatBox(RECT r, const std::wstring& text, int iconIdx) {
    int bw = r.right - r.left, bh = r.bottom - r.top;
    if (bw <= 0 || bh <= 0) return;
    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), bw, -bh, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(g.memDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return;
    HDC dc = CreateCompatibleDC(g.memDC);
    HGDIOBJ ob = SelectObject(dc, bmp);
    RECT all = {0, 0, bw, bh};
    HBRUSH br = CreateSolidBrush(RGB(30, 30, 38));
    FillRect(dc, &all, br);
    DeleteObject(br);
    SetBkMode(dc, TRANSPARENT);
    int isz = S(28);
    RECT tr = {iconIdx >= 0 ? S(10) + isz + S(10) : S(12), 0, bw - S(12), bh};
    SelectObject(dc, g.fText);
    SetTextColor(dc, cText);
    DrawTextW(dc, text.c_str(), -1, &tr, (iconIdx >= 0 ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    GdiFlush();
    float R = (float)S(9);
    const BYTE* s = (const BYTE*)bits;
    for (int y = 0; y < bh; y++) {
        int dy = r.top + y;
        if (dy < 0 || dy >= g.memH) continue;
        for (int x = 0; x < bw; x++) {
            int dx = r.left + x;
            if (dx < 0 || dx >= g.memW) continue;
            float e = EdgeDist(x, y, bw, bh, R);
            float cov = e + 0.5f;
            cov = cov < 0 ? 0 : cov > 1 ? 1 : cov;
            int a = (int)(245 * cov);
            const BYTE* sp = s + ((size_t)y * bw + x) * 4;
            int lift = e < 1.5f ? 40 : 0;  // feine helle Kante
            BYTE* d = g.bits + ((size_t)dy * g.memW + dx) * 4;
            for (int c = 0; c < 3; c++) d[c] = (BYTE)((std::min(255, sp[c] + lift) * a + d[c] * (255 - a)) / 255);
            d[3] = (BYTE)(a + d[3] * (255 - a) / 255);
        }
    }
    SelectObject(dc, ob);
    DeleteDC(dc);
    DeleteObject(bmp);
    if (iconIdx >= 0) BlitIcon(g.memDC, iconIdx, r.left + S(10), r.top + (bh - isz) / 2, isz);
}

static bool IsSongHit(HT t) { return t == HT::MTile || t == HT::MRow || t == HT::PlSongRow || t == HT::AllSongRow; }
static bool IsAppHit(HT t) { return t == HT::Slot || t == HT::Cell || t == HT::GameRow || t == HT::AppSizeRow || t == HT::SetAppRow || t == HT::AllAppRow || t == HT::Fav || t == HT::FavSlot || IsSongHit(t); }

static void DrawOverlays() {
    // App-Name (bzw. Bluetooth-Status) beim Darüberfahren
    if (g.tipShown && !g.dragging && g.hot >= 0 && g.hot < (int)g.hits.size() &&
        ((g.hits[g.hot].idx >= 0 && IsAppHit(g.hits[g.hot].type)) || g.hits[g.hot].type == HT::Bluetooth || g.hits[g.hot].type == HT::MediaPhone ||
         g.hits[g.hot].type == HT::Wifi || g.hits[g.hot].type == HT::Battery)) {
        const Hit& h = g.hits[g.hot];
        std::wstring text;
        if (h.type == HT::Wifi) {
            text = g.net == 0 ? L"Kein Netzwerk" : g.net == 2 ? L"LAN" : g.net == 3 ? L"Mobilfunk" : L"WLAN";
            if (g.net && !g.netName.empty()) text += L": " + g.netName;
            if (g.net && !g.netInternet) text += L" (kein Internet)";
        } else if (h.type == HT::Battery) {
            text = L"Akku " + std::to_wstring(g.batt) + L" %" + (g.battCharging ? L" – lädt" : L"");
        } else if (h.type == HT::MediaPhone) {
            text = g.phoneConnected ? L"Handy-Musik läuft über den PC (" + g.phoneName + L") – klicken zum Trennen"
                                    : L"Musik vom Handy über den PC abspielen";
        } else if (h.type == HT::Bluetooth) {
            std::wstring names;
            for (auto& d : g.btDevices)
                if (d.connected) names += (names.empty() ? L"" : L", ") + d.name;
            text = !g.btRadio ? L"Bluetooth ist aus – klicken zum Verbinden" : names.empty() ? L"Bluetooth: nichts verbunden" : L"Verbunden: " + names;
        } else {
            if (IsSongHit(h.type)) {
                const Song* s = SongAt(h.idx);
                text = s ? s->title + (s->artist.empty() ? L"" : L" – " + s->artist) : L"";
            } else {
                text = g.apps[h.idx].name;
            }
        }
        HGDIOBJ of = SelectObject(g.memDC, g.fText);
        SIZE ext = {};
        GetTextExtentPoint32W(g.memDC, text.c_str(), (int)text.size(), &ext);
        SelectObject(g.memDC, of);
        int tw = std::min((int)ext.cx + S(26), g.w - 2 * S(8)), th = S(32);
        bool row = h.type == HT::SetAppRow || h.type == HT::AllAppRow || h.type == HT::MRow || h.type == HT::PlSongRow || h.type == HT::AllSongRow;
        int x = row ? h.rc.left + S(40) : (h.rc.left + h.rc.right) / 2 - tw / 2;
        int y = h.rc.top - th - S(2);
        int minY = h.type == HT::Bluetooth || h.type == HT::MediaPhone || h.type == HT::Wifi || h.type == HT::Battery ? 0 : h.pane == 1 ? g.lTop : h.pane == 2 ? g.rTop : ContentTop();
        if (y < minY) y = h.rc.bottom + S(2);
        x = std::max(S(8), std::min(x, g.w - tw - S(8)));
        DrawFloatBox({x, y, x + tw, y + th}, text, -1);
    }
    // Gezogene App folgt der Maus
    if (g.dragging && g.dragApp >= 0) {
        int gw = std::min(g.w / 2 - S(24), S(280)), gh = S(42);
        int gx = std::max(0, std::min(g.w - gw, (int)g.mousePt.x - S(24))), gy = g.mousePt.y - gh / 2;
        if (g.dragMusic) {
            const Song* s = SongAt(g.dragApp);
            DrawFloatBox({gx, gy, gx + gw, gy + gh}, s ? s->title : L"", -1);
        } else {
            DrawFloatBox({gx, gy, gx + gw, gy + gh}, g.apps[g.dragApp].name, g.dragApp);
        }
    }
}

// Nur Position/Deckkraft ändern (Animation) oder neuen Inhalt übergeben
static void PushLayer(bool content) {
    POINT dst = {g.curX, g.y};
    SIZE sz = {g.w, g.h};
    POINT src = {0, 0};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, g.fade, AC_SRC_ALPHA};
    if (content && g.memDC)
        UpdateLayeredWindow(g.hwnd, nullptr, &dst, &sz, g.memDC, &src, 0, &bf, ULW_ALPHA);
    else
        UpdateLayeredWindow(g.hwnd, nullptr, &dst, nullptr, nullptr, nullptr, 0, &bf, ULW_ALPHA);
}

static void UpdateChatWindow() {
    if (g.open && !g.closing && !g.animating && g.view == View::Chat && g.chatRect.bottom > g.chatRect.top) {
        RECT wr;
        GetWindowRect(g.hwnd, &wr);
        RECT sr = {wr.left + g.chatRect.left, wr.top + g.chatRect.top, wr.left + g.chatRect.right, wr.top + g.chatRect.bottom};
        Chat_Show(g.hwnd, sr, S(12));
    } else {
        Chat_Hide();
    }
}

static void Render() {
    g.redrawPending = false;
    KillTimer(g.hwnd, T_REDRAW);
    if (!g.open || g.w <= 0 || g.h <= 0) return;
    if (!EnsureDib(g.w, g.h)) return;
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    // 1. Flächen + Text, 2. Glas/Farbe/Bild/Transparenz, 3. Symbole mit Alphakanal, 4. Kurzinfo
    LARGE_INTEGER ts[6];
    QueryPerformanceCounter(&ts[0]);
    DrawScene(g.maskDC, g.w, g.h);
    GdiFlush();
    QueryPerformanceCounter(&ts[1]);
    Compose(g.w, g.h);
    QueryPerformanceCounter(&ts[2]);
    for (auto& q : g.iconQueue) {
        SaveDC(g.memDC);
        IntersectClipRect(g.memDC, q.clip.left, q.clip.top, q.clip.right, q.clip.bottom);
        if (q.idx == -2) {  // Albumbild des Musik-Players (schon in Anzeigegröße)
            if (g.mediaArt) {
                HDC m = CreateCompatibleDC(g.memDC);
                HGDIOBJ o = SelectObject(m, g.mediaArt);
                BITMAP bm = {};
                GetObjectW(g.mediaArt, sizeof(bm), &bm);
                BLENDFUNCTION bf = {AC_SRC_OVER, 0, (BYTE)(255 * q.fade), AC_SRC_ALPHA};
                AlphaBlend(g.memDC, q.x, q.y, q.sz, q.sz, m, 0, 0, bm.bmWidth, bm.bmHeight, bf);
                SelectObject(m, o);
                DeleteDC(m);
            }
        } else if (q.bmp) {  // Cover (schon in Anzeigegröße)
            HDC m = CreateCompatibleDC(g.memDC);
            HGDIOBJ o = SelectObject(m, q.bmp);
            BLENDFUNCTION bf = {AC_SRC_OVER, 0, (BYTE)(255 * q.fade), AC_SRC_ALPHA};
            AlphaBlend(g.memDC, q.x, q.y, q.sz, q.sz, m, 0, 0, q.sz, q.sz, bf);
            SelectObject(m, o);
            DeleteDC(m);
        } else {
            BlitIcon(g.memDC, q.idx, q.x, q.y, q.sz, q.fade);
        }
        RestoreDC(g.memDC, -1);
    }
    GdiFlush();
    QueryPerformanceCounter(&ts[3]);
    ApplyText(g.w, g.h);
    DrawOverlays();
    GdiFlush();
    QueryPerformanceCounter(&ts[4]);
    PushLayer(true);
    QueryPerformanceCounter(&ts[5]);
    {
        LARGE_INTEGER fq2; QueryPerformanceFrequency(&fq2);
        for (int i = 0; i < 5; i++) g.stageMs[i] += (ts[i + 1].QuadPart - ts[i].QuadPart) * 1000.0 / fq2.QuadPart;
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    g.renderMsSum += (t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
    g.renderFrames++;
    UpdateChatWindow();
}

// gebündeltes Neuzeichnen (z. B. wenn viele Symbole nacheinander eintreffen)
static void Redraw() {
    if (g.redrawPending) return;
    g.redrawPending = true;
    SetTimer(g.hwnd, T_REDRAW, 16, nullptr);
}

static void Refresh() {
    Layout();
    ClampScroll();
    Layout();
    Render();
}

// ---------------------------------------------------------------- Fenster-Logik

static void SetDpi(UINT dpi, bool force = false) {
    if (dpi == g.dpi && g.fText && !force) return;
    g.dpi = dpi;
    ClearTextCache();  // Schriften ändern sich
    for (HFONT* f : {&g.fTitle, &g.fText, &g.fHead, &g.fSmall, &g.fIcon, &g.fGlyph, &g.fGlyphL, &g.fBarTitle, &g.fLyric, &g.fNpTitle})
        if (*f) { DeleteObject(*f); *f = nullptr; }
    auto mk = [](int px, int weight) {
        if (g.fontBold) weight = std::min(900, weight + 200);  // "fette Schrift": alles eine Stufe kräftiger
        return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH, g.fontName.c_str());
    };
    g.fTitle = mk(22, FW_SEMIBOLD);
    g.fText = mk(15, FW_NORMAL);
    g.fHead = mk(15, FW_SEMIBOLD);
    g.fSmall = mk(12, FW_NORMAL);
    g.fIcon = mk(18, FW_SEMIBOLD);
    g.fBarTitle = mk(18, FW_SEMIBOLD);  // Songtitel in der Musikleiste
    g.fLyric = mk(19, FW_BOLD);         // Songtext in "Läuft gerade"
    g.fNpTitle = mk(24, FW_BOLD);       // Songtitel in "Läuft gerade"
    // Symbolschrift (Zahnrad, Pfeile): Windows 11 "Segoe Fluent Icons", sonst "Segoe MDL2 Assets"
    if (g.glyphFont.empty()) {
        HDC dc = GetDC(nullptr);
        LOGFONTW lf = {};
        lf.lfCharSet = DEFAULT_CHARSET;
        lstrcpyW(lf.lfFaceName, L"Segoe Fluent Icons");
        bool found = false;
        EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int {
            *(bool*)p = true;
            return 0;
        }, (LPARAM)&found, 0);
        ReleaseDC(nullptr, dc);
        g.glyphFont = found ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    }
    g.fGlyph = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           ANTIALIASED_QUALITY, DEFAULT_PITCH, g.glyphFont.c_str());
    g.fGlyphL = CreateFontW(-S(20), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            ANTIALIASED_QUALITY, DEFAULT_PITCH, g.glyphFont.c_str());
}

static bool IsDesktopWindow(HWND w) {
    if (!w) return true;
    wchar_t cls[64] = {};
    GetClassNameW(w, cls, 64);
    return !lstrcmpW(cls, L"Progman") || !lstrcmpW(cls, L"WorkerW") || !lstrcmpW(cls, L"Shell_TrayWnd") ||
           !lstrcmpW(cls, L"Shell_SecondaryTrayWnd");
}

// Kurzes Protokoll: nur wenn die Maus am Rand verweilt, max. 64 KB
static void Log(const std::wstring& line) {
    std::wstring path = g.ini.substr(0, g.ini.find_last_of(L'\\')) + L"\\HoverBar.log";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    bool big = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 64 * 1024;
    HANDLE f = CreateFileW(path.c_str(), big ? GENERIC_WRITE : FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           big ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t ts[32];
    wsprintfW(ts, L"%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    std::wstring s = ts + line + L"\r\n";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &u[0], n, nullptr, nullptr);
    DWORD wr;
    WriteFile(f, u.data(), (DWORD)u.size(), &wr, nullptr);
    CloseHandle(f);
    // Entwicklung: liegt eine Datei "debuglog" neben der exe, zusätzlich dort mitschreiben
    static int dbg = -1;
    static std::wstring dbgPath;
    if (dbg < 0) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir = exe;
        dir = dir.substr(0, dir.find_last_of(L'\\') + 1);
        dbg = GetFileAttributesW((dir + L"debuglog").c_str()) != INVALID_FILE_ATTRIBUTES;
        dbgPath = dir + L"HoverBar.log";
    }
    if (dbg) {
        HANDLE d = CreateFileW(dbgPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (d != INVALID_HANDLE_VALUE) {
            WriteFile(d, u.data(), (DWORD)u.size(), &wr, nullptr);
            CloseHandle(d);
        }
    }
}

static bool FullscreenActive() {
    QUERY_USER_NOTIFICATION_STATE st = QUNS_ACCEPTS_NOTIFICATIONS;
    SHQueryUserNotificationState(&st);
    return st == QUNS_BUSY || st == QUNS_RUNNING_D3D_FULL_SCREEN || st == QUNS_PRESENTATION_MODE;
}

static bool OpenAllowed(HMONITOR mon) {
    HWND fg = GetForegroundWindow();
    wchar_t cls[64] = L"(keins)";
    if (fg) GetClassNameW(fg, cls, 64);
    std::wstring info = L"Rand erreicht. Vordergrund=" + std::wstring(cls);
    // Nie über Vollbild-Spielen, Videos oder Präsentationen öffnen
    if (FullscreenActive()) {
        Log(info + L" -> blockiert (Vollbild)");
        return false;
    }
    if (IsDesktopWindow(fg)) { Log(info + L" -> öffnen"); return true; }
    if (g.onlyDesktop) { Log(info + L" -> blockiert (nicht Desktop)"); return false; }
    // randloses Vollbild erkennen
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    RECT r;
    if (GetWindowRect(fg, &r) && r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
        r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom) {
        Log(info + L" -> blockiert (randloses Vollbild)");
        return false;
    }
    Log(info + L" -> öffnen");
    return true;
}

static void ForceForeground() {
    HWND fg = GetForegroundWindow();
    DWORD me = GetCurrentThreadId();
    DWORD ft = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    if (ft && ft != me) AttachThreadInput(me, ft, TRUE);
    SetForegroundWindow(g.hwnd);
    SetActiveWindow(g.hwnd);
    SetFocus(g.hwnd);
    if (ft && ft != me) AttachThreadInput(me, ft, FALSE);
}

// Flüssige Animation: ein Bild pro Bildschirmaktualisierung (DwmFlush wartet auf den nächsten Frame)
static void Animate(int from, int to, DWORD dur, bool opening) {
    g.animating = true;
    LARGE_INTEGER f, s, n;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&s);
    for (;;) {
        QueryPerformanceCounter(&n);
        double t = (double)(n.QuadPart - s.QuadPart) * 1000.0 / f.QuadPart / dur;
        if (t > 1) t = 1;
        double e;
        if (opening) {
            e = 1 - std::pow(1 - t, 4);  // schnell los, sanft auslaufen
            g.fade = (BYTE)(255 * std::min(1.0, t * 2.2));
        } else {
            e = t * t * (3 - 2 * t) * 0.35 + t * t * 0.65;  // sanft anfahren
            g.fade = (BYTE)(255 * (1 - std::max(0.0, (t - 0.25) / 0.75)));
        }
        g.curX = from + (int)std::lround((to - from) * e);
        PushLayer(false);
        if (t >= 1) break;
        // Symbole dürfen währenddessen weiter eintreffen
        MSG m;
        while (PeekMessageW(&m, nullptr, WM_APP_LIST, WM_APP_DONE, PM_REMOVE)) DispatchMessageW(&m);
        if (FAILED(DwmFlush())) Sleep(8);
    }
    g.animating = false;
}

static void StopScrollAnim();
static int CreatePlaylist(std::wstring name);
static void MusicChanged();
static void FreeCovers();
static void BuildMusic();

// Gekoppelte Bluetooth-Geräte und ob sie gerade verbunden sind (ohne Suche, dauert nur Millisekunden)
static void RefreshBluetooth() {
    g.btDevices.clear();
    BLUETOOTH_FIND_RADIO_PARAMS rp = {sizeof(rp)};
    HANDLE radio = nullptr;
    HBLUETOOTH_RADIO_FIND rf = BluetoothFindFirstRadio(&rp, &radio);
    g.btRadio = rf != nullptr;
    if (rf) {
        CloseHandle(radio);
        BluetoothFindRadioClose(rf);
    }
    BLUETOOTH_DEVICE_SEARCH_PARAMS sp = {sizeof(sp)};
    sp.fReturnAuthenticated = TRUE;
    sp.fReturnRemembered = TRUE;
    sp.fReturnConnected = TRUE;
    sp.fIssueInquiry = FALSE;
    BLUETOOTH_DEVICE_INFO di = {sizeof(di)};
    HBLUETOOTH_DEVICE_FIND df = BluetoothFindFirstDevice(&sp, &di);
    if (!df) return;
    do {
        if (di.szName[0]) g.btDevices.push_back({di.szName, di.fConnected != FALSE});
        di.dwSize = sizeof(di);
    } while (BluetoothFindNextDevice(df, &di));
    BluetoothFindDeviceClose(df);
    std::stable_sort(g.btDevices.begin(), g.btDevices.end(), [](const State::BtDevice& a, const State::BtDevice& b) { return a.connected > b.connected; });
}

// Windows-Schnellmenü "Bluetooth" (Geräteliste mit Verbinden-Knöpfen), sonst die Einstellungsseite
// App-Logo als vormultipliziertes 32-Bit-Bild in Anzeigegröße (aus dem eingebauten Symbol)
static HBITMAP MakeLogoBitmap(int px) {
    HICON ic = (HICON)LoadImageW(g.inst, MAKEINTRESOURCEW(1), IMAGE_ICON, px, px, LR_DEFAULTCOLOR);
    if (!ic) return nullptr;
    ICONINFO ii = {};
    HBITMAP out = nullptr;
    if (GetIconInfo(ic, &ii) && ii.hbmColor) {
        BITMAPINFO bi = {};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), px, -px, 1, 32, BI_RGB};
        std::vector<uint32_t> px32((size_t)px * px);
        HDC sdc = GetDC(nullptr);
        if (GetDIBits(sdc, ii.hbmColor, 0, px, px32.data(), &bi, DIB_RGB_COLORS)) {
            void* bits = nullptr;
            out = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (out) {
                uint32_t* d = (uint32_t*)bits;
                for (size_t k = 0; k < px32.size(); k++) {  // vormultiplizieren
                    uint32_t c = px32[k], a = c >> 24;
                    d[k] = (a << 24) | ((((c >> 16) & 255) * a / 255) << 16) | ((((c >> 8) & 255) * a / 255) << 8) | ((c & 255) * a / 255);
                }
            }
        }
        ReleaseDC(nullptr, sdc);
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    DestroyIcon(ic);
    return out;
}

// WLAN/LAN und Akku (nur beim Öffnen und alle paar Sekunden, solange offen)
static void RefreshStatus() {
    g.statusAt = GetTickCount64();
    Net_Status(g.net, g.netBars, g.netInternet, g.netName);
    SYSTEM_POWER_STATUS ps = {};
    g.batt = -1;
    if (GetSystemPowerStatus(&ps) && !(ps.BatteryFlag & 128) && ps.BatteryFlag != 255 && ps.BatteryLifePercent <= 100) {
        g.batt = ps.BatteryLifePercent;
        g.battCharging = ps.ACLineStatus == 1;
    }
}

static void OpenBluetoothPanel() {
    if ((INT_PTR)ShellExecuteW(nullptr, L"open", L"ms-actioncenter:controlcenter/bluetooth", nullptr, nullptr, SW_SHOWNORMAL) <= 32)
        ShellExecuteW(nullptr, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL);
}

static void FinishClose() {
    // Ansicht merken (Seite, Ordner, Scrollstand, Suche): innerhalb von 10 s wieder öffnen -> alles noch da
    g.resume = {GetTickCount64(), g.view == View::SongEdit ? View::Music : g.view, g.curFolder, g.scroll, g.scrollR, g.libTab, g.setTab, g.curPlaylist,
                g.npFolder, g.npReturn, g.query};
    ShowWindow(g.hwnd, SW_HIDE);
    StopScrollAnim();
    ClearTextCache();  // Arbeitsspeicher frei, solange zu
    Chat_Hide();
    SetTimer(g.hwnd, T_CHATFREE, 10000, nullptr);  // Claude-Webansicht erst nach 10 s beenden (so lange geht das Wiederöffnen sofort); Anmeldung bleibt gespeichert
    KillTimer(g.hwnd, T_RUN);
    g.showRecent = false;  // "Zuletzt genutzt" beim nächsten Öffnen wieder versteckt
    g.running.clear();
    g.appMatch.clear();
    KillTimer(g.hwnd, T_VOL);
    g.volExp = g.volTarget = 0;  // Lautstärke beim nächsten Öffnen wieder eingeklappt
    g.volLeaveAt = 0;
    Pool_Stop();       // Hilfs-Threads beenden
    FreeCovers();
    KillTimer(g.hwnd, T_PLAYER);
    KillTimer(g.hwnd, T_RELOAD);  // beim nächsten Öffnen wird ohnehin neu gelesen
    g.reload = 0;
    g.ctlCapture = -1;
    if (g.grabCapture) { g.grabCapture = false; RegisterGrabKey(); }  // Aufnahme abgebrochen
    g.pausedMinutes = 0;
    if (Player_State().active) SetTimer(g.hwnd, T_PIDLE, 60000, nullptr);  // pausierte Musik nach 10 Min. beenden
    Media_Close();
    SetTimer(g.hwnd, T_FREE, 3000, nullptr);
    if (g.mediaArt) { DeleteObject(g.mediaArt); g.mediaArt = nullptr; }
    g.media = MediaInfo();
    timeEndPeriod(1);
    g.open = g.closing = false;
    g.tracking = false;
    g.creatingFolder = false;
    FreeBuffer();
    std::vector<uint32_t>().swap(g.bdRaw);
    std::vector<uint32_t>().swap(g.bdBlur);
    std::vector<int>().swap(g.allList);
    g.bdW = g.bdH = 0;
    // App-Liste und Symbole bleiben vorgeladen. Ist die Liste älter als 10 Minuten (neue Programme?),
    // wird sie jetzt – während die Leiste zu ist – im Hintergrund erneuert und danach ausgetauscht.
    if (!g.loading && GetTickCount64() - g.lastLoad > 10 * 60 * 1000) StartLoad();
    EfficiencyMode(true);
    TrimMemory();
}

static void CloseBar(const wchar_t* why = L"?") {
    if (!g.open || g.closing) return;
    if (g.animating) { g.closeAfterAnim = true; return; }
    Log(std::wstring(L"Schließen: ") + why);
    g.closing = true;
    KillTimer(g.hwnd, T_WATCH);
    KillTimer(g.hwnd, T_CARET);
    Animate(g.curX, g.xHidden, kCloseMs[g.animSpeed], false);
    FinishClose();
}

// Breite passend zur Ansicht setzen (Ordner-Einstellungen sind breiter)
static void ApplyWidth() {
    int w = WantedWidth();
    if (w == g.w) return;
    g.w = w;
    g.xShown = g.workRight - g.w - g.margin;
    g.curX = g.xShown;
}

static void OpenBar(POINT pt) {
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);

    EfficiencyMode(false);
    KillTimer(g.hwnd, T_FREE);
    SetDpi(dx);

    g.margin = S(10);
    int m = g.margin;
    int workW = mi.rcWork.right - mi.rcWork.left;
    g.wNormal = std::min(workW - 2 * m, std::max(S(380), workW / 3) + RailW());  // ca. ein Drittel des Bildschirms + Navigationsleiste
    g.wWide = std::min(workW - 2 * m, std::max(g.wNormal, workW * 46 / 100 + RailW()));
    g.workRight = mi.rcWork.right;
    g.w = g.wNormal;
    g.h = (mi.rcWork.bottom - mi.rcWork.top) - 2 * m;
    g.y = mi.rcWork.top + m;
    g.xShown = g.workRight - g.w - m;
    g.xHidden = mi.rcMonitor.right;
    g.monLeft = mi.rcMonitor.left;
    g.monW = mi.rcMonitor.right - mi.rcMonitor.left;

    // Glas: Hintergrund aufnehmen, solange die Leiste noch unsichtbar ist (breit genug für alle Ansichten)
    CaptureBackdrop(g.workRight - m - g.wWide, g.y, g.wWide, g.h);
    RefreshBluetooth();
    RefreshStatus();
    // Musikleiste: eigene Musik (Musik-Ordner) oder, wenn eingestellt, nur Spotify
    if (SpotifyMode() && g.musicOn) {
        Media_SetSpotifyOnly(true);
        Media_Open(g.hwnd, S(kBarArt));
    }
    Music_Init(g.hwnd);
    LoadFavRecent();
    Music_ImportEdits();
    BuildMusic();
    Player_SetVolume(VolumeGain(g.volume));
    g.pst = Player_State();
    KillTimer(g.hwnd, T_PIDLE);
    SetTimer(g.hwnd, T_PLAYER, 400, nullptr);

    KillTimer(g.hwnd, T_CHATFREE);
    g.view = View::Folders;
    g.curFolder = -1;
    StopScrollAnim();
    g.scroll = g.scrollR = 0;
    g.namingFor = -1;
    g.creatingFolder = false;
    bool resumed = g.resume.at && GetTickCount64() - g.resume.at < 10000 && g.resume.view != View::Folders;
    g.resume.at = 0;
    timeBeginPeriod(1);  // genauere Timer für flüssiges Scrollen, nur solange die Leiste offen ist
    g.hot = g.pressed = -1;
    g.query.clear();
    g.results.clear();
    g.caretOn = true;
    if (g.appsOn && !g.loaded && !g.loading) StartLoad();
    if (resumed) {  // vor weniger als 10 s geschlossen: dieselbe Seite wieder zeigen
        const auto& r = g.resume;
        g.view = r.view;
        g.curFolder = r.folder;
        g.libTab = r.libTab;
        g.setTab = r.setTab;
        g.curPlaylist = r.curPlaylist;
        g.npReturn = r.npReturn;
        g.npFolder = r.npFolder;
        g.query = r.query;
        if (g.view == View::Search) UpdateSearch();
        ApplyWidth();
        g.scroll = r.scroll;
        g.scrollR = r.scrollR;
        Log(L"Wiederöffnen binnen 10 s: letzte Ansicht bleibt");
    }
    if (g.timerRinging) g.view = View::Timer;
    else {
        if (g.view == View::Calendar && !g.calendarOn) g.view = View::Folders;
        if (!g.appsOn && (g.view == View::Folders || g.view == View::Folder || g.view == View::Search || g.view == View::AllApps || g.view == View::Storage)) {
            g.view = View::Music;
            g.libTab = 0;
        }
    }

    g.open = true;
    g.closing = false;
    g.closeAfterAnim = false;
    g.curX = g.xHidden;
    g.fade = 0;
    Refresh();
    SetWindowPos(g.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    ForceForeground();
    Animate(g.xHidden, g.xShown, kOpenMs[g.animSpeed], true);
    Render();
    g.watchTick = 0;
    SetTimer(g.hwnd, T_WATCH, 50, nullptr);
    SetTimer(g.hwnd, T_CARET, 530, nullptr);
    if (g.closeAfterAnim) {
        g.closeAfterAnim = false;
        CloseBar(L"während des Öffnens");
    }
}

static void Launch(int idx) {
    App& a = g.apps[idx];
    AddRecent(L"a:" + a.id);
    std::wstring target = a.file.empty() ? L"shell:AppsFolder\\" + a.id : a.file;
    HINSTANCE r = ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32 && a.id.find(L"://") != std::wstring::npos)
        ShellExecuteW(nullptr, L"open", a.id.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    a.launches++;
    IniSet(L"Starts", a.id, std::to_wstring(a.launches).c_str());
    CloseBar(L"App gestartet");
}

static void StopScrollAnim() {
    if (!g.scrollAnim) return;
    KillTimer(g.hwnd, T_SCROLL);
    g.scrollAnim = false;
}

// Statistiken für den offenen Spielmodus-Ordner holen
static void LoadGameStats() {
    g.gstats.clear();
    g.gMaxSize = 1;
    g.gMaxMin = 1;
    if (g.view != View::Folder || g.curFolder < 0 || g.curFolder >= (int)g.folders.size()) return;
    if (!FolderGameMode(g.folders[g.curFolder].name)) return;
    for (int i : g.folders[g.curFolder].items) {
        GameStats s = Games_Get({g.apps[i].id, g.apps[i].file, g.apps[i].name});
        g.gstats[i] = s;
        g.gMaxSize = std::max(g.gMaxSize, s.size);
        g.gMaxMin = std::max(g.gMaxMin, s.minutes);
    }
}

// welche Apps laufen gerade (eigene Prozesse des Benutzers), mit Arbeitsspeicher
static bool RefreshRunning() {
    std::map<int, std::pair<std::vector<DWORD>, SIZE_T>> now;
    if (!g.appMatch.empty()) {
        auto procs = Apps_Processes();
        for (auto& m : g.appMatch)
            for (auto& p : procs)
                if (Apps_MatchProcess(m.second, p.path)) {
                    auto& r = now[m.first];
                    r.first.push_back(p.pid);
                    r.second += p.ram;
                }
    }
    bool changed = now.size() != g.running.size();
    if (!changed)
        for (auto& r : now) {
            auto it = g.running.find(r.first);
            if (it == g.running.end() || it->second.first != r.second.first || it->second.second / (8 << 20) != r.second.second / (8 << 20)) { changed = true; break; }
        }
    g.running = std::move(now);
    return changed;
}
static SIZE_T RunningRam(int idx) {
    auto it = g.running.find(idx);
    return it == g.running.end() ? 0 : it->second.second;
}

static void SortAppSizes() {
    std::sort(g.appSizes.begin(), g.appSizes.end(), [](const std::pair<int, long long>& a, const std::pair<int, long long>& b) {
        if (g.onlyRunning) {
            SIZE_T ra = RunningRam(a.first), rb = RunningRam(b.first);
            if (ra != rb) return ra > rb;
        }
        if (g.sortBySize && a.second != b.second) return a.second > b.second;
        return Lower(g.apps[a.first].name) < Lower(g.apps[b.first].name);
    });
}
// Größe aller sichtbaren Apps (Launcher, Windows-Programmliste, sonst Ordner im Hintergrund messen)
static void LoadAppSizes() {
    g.appSizes.clear();
    g.appMax = 1;
    g.appTotal = 0;
    if (g.view != View::AllApps) {
        g.appMatch.clear();
        g.running.clear();
        KillTimer(g.hwnd, T_RUN);
        return;
    }
    for (int i = 0; i < (int)g.apps.size(); i++) {
        if (g.apps[i].hidden) continue;
        GameRef ref = {g.apps[i].id, g.apps[i].file, g.apps[i].name};
        long long s = Apps_Size(ref, g.apps[i].target);
        g.appSizes.push_back({i, s});
        g.appMatch[i] = Apps_Matcher(ref, g.apps[i].target);
        if (s > 0) { g.appMax = std::max(g.appMax, s); g.appTotal += s; }
    }
    RefreshRunning();
    SortAppSizes();
    SetTimer(g.hwnd, T_RUN, 2000, nullptr);  // laufende Apps alle 2 s auffrischen, solange die Liste offen ist
}

// laufende App beenden: erst normal schließen (wie das X), reagiert sie nicht, auf Wunsch erzwingen
static void EndApp(int idx) {
    auto it = g.running.find(idx);
    if (it == g.running.end()) return;
    std::vector<DWORD> pids = it->second.first;
    std::wstring name = g.apps[idx].name;
    if (!ConfirmBox(L"\u201E" + name + L"\u201C beenden?\nNicht gespeicherte Änderungen in der App können verloren gehen.")) return;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    bool closed = Apps_CloseGracefully(pids, 3000);
    if (!closed) {
        if (ConfirmBox(L"\u201E" + name + L"\u201C hat sich nicht von selbst beendet (läuft z. B. im Infobereich weiter).\nJetzt erzwingen?")) {
            if (!Apps_Kill(pids)) {
                g.menuOpen = true;
                MessageBoxW(g.hwnd, (L"\u201E" + name + L"\u201C läuft mit Administratorrechten und kann von HoverBar nicht beendet werden.").c_str(),
                            L"HoverBar", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
                g.menuOpen = false;
            }
        }
    }
    Log(L"Beenden: " + name + (closed ? L" (normal geschlossen)" : L""));
    RefreshRunning();
    SortAppSizes();
    Refresh();
}

static void ShowView(View v, int folder);
// Seite der Navigationsleiste öffnen
static void GoToPage(int p) {
    g.query.clear();
    g.creatingFolder = false;
    g.namingFor = -1;
    if (p == 1 && g.musicOn) ShowView(View::Music, -1);
    else if (p == 3 && !WizTree_Exe().empty()) ShowView(View::Storage, -1);
    else if (p == 4) ShowView(View::Chat, -1);
    else if (p == 5) ShowView(View::Timer, -1);
    else if (p == 2) {
        if (!g.eventsLoaded) LoadEvents();
        if (!g.calY) {
            int t = Today();
            g.calY = t / 10000;
            g.calM = t / 100 % 100;
            g.calSel = t;
        }
        ShowView(View::Calendar, -1);
    }
    else ShowView(View::Folders, -1);
}

static void ShowView(View v, int folder = -1) {
    StopScrollAnim();
    // ohne App-Launcher führen alle Wege zu den Apps in die Bibliothek
    if (v == View::Calendar && !g.calendarOn) v = View::Folders;
    if (!g.appsOn && (v == View::Folders || v == View::Folder || v == View::Search || v == View::AllApps || v == View::Storage)) {
        v = View::Music;
        folder = -1;
    }
    // Bibliothek startet auf "Alle Songs" (zurück aus einer Playlist bleibt man bei den Playlists)
    if (v == View::Music && g.view != View::MusicList && g.view != View::SongEdit && g.view != View::MusicSettings) g.libTab = 0;
    g.view = v;
    g.curFolder = folder;
    LoadGameStats();
    LoadAppSizes();
    g.scroll = g.scrollR = 0;
    g.hot = -1;
    g.tipShown = false;
    ApplyWidth();
    Refresh();
}

static void LeaveNowPlaying() {
    View v = g.npReturn == View::NowPlaying || g.npReturn == View::Search ? View::Folders : g.npReturn;
    ShowView(v, v == View::Folder ? g.npFolder : -1);
}

static void OnQueryChanged() {
    g.caretOn = true;
    SetTimer(g.hwnd, T_CARET, 530, nullptr);
    if (MusicView() && g.view != View::MusicSettings) { Render(); return; }
    if (g.view == View::Settings || g.view == View::MusicSettings) {
        if (!g.creatingFolder) g.scrollR = 0;
        Refresh();
        return;
    }
    if (g.namingFor >= 0) { Render(); return; }
    UpdateSearch();
    if (!g.query.empty()) g.view = View::Search;
    else g.view = g.curFolder >= 0 ? View::Folder : View::Folders;
    g.scroll = 0;
    g.hot = -1;
    Refresh();
}

// Neuer Ordner: in der Hauptansicht über das Suchfeld, in den Einstellungen über das Feld unten links
static void BeginNaming(int appIdx) {
    g.namingFor = appIdx;
    g.query.clear();
    if (g.view == View::MusicSettings) {
        g.creatingFolder = true;
    } else if (g.view == View::Settings) {
        g.creatingFolder = true;
        if (g.setTab != 1) { g.setTab = 1; ApplyWidth(); }
    }
    SetForegroundWindow(g.hwnd);
    SetFocus(g.hwnd);
    Refresh();
}

static void CancelNaming() {
    g.namingFor = -1;
    g.creatingFolder = false;
    g.query.clear();
    OnQueryChanged();
}

static void FinishNaming() {
    if (g.view == View::MusicSettings) {  // neue Playlist
        int song = g.namingFor;
        std::wstring name = g.query;
        g.namingFor = -1;
        g.creatingFolder = false;
        g.query.clear();
        int pl = CreatePlaylist(name);
        if (pl >= 0 && song >= 0 && song < (int)Music_Songs().size()) {
            Music_Playlists()[pl].ids.push_back(Music_Songs()[song].id);
            MusicChanged();
        }
        g.scroll = 1 << 20;
        Refresh();
        return;
    }
    std::wstring name = g.query;
    name.erase(std::remove(name.begin(), name.end(), L'|'), name.end());
    while (!name.empty() && name.back() == L' ') name.pop_back();
    while (!name.empty() && name.front() == L' ') name.erase(0, 1);
    int idx = g.namingFor;
    g.namingFor = -1;
    g.creatingFolder = false;
    g.query.clear();
    if (!name.empty()) {
        CreateFolder(name);
        if (idx >= 0) {
            for (int f = 0; f < (int)g.allFolders.size(); f++)
                if (g.allFolders[f].name == name) { MoveApp(idx, f, (int)g.allFolders[f].items.size()); break; }
        }
    }
    if (g.view != View::Settings) { g.view = View::Folders; g.curFolder = -1; }
    if (g.view == View::Settings && g.setTab == 1) g.scroll = 1 << 20;  // ans Ende: dort steht der neue Ordner
    Refresh();
}

static int TrackMenu(HMENU m) {
    POINT pt;
    GetCursorPos(&pt);
    g.menuOpen = true;
    SetForegroundWindow(g.hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g.hwnd, nullptr);
    g.menuOpen = false;
    DestroyMenu(m);
    return cmd;
}

// Kleines Eingabefenster (eine Zeile), true bei OK
static std::wstring s_inText;
static bool s_inDone = false, s_inOk = false;
static LRESULT CALLBACK InputProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
        if (LOWORD(wp) == IDOK) {
            wchar_t b[128] = {};
            GetDlgItemTextW(w, 100, b, 128);
            s_inText = b;
            s_inOk = true;
        }
        s_inDone = true;
        return 0;
    }
    if (m == WM_CLOSE) { s_inDone = true; return 0; }
    if (m == WM_CTLCOLORSTATIC || m == WM_CTLCOLORDLG) {
        SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    return DefWindowProcW(w, m, wp, lp);
}
static bool InputBox(const std::wstring& title, const std::wstring& prompt, std::wstring& text) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = InputProc;
        wc.hInstance = g.inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.hIcon = LoadIconW(g.inst, MAKEINTRESOURCEW(1));
        wc.lpszClassName = L"HoverBarInput";
        RegisterClassW(&wc);
        reg = true;
    }
    int W = S(420), H = S(170);
    RECT br;
    GetWindowRect(g.hwnd, &br);
    int x = (br.left + br.right - W) / 2, y = (br.top + br.bottom - H) / 2;
    HWND w = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, L"HoverBarInput", title.c_str(), WS_POPUP | WS_CAPTION | WS_SYSMENU,
                             x, y, W, H, g.hwnd, nullptr, g.inst, nullptr);
    if (!w) return false;
    HFONT f = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    RECT cr;
    GetClientRect(w, &cr);
    int p = S(14), cw = cr.right - 2 * p;
    HWND st = CreateWindowW(L"STATIC", prompt.c_str(), WS_CHILD | WS_VISIBLE, p, p, cw, S(40), w, nullptr, g.inst, nullptr);
    HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, p, p + S(44), cw, S(28),
                              w, (HMENU)100, g.inst, nullptr);
    int bw = S(100), bh = S(30), by = cr.bottom - p - bh;
    HWND ok = CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, cr.right - p - 2 * bw - S(8), by, bw, bh, w,
                            (HMENU)IDOK, g.inst, nullptr);
    HWND ca = CreateWindowW(L"BUTTON", L"Abbrechen", WS_CHILD | WS_VISIBLE | WS_TABSTOP, cr.right - p - bw, by, bw, bh, w, (HMENU)IDCANCEL,
                            g.inst, nullptr);
    for (HWND c : {st, ed, ok, ca}) SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
    SendMessageW(ed, EM_SETSEL, 0, -1);
    s_inDone = s_inOk = false;
    g.menuOpen = true;
    ShowWindow(w, SW_SHOW);
    SetForegroundWindow(w);
    SetFocus(ed);
    MSG msg;
    while (!s_inDone && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) { SendMessageW(w, WM_COMMAND, IDOK, 0); continue; }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { s_inDone = true; continue; }
        if (!IsDialogMessageW(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DestroyWindow(w);
    DeleteObject(f);
    g.menuOpen = false;
    SetForegroundWindow(g.hwnd);
    if (s_inOk) text = s_inText;
    return s_inOk;
}

// Bisherige Spielzeit eingeben: "120" = 120 Std, "12:30" = 12 Std 30 Min, "5,5" = 5,5 Std; leer = entfernen
static void AskManualPlaytime(int idx) {
    const App& a = g.apps[idx];
    int cur = Games_ManualMinutes(a.id);
    std::wstring t;
    if (cur >= 0) t = std::to_wstring(cur / 60) + (cur % 60 ? L":" + std::to_wstring(cur % 60) : L"");
    if (!InputBox(L"Bisherige Spielzeit – " + a.name,
                  L"Wie viele Stunden hast du schon gespielt? (z. B. 120, 12:30 oder 5,5)\nWird zur gemessenen Zeit addiert, leer = entfernen.", t))
        return;
    size_t s0 = t.find_first_not_of(L" "), s1 = t.find_last_not_of(L" ");
    t = s0 == std::wstring::npos ? L"" : t.substr(s0, s1 - s0 + 1);
    if (t.empty()) {
        Games_SetManualMinutes(a.id, -1);
    } else {
        std::replace(t.begin(), t.end(), L',', L'.');
        int minutes;
        size_t colon = t.find(L':');
        if (colon != std::wstring::npos) minutes = _wtoi(t.c_str()) * 60 + _wtoi(t.c_str() + colon + 1);
        else minutes = (int)(_wtof(t.c_str()) * 60 + 0.5);
        Games_SetManualMinutes(a.id, std::max(0, minutes));
    }
    LoadGameStats();
}

static void OpenLink(const std::wstring& url) {
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    AddRecent(L"l:" + url);
    CloseBar(L"Link geöffnet");
}
static void AskAddLink() {
    std::wstring t;
    if (!InputBox(L"Favoriten-Link hinzufügen", L"Adresse der Website (z. B. youtube.com oder https://…):", t)) return;
    size_t s0 = t.find_first_not_of(L" \t"), s1 = t.find_last_not_of(L" \t");
    if (s0 == std::wstring::npos) return;
    t = t.substr(s0, s1 - s0 + 1);
    if (t.find(L"://") == std::wstring::npos) t = L"https://" + t;
    State::FavLink l;
    l.url = t;
    l.host = LinkHost(t);
    l.name = LinkNiceName(l.host);
    if (l.host.empty() || l.host.find(L'.') == std::wstring::npos) return;
    g.links.push_back(l);
    SaveLinks();
    StartFavicon(l.host);
    Refresh();
}
static void LinkMenu(int k) {
    if (k < 0 || k >= (int)g.links.size()) return;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Öffnen");
    AppendMenuW(m, MF_STRING, 2, L"Umbenennen …");
    AppendMenuW(m, MF_STRING | (k > 0 ? 0 : MF_GRAYED), 3, L"Nach links");
    AppendMenuW(m, MF_STRING | (k + 1 < (int)g.links.size() ? 0 : MF_GRAYED), 4, L"Nach rechts");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 5, L"Entfernen");
    int cmd = TrackMenu(m);
    if (cmd == 1) { OpenLink(g.links[k].url); return; }
    if (cmd == 2) {
        std::wstring t = g.links[k].name;
        if (!InputBox(L"Link umbenennen", L"Name für " + g.links[k].host + L":", t) || t.empty()) return;
        g.links[k].name = t;
    } else if (cmd == 3) std::swap(g.links[k], g.links[k - 1]);
    else if (cmd == 4) std::swap(g.links[k], g.links[k + 1]);
    else if (cmd == 5) g.links.erase(g.links.begin() + k);
    else return;
    SaveLinks();
    Refresh();
}

// Termin eingeben/bearbeiten: "14:30 Zahnarzt", "9.15 Training" oder nur "Geburtstag Tom" (ganztägig)
static bool AskEvent(int date, State::CalEntry& e) {
    std::wstring t = e.time.empty() ? e.title : e.time + L" " + e.title;
    if (!InputBox(L"Termin – " + DateText(date), L"Uhrzeit und Titel, z. B. \u201E14:30 Zahnarzt\u201C (ohne Uhrzeit = ganztägig):", t)) return false;
    size_t s0 = t.find_first_not_of(L" \t");
    if (s0 == std::wstring::npos) return false;
    t = t.substr(s0);
    int hh = -1, mm = 0, used = 0;
    if (iswdigit(t[0])) {
        hh = _wtoi(t.c_str());
        size_t p = t.find_first_not_of(L"0123456789");
        if (p != std::wstring::npos && (t[p] == L':' || t[p] == L'.') && p + 1 < t.size() && iswdigit(t[p + 1])) {
            mm = _wtoi(t.c_str() + p + 1);
            used = (int)t.find_first_not_of(L"0123456789", p + 1);
        } else {
            used = (int)p;
        }
        if (used <= 0 || hh > 23 || mm > 59 || (used < (int)t.size() && t[used] != L' ')) hh = -1;  // keine Uhrzeit, sondern Teil des Titels
    }
    if (hh >= 0) {
        wchar_t b[8];
        swprintf(b, 8, L"%02d:%02d", hh, mm);
        e.time = b;
        t = t.substr(used);
        size_t s1 = t.find_first_not_of(L" \t");
        t = s1 == std::wstring::npos ? L"" : t.substr(s1);
    } else {
        e.time.clear();
    }
    e.title = t.empty() ? L"Termin" : t;
    e.date = date;
    return true;
}
static void EventMenu(int i) {
    if (i < 0 || i >= (int)g.events.size()) return;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Bearbeiten …");
    AppendMenuW(m, MF_STRING, 2, L"Auf morgen verschieben");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 3, L"Löschen");
    int cmd = TrackMenu(m);
    if (cmd == 1) {
        State::CalEntry e = g.events[i];
        if (!AskEvent(e.date, e)) return;
        g.events[i] = e;
    } else if (cmd == 2) {
        int d = g.events[i].date, y = d / 10000, mo = d / 100 % 100, dd = d % 100 + 1;
        if (dd > DaysIn(y, mo)) { dd = 1; if (++mo > 12) { mo = 1; y++; } }
        g.events[i].date = y * 10000 + mo * 100 + dd;
    } else if (cmd == 3) {
        g.events.erase(g.events.begin() + i);
    } else {
        return;
    }
    SaveEvents();
    Refresh();
}

// "+" bzw. Rechtsklick auf eine App: in welchen Ordner?
static void AddMenu(int idx) {
    App& a = g.apps[idx];
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Zu Ordner hinzufügen:");
    for (int f = 0; f < (int)g.allFolders.size(); f++)
        AppendMenuW(m, MF_STRING | (g.allFolders[f].name == a.folder ? MF_CHECKED : 0), 100 + f, g.allFolders[f].name.c_str());
    AppendMenuW(m, MF_STRING, 90, L"Neuer Ordner …");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (a.folder.empty() ? MF_GRAYED : 0), 1, L"Aus dem Ordner nehmen");
    bool overridden = !IniGet(L"Zuordnung", a.id).empty();
    AppendMenuW(m, MF_STRING | (overridden ? 0 : MF_GRAYED), 2, L"Automatisch einsortieren");
    AppendMenuW(m, MF_STRING, 3, a.hidden ? L"Wieder einblenden" : L"Ausblenden");
    int favK = FavIndexOf(idx), freeK = -1;
    for (int k = 3; k >= 0; k--)
        if (g.favs[k] < 0) freeK = k;
    if (favK >= 0) AppendMenuW(m, MF_STRING, 6, L"Von der Startseite entfernen");
    else AppendMenuW(m, MF_STRING | (freeK >= 0 ? 0 : MF_GRAYED), 7, L"Zur Startseite hinzufügen");
    bool gameFolder = g.view == View::Folder && g.curFolder >= 0 && g.curFolder < (int)g.folders.size() &&
                      FolderGameMode(g.folders[g.curFolder].name);
    std::wstring wizDir;
    if (!WizTree_Exe().empty()) {  // Ordner der App für die Baumansicht
        AppMatcher am = Apps_Matcher({a.id, a.file, a.name}, a.target);
        if (!am.dir.empty()) wizDir = am.dir;
        else if (!am.exe.empty() && !am.system) wizDir = am.exe.substr(0, am.exe.find_last_of(L'\\') + 1);
        if (!wizDir.empty()) AppendMenuW(m, MF_STRING, 9, L"In WizTree anzeigen");
    }
    if (gameFolder && !Games_IsSteam(a.id)) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, 8, L"Bisherige Spielzeit eingeben …");
    }
    if (g.view != View::Settings) {
        bool hasFile = !a.target.empty() && GetFileAttributesW(a.target.c_str()) != INVALID_FILE_ATTRIBUTES;
        AppendMenuW(m, MF_STRING | (hasFile ? 0 : MF_GRAYED), 4, L"Dateispeicherort öffnen");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, 5, L"Öffnen");
    }
    int cmd = TrackMenu(m);
    if (cmd >= 100 && cmd < 100 + (int)g.allFolders.size()) {
        int f = cmd - 100;
        if (g.apps[idx].folder != g.allFolders[f].name) MoveApp(idx, f, (int)g.allFolders[f].items.size());
    } else if (cmd == 90) {
        BeginNaming(idx);
        return;
    } else if (cmd == 1) {
        RemoveFromFolder(idx);
    } else if (cmd == 2) {
        IniSet(L"Zuordnung", a.id, nullptr);
        BuildFolders();
    } else if (cmd == 3) {
        IniSet(L"Ausgeblendet", a.id, a.hidden ? nullptr : L"1");
        BuildFolders();
        UpdateSearch();
    } else if (cmd == 4) {
        std::wstring args = L"/select,\"" + a.target + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        CloseBar(L"Dateispeicherort");
        return;
    } else if (cmd == 6) {
        SetFav(favK, -1);
    } else if (cmd == 7) {
        SetFav(freeK, idx);
    } else if (cmd == 8) {
        AskManualPlaytime(idx);
    } else if (cmd == 9) {
        if (WizTree_Open(wizDir)) { CloseBar(L"WizTree"); return; }
    } else if (cmd == 5) {
        Launch(idx);
        return;
    } else {
        return;
    }
    Refresh();
}

static void FolderMenu(int f) {
    if (f < 0 || f >= (int)g.allFolders.size()) return;
    std::wstring name = g.allFolders[f].name;
    auto custom = CustomFolders();
    bool isCustom = std::find(custom.begin(), custom.end(), name) != custom.end();
    bool hasOrder = !IniGetLong(L"Reihenfolge", name).empty();
    HMENU m = CreatePopupMenu();
    if (g.view != View::Settings) AppendMenuW(m, MF_STRING, 3, L"Ordner öffnen");
    AppendMenuW(m, MF_STRING | (hasOrder ? 0 : MF_GRAYED), 1, L"Reihenfolge zurücksetzen (nach Nutzung)");
    AppendMenuW(m, MF_STRING, 2, L"Ordner löschen …");
    int cmd = TrackMenu(m);
    if (cmd == 3) {
        for (int i = 0; i < (int)g.folders.size(); i++)
            if (g.folders[i].name == name) ShowView(View::Folder, i);
        return;
    } else if (cmd == 1) {
        IniSet(L"Reihenfolge", name, nullptr);
    } else if (cmd == 2) {
        if (!ConfirmDeleteFolder(name)) return;
        DeleteFolder(name);
        if (g.view == View::Folder) { ShowView(View::Folders); return; }
    } else {
        return;
    }
    BuildFolders();
    Refresh();
}

static void BluetoothMenu() {
    RefreshBluetooth();
    HMENU m = CreatePopupMenu();
    if (!g.btRadio) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Bluetooth ist ausgeschaltet");
    else if (g.btDevices.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Keine gekoppelten Geräte");
    for (int i = 0; i < (int)g.btDevices.size() && i < 20; i++) {
        std::wstring s = g.btDevices[i].name + (g.btDevices[i].connected ? L"\tverbunden" : L"");
        AppendMenuW(m, MF_STRING | (g.btDevices[i].connected ? MF_CHECKED : 0), 100 + i, s.c_str());
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 1, L"Verbinden … (Schnellmenü)");
    AppendMenuW(m, MF_STRING, 2, L"Bluetooth-Einstellungen");
    AppendMenuW(m, MF_STRING, 3, L"Neues Gerät koppeln …");
    int cmd = TrackMenu(m);
    if (cmd == 1 || cmd >= 100) OpenBluetoothPanel();
    else if (cmd == 2) ShellExecuteW(nullptr, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL);
    else if (cmd == 3) ShellExecuteW(nullptr, L"open", L"ms-settings-connectabledevices:devicediscovery", nullptr, nullptr, SW_SHOWNORMAL);
    Render();
}

static void OpenSettings(int tab) {
    g.setTab = tab;
    g.query.clear();
    g.results.clear();
    g.creatingFolder = false;
    g.namingFor = -1;
    ShowView(View::Settings);
}

static void MainMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Apps neu laden");
    AppendMenuW(m, MF_STRING, 2, L"Design …");
    AppendMenuW(m, MF_STRING, 3, L"Ordner bearbeiten …");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 5, L"HoverBar beenden");
    int cmd = TrackMenu(m);
    switch (cmd) {
    case 1:
        FreeData();
        StartLoad();
        Refresh();
        break;
    case 2:
        OpenSettings(0);
        break;
    case 3:
        OpenSettings(1);
        break;
    case 5:
        DestroyWindow(g.hwnd);
        break;
    }
}

// ---------------------------------------------------------------- Einstellungen: Design

static void ApplySlider(int x) {
    for (auto& h : g.hits) {
        if ((h.type != HT::SetSlider && h.type != HT::BarVol) || h.idx != g.sliderDrag) continue;
        if (h.idx == 10) {  // Schneiden: nächsten Griff ziehen
            int kr = S(10), x0 = h.rc.left + kr, x1 = h.rc.right - kr;
            double t = x1 > x0 ? std::max(0.0, std::min(1.0, (x - x0) / (double)(x1 - x0))) * g.trimLen : 0;
            if (!g.trimHandle) g.trimHandle = std::fabs(t - g.trimA) <= std::fabs(t - g.trimB) ? 1 : 2;
            if (g.trimHandle == 1) g.trimA = std::max(0.0, std::min(t, g.trimB - 0.1));
            else g.trimB = std::min(g.trimLen, std::max(t, g.trimA + 0.1));
            g.trimSel = g.trimHandle;
            g.editField = 0;  // Pfeiltasten gehen jetzt an die Linie, nicht ins Textfeld
            g.lastSlider = 10;
            return;
        }
        int kr = S(9), x0 = h.rc.left + kr, x1 = h.rc.right - kr;
        int v = x1 > x0 ? (x - x0) * 100 / (x1 - x0) : 0;
        v = std::max(0, std::min(100, v));
        if (h.idx == 0) g.bgAlpha = 40 + v * 215 / 100;
        else if (h.idx == 1) g.imgStrength = v;
        else if (h.idx == 3) { g.volume = v; Player_SetVolume(VolumeGain(v)); }
        else if (v != g.blurAmount) { g.blurAmount = v; BlurBackdrop(); }
        g.lastSlider = h.idx;
        return;
    }
}

static void SaveSliders() {
    if (g.lastSlider == 10) { g.trimHandle = 0; return; }  // Schneiden: nichts zu speichern
    IniSet(L"Einstellungen", L"Deckkraft", std::to_wstring(g.bgAlpha).c_str());
    IniSet(L"Einstellungen", L"Bildstaerke", std::to_wstring(g.imgStrength).c_str());
    IniSet(L"Einstellungen", L"Unschaerfegrad", std::to_wstring(g.blurAmount).c_str());
    IniSet(L"Einstellungen", L"Lautstaerke", std::to_wstring(g.volume).c_str());
    if (g.lastSlider == 3) return;  // Lautstärke: kein Neuaufnehmen des Glases
    if (g.blur && !g.imgPath.empty()) CaptureBackdrop(g.capX, g.capY, g.wWide, g.h);  // Bild neu einmischen
}

static void SaveTint() {
    wchar_t buf[8];
    wsprintfW(buf, L"%02X%02X%02X", GetRValue(g.tint), GetGValue(g.tint), GetBValue(g.tint));
    IniSet(L"Einstellungen", L"Farbe", buf);
}

// Glas neu aufnehmen, obwohl die Leiste sichtbar ist: kurz ausblenden, aufnehmen, wieder zeigen
static void Recapture() {
    if (!g.blur) return;
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 0, AC_SRC_ALPHA};
    UpdateLayeredWindow(g.hwnd, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &bf, ULW_ALPHA);
    DwmFlush();
    DwmFlush();
    CaptureBackdrop(g.workRight - g.margin - g.wWide, g.y, g.wWide, g.h);
}

// Eigene Farbe wählen; zu helle Farben werden abgedunkelt, damit die Schrift lesbar bleibt
static void PickColor() {
    static COLORREF custom[16] = {};
    CHOOSECOLORW cc = {sizeof(cc)};
    cc.hwndOwner = g.hwnd;
    int r = GetRValue(g.tint), gg = GetGValue(g.tint), b = GetBValue(g.tint);
    int m = std::max(1, std::max(r, std::max(gg, b)));
    cc.rgbResult = RGB(r * 200 / m, gg * 200 / m, b * 200 / m);
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    g.menuOpen = true;
    BOOL ok = ChooseColorW(&cc);
    g.menuOpen = false;
    SetForegroundWindow(g.hwnd);
    if (!ok) return;
    r = GetRValue(cc.rgbResult); gg = GetGValue(cc.rgbResult); b = GetBValue(cc.rgbResult);
    m = std::max(r, std::max(gg, b));
    if (m > 64) { r = r * 64 / m; gg = gg * 64 / m; b = b * 64 / m; }
    g.tint = RGB(r, gg, b);
    SaveTint();
}

static void PickImage() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC spec[] = {{L"Bilder (JPEG, PNG, BMP)", L"*.jpg;*.jpeg;*.png;*.bmp"}};
    dlg->SetFileTypes(1, spec);
    dlg->SetTitle(L"Hintergrundbild für HoverBar wählen");
    g.menuOpen = true;
    HRESULT hr = dlg->Show(g.hwnd);
    g.menuOpen = false;
    SetForegroundWindow(g.hwnd);
    if (SUCCEEDED(hr)) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                g.imgPath = p;
                g.imgLoaded.clear();
                IniSet(L"Einstellungen", L"Bild", p);
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    dlg->Release();
}

static bool FontInstalled(const wchar_t* name) {
    HDC dc = GetDC(nullptr);
    LOGFONTW lf = {};
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, name, LF_FACESIZE);
    bool found = false;
    EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int {
        *(bool*)p = true;
        return 0;
    }, (LPARAM)&found, 0);
    ReleaseDC(nullptr, dc);
    return found;
}

static void SetFont(const std::wstring& name) {
    g.fontName = name;
    IniSet(L"Einstellungen", L"Schrift", name.c_str());
    SetDpi(g.dpi, true);
}

// Schriftauswahl: gängige Schriften + "Andere Schrift …" (alle installierten)
static void FontMenu() {
    static const wchar_t* candidates[] = {L"Segoe UI", L"Segoe UI Variable Display", L"Bahnschrift", L"Arial", L"Calibri",
                                          L"Candara", L"Corbel", L"Verdana", L"Tahoma", L"Trebuchet MS", L"Georgia",
                                          L"Cambria", L"Century Gothic", L"Franklin Gothic Medium", L"Consolas",
                                          L"Comic Sans MS", L"Inter", L"Roboto", L"Montserrat", L"Poppins", L"SF Pro Display"};
    std::vector<std::wstring> list;
    for (auto c : candidates)
        if (FontInstalled(c)) list.push_back(c);
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < (int)list.size(); i++)
        AppendMenuW(m, MF_STRING | (list[i] == g.fontName ? MF_CHECKED : 0), 100 + i, list[i].c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 1, L"Andere Schrift …");
    int cmd = TrackMenu(m);
    if (cmd >= 100 && cmd < 100 + (int)list.size()) {
        SetFont(list[cmd - 100]);
    } else if (cmd == 1) {
        LOGFONTW lf = {};
        lstrcpynW(lf.lfFaceName, g.fontName.c_str(), LF_FACESIZE);
        lf.lfHeight = -S(15);
        CHOOSEFONTW cf = {sizeof(cf)};
        cf.hwndOwner = g.hwnd;
        cf.lpLogFont = &lf;
        cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOSCRIPTSEL | CF_NOSIZESEL | CF_NOSTYLESEL;
        g.menuOpen = true;
        BOOL ok = ChooseFontW(&cf);
        g.menuOpen = false;
        SetForegroundWindow(g.hwnd);
        if (ok && lf.lfFaceName[0]) SetFont(lf.lfFaceName);
    } else {
        return;
    }
    Refresh();
}

// ---------------------------------------------------------------- Musik: Aktionen

static void MusicChanged() {
    Music_Save();
    BuildMusic();
    Refresh();
}

// Song an Position in einer Playlist legen; kommt er aus einer Playlist (fromPl/fromPos), wird er dort entfernt
static void PlaylistPlace(int songIdx, int fromPl, int fromPos, int toPl, int toPos) {
    auto& pls = Music_Playlists();
    if (toPl < 0 || toPl >= (int)pls.size()) return;
    const std::wstring id = Music_Songs()[songIdx].id;
    if (fromPl >= 0 && fromPl < (int)pls.size()) {
        // Position in der Anzeige -> Position in der gespeicherten Liste (gleich, solange alle Dateien existieren)
        auto& src = pls[fromPl].ids;
        auto it = std::find(src.begin(), src.end(), id);
        if (it != src.end()) {
            int old = (int)(it - src.begin());
            src.erase(it);
            if (fromPl == toPl && old < toPos) toPos--;
        }
    } else {
        // neu hinzufügen: steht er schon drin, wird er nur verschoben
        auto& dst = pls[toPl].ids;
        auto it = std::find(dst.begin(), dst.end(), id);
        if (it != dst.end()) {
            int old = (int)(it - dst.begin());
            dst.erase(it);
            if (old < toPos) toPos--;
        }
    }
    auto& dst = pls[toPl].ids;
    toPos = std::max(0, std::min(toPos, (int)dst.size()));
    dst.insert(dst.begin() + toPos, id);
    MusicChanged();
}

static void PlaylistRemove(int pl, int songIdx) {
    auto& pls = Music_Playlists();
    if (pl < 0 || pl >= (int)pls.size()) return;
    auto& v = pls[pl].ids;
    v.erase(std::remove(v.begin(), v.end(), Music_Songs()[songIdx].id), v.end());
    MusicChanged();
}

static int CreatePlaylist(std::wstring name) {
    name.erase(std::remove(name.begin(), name.end(), L'\t'), name.end());
    while (!name.empty() && name.back() == L' ') name.pop_back();
    while (!name.empty() && name.front() == L' ') name.erase(0, 1);
    if (name.empty()) return -1;
    auto& pls = Music_Playlists();
    for (int i = 0; i < (int)pls.size(); i++)
        if (pls[i].name == name) return i;
    pls.push_back({name, {}});
    MusicChanged();
    return (int)pls.size() - 1;
}

static void PlayList(int pl, int start) {
    const auto& items = MusicListItems(pl);
    if (items.empty()) return;
    Player_Play(IdsOf(items), start);
    StartDuckWatch(g.hwnd);
    g.pst = Player_State();
    Render();
}

// Musikquelle wechseln: HoverBar-Player (0) oder Spotify (1)
static void SetMusicSource(int src) {
    g.musicSrc = src;
    IniSet(L"Funktionen", L"MusikQuelle", src ? L"1" : L"0");
    Media_SetSpotifyOnly(src == 1);
    if (src == 1) {
        if (Player_State().active) Player_Stop();  // eigener Player aus, Spotify übernimmt
        g.pst = PlayerState();
        if (g.open) Media_Open(g.hwnd, S(kBarArt));
    } else {
        Media_Close();
        g.media = MediaInfo();
        if (g.mediaArt) { DeleteObject(g.mediaArt); g.mediaArt = nullptr; }
    }
}

static void OpenSongEdit(int songIdx) {
    const Song* s = SongAt(songIdx);
    if (!s) return;
    if (g.view != View::SongEdit) { g.editReturn = g.view; g.editReturnTab = g.setTab; }
    g.editId = s->id;
    g.editTitle = s->title;
    g.editArtist = s->artist;
    g.editField = 1;
    g.coverResults.clear();
    g.coverSearching = false;
    g.coverNone = false;
    g.query.clear();
    g.trimLen = Music_Length(s->id);
    Log((L"Song bearbeiten: " + s->title + L", Länge " + std::to_wstring((int)g.trimLen) + L" s").c_str());
    g.trimA = 0;
    g.trimB = g.trimLen;
    g.trimHandle = 0;
    g.trimSel = 0;
    g.wave.clear();
    Music_LoadWave(s->id, 400);
    ShowView(View::SongEdit);
}

static void LeaveSongEdit() {
    g.editField = 0;
    g.coverResults.clear();
    g.coverSearching = false;
    g.coverNone = false;
    if (g.editReturn == View::Settings) { g.setTab = g.editReturnTab; ShowView(View::Settings); }
    else ShowView(g.editReturn == View::SongEdit ? View::Music : g.editReturn, g.editReturn == View::Folder ? g.curFolder : -1);
}

// Song-Ordner umstellen (neue Downloads landen dort) und auf Wunsch die vorhandenen Songs verschieben
static int SetSongFolder(const std::wstring& path, bool moveExisting) {
    SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);  // auch fehlende Zwischenordner anlegen
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log(L"Song-Ordner: kann nicht angelegt werden: " + path);
        return -1;
    }
    Music_SetFolder(path);
    IniSet(L"Funktionen", L"SongOrdner", path.c_str());
    Log(L"Song-Ordner: " + path);
    int moved = 0;
    if (moveExisting) {
        Music_Init(g.hwnd);  // Bibliothek laden, falls die Leiste noch nicht offen war
        if (Player_State().active) Player_Stop();  // Dateien freigeben
        g.pst = PlayerState();
        moved = Music_MoveSongsToFolder();
        Log(L"Songs verschoben: " + std::to_wstring(moved) + L" von " + std::to_wstring(Music_Songs().size()));
    }
    return moved;
}

// ---------------------------------------------------------------- Musik-Sync: Cloud-Dienste erkennen, auswählen, einrichten
// Bestätigung für Aktionen mit Folgen (Programm installieren, Songs verschieben/hochladen): Eine Antwort in unter 0,7 s kann niemand gelesen
// haben (z. B. ein durchgeklickter Mausklick) und zählt als "Nein".
static bool ConfirmCareful(const std::wstring& msg, const wchar_t* actionText) {
    // eigener Dialog statt Ja/Nein: nur der Knopf mit dem Namen der Aktion stimmt zu, "Abbrechen" ist vorbelegt (Tastenkürzel wie J/N tun nichts)
    TASKDIALOG_BUTTON btns[] = {{100, actionText}, {IDCANCEL, L"Abbrechen"}};
    TASKDIALOGCONFIG c = {sizeof(c)};
    c.hwndParent = g.open ? g.hwnd : nullptr;
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    c.pszWindowTitle = L"HoverBar";
    c.pszMainInstruction = L"Bitte bestätigen";
    c.pszContent = msg.c_str();
    c.pszMainIcon = TD_INFORMATION_ICON;
    c.cButtons = 2;
    c.pButtons = btns;
    c.nDefaultButton = IDCANCEL;
    int btn = 0;
    ULONGLONG t0 = GetTickCount64();
    g.menuOpen = true;
    HRESULT hr = TaskDialogIndirect(&c, &btn, nullptr, nullptr);
    g.menuOpen = false;
    if (g.open) SetForegroundWindow(g.hwnd);
    if (FAILED(hr) || btn != 100) return false;
    if (GetTickCount64() - t0 < 700) {  // schneller als lesbar: durchgeklickt
        Log(L"Bestätigung zu schnell beantwortet – als Nein gewertet");
        return false;
    }
    return true;
}
struct CloudProv {
    std::wstring name, quota, url, base;  // Anzeigename, gratis-Speicher, Download-Seite, Basisordner (leer = nicht installiert)
    bool reportsFree = false;             // zeigt das Laufwerk den freien Cloud-Speicher? (Google Drive, pCloud)
    std::wstring wingetId;                // Paket im Windows-Paketmanager (leer = nur Download-Seite öffnen)
    std::wstring androidPkg;              // Paketname der Android-App (für die Handy-Einrichtung)
    std::wstring parentHint;              // Ordner zwischen Cloud-Wurzel und HoverBar-Musik (z. B. "Meine Ablage")
    std::wstring directUrl;               // Installer direkt vom Hersteller (HTTPS), wenn winget fehlt oder es kein winget-Paket gibt
    std::wstring signer;                  // so muss der Unterzeichner des Installers (Teil des Namens, klein) heißen
    std::wstring silentArgs;              // Schalter für stille Installation (leer = normaler Ablauf)
};

// ---------------------------------------------------------------- Handy verbinden: QR-Code für die Einrichtung von HoverMusic
static std::string Utf8Of(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::string JsonText(const std::wstring& w) {  // für Strings in der Einrichtungs-JSON
    std::string s = Utf8Of(w), o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        if ((unsigned char)c >= 0x20) o += c;
    }
    return o;
}
static std::string B64Url(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string o;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8 | (unsigned char)in[i + 2];
        o += t[v >> 18 & 63]; o += t[v >> 12 & 63]; o += t[v >> 6 & 63]; o += t[v & 63];
    }
    if (i + 1 == in.size()) {
        unsigned v = (unsigned char)in[i] << 16;
        o += t[v >> 18 & 63]; o += t[v >> 12 & 63];
    } else if (i + 2 == in.size()) {
        unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8;
        o += t[v >> 18 & 63]; o += t[v >> 12 & 63]; o += t[v >> 6 & 63];
    }
    return o;
}
// Inhalt des QR-Codes: ein Link auf die HoverMusic-Downloadseite (so führt auch die normale Handy-Kamera zur App),
// im Teil hinter "#hm=" steht die Einrichtung (Dienst, Android-Paket, Ordner) für die Scan-Funktion in HoverMusic. Keine Zugangsdaten, keine Pfade vom PC.
static std::string PhoneSetupUrl() {
    std::wstring folder = g.syncDir.empty() ? L"HoverBar-Musik" : g.syncDir.substr(g.syncDir.find_last_of(L'\\') + 1);
    std::string json = "{\"v\":1,\"cloud\":\"" + JsonText(g.syncCloud) + "\",\"folder\":\"" + JsonText(folder) + "\",\"pkg\":\"" + JsonText(g.syncPkg) +
                       "\",\"parent\":\"" + JsonText(g.syncParent) + "\"}";
    return "https://github.com/KarosuMinoyari/HoverBar/releases#hm=" + B64Url(json);
}

static HWND sQrWnd = nullptr;
static QrCode sQr;
static LRESULT CALLBACK QrProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(24, 24, 30));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, g.fTitle);
        SetTextColor(dc, RGB(245, 245, 248));
        RECT t = {S(24), S(16), rc.right - S(24), S(54)};
        DrawTextW(dc, L"Handy verbinden", -1, &t, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        int quiet = 4, mods = sQr.size + 2 * quiet, px = std::max(3, S(300) / mods), total = px * mods;
        int x0 = (rc.right - total) / 2, y0 = S(66);
        HBRUSH white = CreateSolidBrush(RGB(255, 255, 255)), black = CreateSolidBrush(RGB(0, 0, 0));
        RECT wr = {x0, y0, x0 + total, y0 + total};
        FillRect(dc, &wr, white);
        for (int y = 0; y < sQr.size; y++)
            for (int x = 0; x < sQr.size; x++)
                if (sQr.dark(x, y)) {
                    RECT r = {x0 + (x + quiet) * px, y0 + (y + quiet) * px, x0 + (x + quiet + 1) * px, y0 + (y + quiet + 1) * px};
                    FillRect(dc, &r, black);
                }
        DeleteObject(white);
        DeleteObject(black);
        std::wstring cloud = g.syncCloud.empty() || g.syncCloud == L"Cloud" ? L"deinen Cloud-Dienst" : g.syncCloud;
        std::wstring txt = L"1.  HoverMusic aufs Handy: den Code mit der Kamera scannen (öffnet die Download-Seite) und die App installieren.\n\n"
                           L"2.  In HoverMusic \u201EQR-Code scannen\u201C antippen und diesen Code scannen.\n\n"
                           L"3.  Die App führt dich weiter: " + cloud + L" auf dem Handy installieren und anmelden, dann den Ordner \u201EHoverBar-Musik\u201C wählen.";
        SelectObject(dc, g.fText);
        SetTextColor(dc, RGB(220, 220, 228));
        RECT a = {S(28), y0 + total + S(18), rc.right - S(28), rc.bottom - S(12)};
        DrawTextW(dc, txt.c_str(), -1, &a, DT_LEFT | DT_WORDBREAK);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        sQrWnd = nullptr;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static void ShowPhoneQr() {
    std::string url = PhoneSetupUrl();
    if (!Qr_Encode(url, true, sQr) && !Qr_Encode(url, false, sQr)) return;
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = QrProc;
        wc.hInstance = g.inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"HoverBarQr";
        wc.hIcon = LoadIconW(g.inst, MAKEINTRESOURCEW(1));
        RegisterClassW(&wc);
        reg = true;
    }
    if (sQrWnd) {
        InvalidateRect(sQrWnd, nullptr, TRUE);
        ShowWindow(sQrWnd, SW_SHOW);
        SetForegroundWindow(sQrWnd);
        return;
    }
    RECT r = {0, 0, S(440), S(700)};
    AdjustWindowRectEx(&r, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOPMOST);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = std::max(0, (GetSystemMetrics(SM_CYSCREEN) - hgt) / 2);
    sQrWnd = CreateWindowExW(WS_EX_TOPMOST, L"HoverBarQr", L"HoverBar – Handy verbinden", WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, w, hgt, nullptr, nullptr,
                             g.inst, nullptr);
    if (sQrWnd) SetForegroundWindow(sQrWnd);
}

// ---- Nach der Installation: Programm starten, auf die Anmeldung warten (der Ordner erscheint), Einrichtung automatisch fortsetzen
static void CloudSet(int st, const std::wstring& msg);
static std::wstring sCloudName;       // Dienst, der gerade installiert wurde
static std::wstring sCloudWait;       // Dienst, auf dessen Anmeldung gewartet wird (leer = nichts)
static ULONGLONG sCloudWaitSince = 0;
// Zeigt die Verknüpfung auf eine Datei, die es noch gibt? (Verknüpfungen auf Store-Apps ohne Dateipfad gelten als vorhanden)
static bool LnkTargetExists(const std::wstring& lnk) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return true;
    bool ok = true;
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
        if (SUCCEEDED(pf->Load(lnk.c_str(), STGM_READ))) {
            wchar_t raw[MAX_PATH * 2] = {}, path[MAX_PATH * 2] = {};
            if (SUCCEEDED(sl->GetPath(raw, MAX_PATH * 2, nullptr, SLGP_RAWPATH)) && raw[0]) {
                ExpandEnvironmentStringsW(raw, path, MAX_PATH * 2);
                ok = GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
            }
        }
        pf->Release();
    }
    sl->Release();
    return ok;
}
// Verknüpfung des Programms im Startmenü suchen (Name enthält das Stichwort) und starten, damit man sich anmelden kann
static bool LaunchCloudApp(const std::wstring& keyword, bool run = true, std::wstring* found = nullptr) {
    std::vector<std::wstring> roots;
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &p))) roots.push_back(p);
    CoTaskMemFree(p);
    p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_CommonPrograms, 0, nullptr, &p))) roots.push_back(p);
    CoTaskMemFree(p);
    std::wstring key = Lower(keyword);
    for (auto& root : roots) {
        std::vector<std::wstring> dirs = {root};
        for (size_t d = 0; d < dirs.size() && d < 60; d++) {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((dirs[d] + L"\\*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                std::wstring name = fd.cFileName;
                if (name == L"." || name == L"..") continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { dirs.push_back(dirs[d] + L"\\" + name); continue; }
                std::wstring low = Lower(name);
                if (low.size() > 4 && low.compare(low.size() - 4, 4, L".lnk") == 0 && low.find(key) != std::wstring::npos &&
                    low.find(L"uninstall") == std::wstring::npos && low.find(L"deinstall") == std::wstring::npos) {
                    std::wstring lnk = dirs[d] + L"\\" + name;
                    if (!LnkTargetExists(lnk)) continue;  // Programm wurde gelöscht, nur die Verknüpfung blieb übrig
                    FindClose(h);
                    if (found) *found = lnk;
                    if (run) ShellExecuteW(nullptr, L"open", lnk.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    return true;
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    return false;
}
static const wchar_t* CloudLaunchKeyword(const std::wstring& name) {
    if (name == L"MEGA") return L"MEGAsync";
    return name.c_str();
}
static std::vector<CloudProv> ListClouds();
static void SyncSetupFinish(const std::wstring& dir, const std::wstring& cloudName, const std::wstring& pkg, const std::wstring& parent, bool reportsFree, bool custom);
static void CloudWaitStart(const std::wstring& name) {
    sCloudWait = name;
    sCloudWaitSince = GetTickCount64();
    SetTimer(g.hwnd, T_CLOUDWAIT, 4000, nullptr);
}
static void CloudWaitStop() {
    sCloudWait.clear();
    KillTimer(g.hwnd, T_CLOUDWAIT);
}
// alle paar Sekunden: ist der Cloud-Ordner da (= angemeldet)? Dann mit der Einrichtung weitermachen
static void CloudWaitTick() {
    if (sCloudWait.empty()) { KillTimer(g.hwnd, T_CLOUDWAIT); return; }
    if (GetTickCount64() - sCloudWaitSince > 20ull * 60 * 1000) {
        CloudSet(3, sCloudWait + L": keine Anmeldung bemerkt (20 Minuten). Melde dich an und klicke dann hier auf „Einrichten“.");
        CloudWaitStop();
        if (g.open && !g.closing && g.view == View::Settings) Render();
        return;
    }
    for (auto& c : ListClouds())
        if (c.name == sCloudWait && !c.base.empty()) {
            std::wstring name = c.name;
            CloudWaitStop();
            CloudSet(0, L"");
            Log(L"Cloud-Ordner gefunden, setze die Einrichtung fort: " + name);
            SyncSetupFinish(c.base + L"\\HoverBar-Musik", c.name, c.androidPkg, c.parentHint, c.reportsFree, false);
            if (g.open && !g.closing) Refresh();
            return;
        }
}

// ---- Cloud-Programm per winget installieren (wie yt-dlp: ein Klick, Fortschritt auf der Karte)
enum { WM_APP_CLOUD = WM_APP + 100 };
static bool WingetAvailable() {
    wchar_t b[MAX_PATH];
    return SearchPathW(nullptr, L"winget.exe", nullptr, MAX_PATH, b, nullptr) > 0;
}
static void CloudSet(int st, const std::wstring& msg) {
    std::lock_guard<std::mutex> l(sCloudMu);
    sCloudState = st;
    sCloudMsg = msg;
}
// Digitale Signatur gültig und vom erwarteten Hersteller? (Name des Unterzeichners enthält signerLower)
static bool SignedBy(const std::wstring& file, const std::wstring& signerLower, std::wstring& foundName) {
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
            foundName = name;
            ok = Lower(foundName).find(signerLower) != std::wstring::npos;
        }
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &wd);
    return ok;
}
struct CloudDlCb : IBindStatusCallback {
    HWND h;
    std::wstring name;
    int last = -1;
    CloudDlCb(HWND hh, const std::wstring& n) : h(hh), name(n) {}
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
            int pct = (int)((unsigned long long)now * 100 / total);
            if (pct != last) {
                last = pct;
                CloudSet(1, name + L" wird geladen … " + std::to_wstring(pct) + L" %");
                PostMessageW(h, WM_APP_CLOUD, 0, 0);
            }
        }
        return S_OK;
    }
    STDMETHODIMP OnStopBinding(HRESULT, LPCWSTR) override { return S_OK; }
    STDMETHODIMP GetBindInfo(DWORD*, BINDINFO*) override { return S_OK; }
    STDMETHODIMP OnDataAvailable(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override { return S_OK; }
    STDMETHODIMP OnObjectAvailable(REFIID, IUnknown*) override { return S_OK; }
};
// Installer direkt vom Hersteller: HTTPS-Download, Signatur prüfen (gültig und vom richtigen Hersteller), dann (still) starten
static void StartCloudDirectInstall(const CloudProv& c) {
    Log(L"Cloud-Installation (direkt) gestartet: " + c.name);
    sCloudName = c.name;
    CloudSet(1, c.name + L" wird geladen …");
    struct Job { HWND h; std::wstring name, url, signer, args; };
    auto* j = new Job{g.hwnd, c.name, c.directUrl, c.signer, c.silentArgs};
    HANDLE t = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        auto* j = (Job*)p;
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        std::wstring dir = std::wstring(tmp) + L"HoverBarCloud";
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring file = dir + L"\\" + j->name + L"-Setup.exe";
        DeleteFileW(file.c_str());
        CloudDlCb cb(j->h, j->name);
        do {
            if (FAILED(URLDownloadToFileW(nullptr, j->url.c_str(), file.c_str(), 0, &cb))) {
                CloudSet(3, j->name + L": Download fehlgeschlagen (Internetverbindung?).");
                break;
            }
            CloudSet(1, j->name + L": prüfe die digitale Signatur …");
            PostMessageW(j->h, WM_APP_CLOUD, 0, 0);
            std::wstring found;
            if (!SignedBy(file, j->signer, found)) {
                DeleteFileW(file.c_str());
                CloudSet(3, j->name + L": Die Signatur des Installers ist ungültig oder nicht vom Hersteller" + (found.empty() ? L"" : L" (Unterzeichner: " + found + L")") + L" – abgebrochen.");
                break;
            }
            Log(L"Installer geprüft, Unterzeichner: " + found);
            CloudSet(1, j->name + L" wird installiert … (Windows fragt eventuell nach Administratorrechten)");
            PostMessageW(j->h, WM_APP_CLOUD, 0, 0);
            SHELLEXECUTEINFOW sei = {sizeof(sei)};
            sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
            sei.lpVerb = L"open";
            sei.lpFile = file.c_str();
            sei.lpParameters = j->args.empty() ? nullptr : j->args.c_str();
            sei.nShow = j->args.empty() ? SW_SHOWNORMAL : SW_HIDE;
            if (!ShellExecuteExW(&sei) || !sei.hProcess) {
                DeleteFileW(file.c_str());
                CloudSet(3, j->name + L": Der Installer konnte nicht gestartet werden (Windows-Rückfrage abgelehnt?).");
                break;
            }
            DWORD wait = WaitForSingleObject(sei.hProcess, 25 * 60 * 1000), code = 1;
            GetExitCodeProcess(sei.hProcess, &code);
            CloseHandle(sei.hProcess);
            DeleteFileW(file.c_str());
            wchar_t hex[16];
            swprintf(hex, 16, L"0x%08X", code);
            if (wait == WAIT_TIMEOUT) CloudSet(3, L"Die Installation dauert zu lange – abgebrochen.");
            else if (code == 0) CloudSet(2, j->name + L" ist installiert.");
            else CloudSet(3, j->name + L": Installation fehlgeschlagen (Code " + hex + L").");
        } while (false);
        PostMessageW(j->h, WM_APP_CLOUD, 0, 0);
        delete j;
        return 0;
    }, j, 0, nullptr);
    if (t) CloseHandle(t);
    else { delete j; CloudSet(3, L"Konnte nicht starten."); }
}
static void StartCloudInstall(const CloudProv& c) {
    {
        std::lock_guard<std::mutex> l(sCloudMu);
        if (sCloudState == 1) return;
    }
    if (c.wingetId.empty() || !WingetAvailable()) {  // kein winget(-Paket): Installer direkt vom Hersteller laden
        if (!c.directUrl.empty()) StartCloudDirectInstall(c);
        else CloudSet(3, c.name + L": kein Installationsweg gefunden.");
        return;
    }
    Log(L"Cloud-Installation gestartet: " + c.wingetId);
    sCloudName = c.name;
    CloudSet(1, c.name + L" wird installiert … (Windows fragt eventuell nach Administratorrechten)");
    struct Job { HWND h; std::wstring name, id; };
    auto* j = new Job{g.hwnd, c.name, c.wingetId};
    HANDLE t = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        auto* j = (Job*)p;
        std::wstring cmd = L"winget.exe install --id " + j->id + L" -e --source winget --silent --accept-package-agreements --accept-source-agreements";
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(0);
        STARTUPINFOW si = {sizeof(si)};
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            CloudSet(3, L"winget konnte nicht gestartet werden.");
        } else {
            DWORD wait = WaitForSingleObject(pi.hProcess, 25 * 60 * 1000), code = 1;
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            wchar_t hex[16];
            swprintf(hex, 16, L"0x%08X", code);
            if (wait == WAIT_TIMEOUT) CloudSet(3, L"Die Installation dauert zu lange – abgebrochen.");
            else if (code == 0) CloudSet(2, j->name + L" ist installiert. Melde dich dort an, danach hier nochmal auf „Einrichten“ klicken.");
            else CloudSet(3, j->name + L": Installation fehlgeschlagen (winget " + hex + L"). Eventuell wurde die Windows-Rückfrage abgelehnt.");
        }
        PostMessageW(j->h, WM_APP_CLOUD, 0, 0);
        delete j;
        return 0;
    }, j, 0, nullptr);
    if (t) CloseHandle(t);
    else { delete j; CloudSet(3, L"Konnte nicht starten."); }
}
static std::wstring DriveWithLabel(const wchar_t* label) {
    for (wchar_t c = L'D'; c <= L'Z'; c++) {
        wchar_t root[4] = {c, L':', L'\\', 0}, lab[MAX_PATH] = {};
        if (GetDriveTypeW(root) != DRIVE_NO_ROOT_DIR && GetVolumeInformationW(root, lab, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0) && _wcsicmp(lab, label) == 0)
            return root;
    }
    return L"";
}
static std::wstring EnvPath(const wchar_t* name) {
    wchar_t b[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, b, MAX_PATH * 2);
    return n && n < MAX_PATH * 2 ? std::wstring(b) : L"";
}
static bool DirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return !p.empty() && a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static std::vector<CloudProv> ListClouds() {
    std::vector<CloudProv> v;
    std::wstring home = EnvPath(L"USERPROFILE");
    {   // Google Drive für Desktop: Laufwerk "Google Drive", darin "Meine Ablage" / "My Drive"
        CloudProv c{L"Google Drive", L"15 GB gratis", L"https://www.google.com/drive/download/", L"", true, L"Google.GoogleDrive", L"com.google.android.apps.docs", L""};
        std::wstring root = DriveWithLabel(L"Google Drive");
        if (!root.empty()) {
            std::wstring first;
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((root + L"*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' || fd.cFileName[0] == L'$') continue;
                    std::wstring n = fd.cFileName;
                    if (n == L"My Drive" || n == L"Meine Ablage" || n == L"Mein Laufwerk" || n == L"Mon Drive" || n == L"Mi unidad") { first = n; break; }
                    if (first.empty()) first = n;
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            if (!first.empty()) { c.base = root + first; c.parentHint = first; }
        }
        v.push_back(c);
    }
    {
        CloudProv c{L"OneDrive", L"5 GB gratis, bei Windows dabei", L"https://www.microsoft.com/microsoft-365/onedrive/download", EnvPath(L"OneDrive"), false, L"Microsoft.OneDrive", L"com.microsoft.skydrive", L""};
        if (!DirExists(c.base)) c.base.clear();
        v.push_back(c);
    }
    {
        CloudProv c{L"Koofr", L"10 GB gratis, kleiner Anbieter", L"https://koofr.eu/desktop-apps/", L"", false, L"", L"net.koofr.android.app", L""};
        for (const wchar_t* d : {L"\\Koofr", L"\\Koofr Sync", L"\\Koofr Desktop"})
            if (DirExists(home + d)) { c.base = home + d; break; }
        v.push_back(c);
    }
    {
        CloudProv c{L"MEGA", L"20 GB gratis", L"https://mega.io/desktop", L"", false, L"Mega.MEGASync", L"mega.privacy.android.app", L""};
        PWSTR docs = nullptr;  // das neuere MEGA legt den Ordner unter "Dokumente" an
        std::wstring docDir;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) docDir = docs;
        CoTaskMemFree(docs);
        // ein echter MEGA-Sync-Ordner enthält die Datei .megaignore: in Benutzerordner, Dokumenten und den Laufwerkswurzeln suchen
        std::vector<std::wstring> roots = {home, docDir};
        for (wchar_t dl = L'C'; dl <= L'Z'; dl++) {
            wchar_t rt[4] = {dl, L':', L'\\', 0};
            if (GetDriveTypeW(rt) == DRIVE_FIXED) roots.push_back(std::wstring(rt));
        }
        for (auto& r : roots) {
            if (r.empty() || !c.base.empty()) continue;
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((r + (r.back() == L'\\' ? L"" : L"\\") + L"*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' || fd.cFileName[0] == L'$') continue;
                std::wstring d = r + (r.back() == L'\\' ? L"" : L"\\") + fd.cFileName;
                if (GetFileAttributesW((d + L"\\.megaignore").c_str()) != INVALID_FILE_ATTRIBUTES) { c.base = d; break; }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (c.base.empty())  // Notlösung: der Standardordner, falls noch keine Markierungsdatei da ist
            for (const std::wstring& d : {home + L"\\MEGA", docDir + L"\\MEGA", home + L"\\MEGAsync"})
                if (d.size() > 5 && DirExists(d)) { c.base = d; break; }
        v.push_back(c);
    }
    {
        CloudProv c{L"pCloud", L"10 GB gratis", L"https://www.pcloud.com/download-free-online-cloud-file-storage.html", L"", true, L"pCloudAG.pCloudDrive", L"com.pcloud.pcloud", L""};
        std::wstring root = DriveWithLabel(L"pCloud Drive");
        if (root.empty()) root = DriveWithLabel(L"pCloud");
        if (!root.empty() && DirExists(root + L"Sync")) c.base = root + L"Sync";
        else if (!root.empty()) c.base = root.substr(0, root.size() - 1);
        v.push_back(c);
    }
    {
        CloudProv c{L"Dropbox", L"2 GB gratis", L"https://www.dropbox.com/install", home + L"\\Dropbox", false, L"Dropbox.Dropbox", L"com.dropbox.android", L""};
        if (!DirExists(c.base)) c.base.clear();
        v.push_back(c);
    }
    for (auto& c : v) {  // Installer direkt vom Hersteller (Signatur wird vor dem Start geprüft)
        if (c.name == L"Google Drive") { c.directUrl = L"https://dl.google.com/drive-file-stream/GoogleDriveSetup.exe"; c.signer = L"google"; c.silentArgs = L"--silent"; }
        else if (c.name == L"OneDrive") { c.directUrl = L"https://go.microsoft.com/fwlink/?linkid=844652"; c.signer = L"microsoft"; c.silentArgs = L"/silent"; }
        else if (c.name == L"Koofr") { c.directUrl = L"https://app.koofr.net/dl/apps/win"; c.signer = L"koofr"; c.silentArgs = L"/S"; }
        else if (c.name == L"MEGA") { c.directUrl = L"https://mega.nz/MEGAsyncSetup64.exe"; c.signer = L"mega"; c.silentArgs = L"/S"; }
        else if (c.name == L"Dropbox") { c.directUrl = L"https://www.dropbox.com/download?full=1&plat=win"; c.signer = L"dropbox"; }
    }
    return v;
}
static std::wstring FmtGB(unsigned long long bytes) {
    wchar_t b[32];
    swprintf(b, 32, L"%.1f GB", bytes / 1073741824.0);
    for (auto& ch : b) if (ch == L'.') ch = L',';
    return b;
}

static void SyncSetupFinish(const std::wstring& dir, const std::wstring& cloudName, const std::wstring& pkg, const std::wstring& parent, bool reportsFree, bool custom);
static void SyncSetupFlow() {
    std::vector<CloudProv> clouds = ListClouds();
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < (int)clouds.size(); i++) {
        auto& c = clouds[i];
        std::wstring label;
        if (!c.base.empty()) {
            ULARGE_INTEGER fr = {}, tot = {}, fa = {};
            std::wstring free;
            if (c.reportsFree && GetDiskFreeSpaceExW(c.base.c_str(), &fr, &tot, &fa)) free = L", " + FmtGB(fr.QuadPart) + L" frei";
            label = L"✓  " + c.name + L"  (" + c.quota + free + L")";
        } else if (LaunchCloudApp(CloudLaunchKeyword(c.name), false)) {  // installiert, aber noch kein Cloud-Ordner: Anmeldung fehlt
            label = c.name + L" ist installiert – öffnen und anmelden …";
        } else {
            label = c.name + ((!c.wingetId.empty() && WingetAvailable()) || !c.directUrl.empty() ? L" installieren …  (" : L" herunterladen (Seite öffnen) …  (") + c.quota + L")";
        }
        AppendMenuW(m, MF_STRING, i + 1, label.c_str());
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 100, L"Eigenen Ordner wählen …");
    int cmd = TrackMenu(m);
    if (cmd <= 0) return;

    std::wstring dir, cloudName, pkg, parent;
    bool reportsFree = false;
    if (cmd == 100) {
        IFileOpenDialog* dlg = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
        DWORD opt = 0;
        dlg->GetOptions(&opt);
        dlg->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(L"Ordner deines Cloud-Programms wählen");
        g.menuOpen = true;
        HRESULT hr = dlg->Show(g.hwnd);
        g.menuOpen = false;
        SetForegroundWindow(g.hwnd);
        if (SUCCEEDED(hr)) {
            IShellItem* it = nullptr;
            if (SUCCEEDED(dlg->GetResult(&it))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { dir = p; CoTaskMemFree(p); }
                it->Release();
            }
        }
        dlg->Release();
        if (dir.empty()) return;
        cloudName = L"deinem Ordner";
        parent = dir.substr(dir.find_last_of(L'\\') == std::wstring::npos ? 0 : dir.find_last_of(L'\\') + 1);
        dir += L"\\HoverBar-Musik";
    } else {
        auto& c = clouds[cmd - 1];
        if (c.base.empty() && LaunchCloudApp(CloudLaunchKeyword(c.name), true)) {  // schon installiert: starten, auf die Anmeldung warten, dann automatisch weiter
            CloudSet(2, c.name + L" wurde gestartet. Melde dich dort an – HoverBar macht danach automatisch mit dem Einrichten weiter.");
            CloudWaitStart(c.name);
            return;
        }
        if (c.base.empty()) {  // nicht installiert: per winget installieren (ein Klick), sonst Download-Seite öffnen
            bool viaWinget = !c.wingetId.empty() && WingetAvailable();
            if (viaWinget || !c.directUrl.empty()) {
                std::wstring how = viaWinget ? L"HoverBar nutzt dafür den Windows-Paketmanager winget (Paket \"" + c.wingetId + L"\", vom Hersteller, mit Prüfsumme)."
                                             : L"HoverBar lädt den Installer direkt vom Hersteller (HTTPS) und startet ihn erst, wenn seine digitale Signatur zu " + c.name + L" passt.";
                if (ConfirmCareful(c.name + L" jetzt installieren?\n\n" + how + L" Windows fragt eventuell nach Administratorrechten.\n\nDanach meldest du dich in " + c.name +
                               L" an – HoverBar macht dann automatisch mit dem Einrichten weiter.", L"Installieren"))
                    StartCloudInstall(c);
            } else {
                ShellExecuteW(nullptr, L"open", c.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                g.syncNote = L"Lade " + c.name + L" und melde dich an, danach hier nochmal auf „Einrichten“ klicken.";
            }
            return;
        }
        dir = c.base + L"\\HoverBar-Musik";
        cloudName = c.name;
        pkg = c.androidPkg;
        parent = c.parentHint;
        reportsFree = c.reportsFree;
    }

    SyncSetupFinish(dir, cloudName, pkg, parent, reportsFree, cmd == 100);
}

// Rückfrage, Platzprüfung, Ordner umstellen, Songs im Hintergrund verschieben (danach QR-Code)
static void SyncSetupFinish(const std::wstring& dir, const std::wstring& cloudName, const std::wstring& pkg, const std::wstring& parent, bool reportsFree, bool custom) {
    // Wie viel muss hochgeladen werden? (Songs, die noch nicht im Ordner liegen)
    Music_Init(g.hwnd);
    unsigned long long need = 0;
    size_t n = 0;
    for (auto& s : Music_Songs()) {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExW(s.file.c_str(), GetFileExInfoStandard, &fa)) continue;
        n++;
        if (_wcsnicmp(s.file.c_str(), dir.c_str(), dir.size()) != 0) need += ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    }
    std::wstring spaceLine;
    if (reportsFree) {
        ULARGE_INTEGER fr = {}, tot = {}, fa = {};
        if (GetDiskFreeSpaceExW(dir.substr(0, dir.find_last_of(L'\\')).c_str(), &fr, &tot, &fa)) {
            if (fr.QuadPart < need + need / 20 + (50ull << 20)) {
                g.menuOpen = true;
                MessageBoxW(g.hwnd, (L"In " + cloudName + L" ist nicht genug Platz frei.\n\nBenötigt: " + FmtGB(need) + L", frei: " + FmtGB(fr.QuadPart) +
                                     L"\n\nWähle einen anderen Dienst (z. B. MEGA mit 20 GB oder Koofr mit 10 GB gratis) oder gib in " + cloudName + L" Platz frei.").c_str(),
                            L"HoverBar", MB_OK | MB_ICONWARNING | MB_TOPMOST);
                g.menuOpen = false;
                return;
            }
            spaceLine = L"\nFrei in der Cloud: " + FmtGB(fr.QuadPart) + L".";
        }
    } else {
        spaceLine = L"\nPrüfe, ob in deinem Cloud-Konto genug frei ist.";
    }
    if (!ConfirmCareful(L"Musik-Sync mit " + cloudName + L" einrichten?\n\n" + std::to_wstring(n) + L" Songs (" + FmtGB(need) + L") werden nach\n" + dir +
                    L"\nverschoben und dort vom Cloud-Programm hochgeladen. Playlists, Favoriten und Cover kommen als HoverBar-Bibliothek.json dazu." + spaceLine +
                    L"\n\nDie Songs liegen danach in deinem Cloud-Konto: nur privat nutzen, nicht teilen.", L"Einrichten"))
        return;
    if (SetSongFolder(dir, false) < 0) {
        g.syncNote = L"Ordner konnte nicht angelegt werden: " + dir;
        return;
    }
    g.syncOn = true;
    g.syncDir = dir;
    g.syncNote.clear();
    g.syncCloud = custom ? L"Cloud" : cloudName;  // für den QR-Code: welcher Dienst, welcher Ordner
    g.syncPkg = pkg;
    g.syncParent = parent;
    g.syncShowQr = true;
    IniSet(L"Erweiterungen", L"MusikSync", L"1");
    IniSet(L"Erweiterungen", L"MusikSyncOrdner", dir.c_str());
    IniSet(L"Erweiterungen", L"MusikSyncCloud", g.syncCloud.c_str());
    IniSet(L"Erweiterungen", L"MusikSyncPaket", g.syncPkg.c_str());
    IniSet(L"Erweiterungen", L"MusikSyncElternordner", g.syncParent.c_str());
    Music_SetFavorites(g.favIds);
    Music_SetSyncExport(true);
    if (Player_State().active) { Player_Stop(); g.pst = PlayerState(); }  // Dateien freigeben
    Music_SyncSetup(g.hwnd, dir);
}

// Ordner für neue Songs wählen; auf Wunsch die vorhandenen Songs mitnehmen
static void PickSongFolder() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opt = 0;
    dlg->GetOptions(&opt);
    dlg->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Ordner für deine Songs wählen");
    IShellItem* cur = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(Music_Folder().c_str(), nullptr, IID_PPV_ARGS(&cur)))) {
        dlg->SetFolder(cur);
        cur->Release();
    }
    g.menuOpen = true;
    HRESULT hr = dlg->Show(g.hwnd);
    g.menuOpen = false;
    SetForegroundWindow(g.hwnd);
    std::wstring path;
    if (SUCCEEDED(hr)) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { path = p; CoTaskMemFree(p); }
            it->Release();
        }
    }
    dlg->Release();
    if (path.empty() || _wcsicmp(path.c_str(), Music_Folder().c_str()) == 0) return;
    Music_Init(g.hwnd);
    size_t n = Music_Songs().size();
    bool move = n && ConfirmBox(L"Neue Songs landen jetzt in\n" + path + L"\n\nSollen die " + std::to_wstring(n) +
                                L" vorhandenen Songs auch dorthin verschoben werden?");
    SetSongFolder(path, move);
}

static void PickLocalCover() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC spec[] = {{L"Bilder (JPEG, PNG, BMP, WebP)", L"*.jpg;*.jpeg;*.png;*.bmp;*.webp"}};
    dlg->SetFileTypes(1, spec);
    dlg->SetTitle(L"Cover für den Song wählen");
    g.menuOpen = true;
    HRESULT hr = dlg->Show(g.hwnd);
    g.menuOpen = false;
    SetForegroundWindow(g.hwnd);
    if (SUCCEEDED(hr)) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                Music_SetCoverFromFile(g.editId, p);
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    dlg->Release();
}

// "+" bei einem Song: zu welcher Playlist?
static void AddToPlaylistMenu(int songIdx) {
    auto& pls = Music_Playlists();
    const std::wstring id = Music_Songs()[songIdx].id;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Zu Playlist hinzufügen:");
    for (int i = 0; i < (int)pls.size(); i++) {
        bool in = std::find(pls[i].ids.begin(), pls[i].ids.end(), id) != pls[i].ids.end();
        AppendMenuW(m, MF_STRING | (in ? MF_CHECKED : 0), 100 + i, pls[i].name.c_str());
    }
    AppendMenuW(m, MF_STRING, 90, L"Neue Playlist …");
    int cmd = TrackMenu(m);
    if (cmd >= 100 && cmd < 100 + (int)pls.size()) {
        auto& v = pls[cmd - 100].ids;
        if (std::find(v.begin(), v.end(), id) != v.end()) v.erase(std::remove(v.begin(), v.end(), id), v.end());  // Haken = wieder entfernen
        else v.push_back(id);
        MusicChanged();
    } else if (cmd == 90) {
        g.namingFor = songIdx;
        g.creatingFolder = true;
        g.query.clear();
        if (g.view != View::MusicSettings) { ShowView(View::MusicSettings); g.creatingFolder = true; g.namingFor = songIdx; }
        Refresh();
    }
}

// Rechtsklick auf einen Song
static void SongMenu(int songIdx, int pl, int pos) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Abspielen");
    AppendMenuW(m, MF_STRING, 2, L"Bearbeiten / Schneiden …");
    AppendMenuW(m, MF_STRING, 3, L"Zu Playlist hinzufügen …");
    bool fav = IsFav(Music_Songs()[songIdx].id);
    AppendMenuW(m, MF_STRING, 6, fav ? L"Aus Favoriten entfernen" : L"Zu Favoriten ♥");
    if (pl >= 0) AppendMenuW(m, MF_STRING, 4, L"Aus dieser Playlist entfernen");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 5, L"Im Explorer zeigen");
    int cmd = TrackMenu(m);
    if (cmd == 1) PlayList(pl, pos >= 0 ? pos : 0);
    else if (cmd == 2) OpenSongEdit(songIdx);
    else if (cmd == 3) AddToPlaylistMenu(songIdx);
    else if (cmd == 4) PlaylistRemove(pl, songIdx);
    else if (cmd == 6) { ToggleFav(Music_Songs()[songIdx].id); Refresh(); }
    else if (cmd == 5) {
        std::wstring args = L"/select,\"" + Music_Songs()[songIdx].file + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

static bool ConfirmBox(const std::wstring& msg) {
    g.menuOpen = true;
    int r = MessageBoxW(g.hwnd, msg.c_str(), L"HoverBar", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_TOPMOST);
    g.menuOpen = false;
    SetForegroundWindow(g.hwnd);
    return r == IDYES;
}

// Wohin fällt ein gezogener Song? dropF >= 0: Playlist, -2: rechts = aus der Playlist nehmen
static void UpdateDropMusic() {
    POINT pt = g.mousePt;
    g.dropF = g.dropPos = g.dropLineY = -1;
    if (pt.x >= g.paneMid) {
        if (g.dragFromF >= 0) g.dropF = -2;
        return;
    }
    pt.y = std::max((LONG)g.lTop, std::min(pt.y, (LONG)g.lBot - 1));
    for (auto& h : g.hits) {
        if (h.pane != 1 || pt.y < h.rc.top || pt.y >= h.rc.bottom) continue;
        if (h.type == HT::PlSongRow) {
            bool before = pt.y < (h.rc.top + h.rc.bottom) / 2;
            g.dropF = h.f;
            g.dropPos = before ? h.pos : h.pos + 1;
            g.dropLineY = before ? h.rc.top : h.rc.bottom;
            return;
        }
        if (h.type == HT::PlHead) {
            g.dropF = h.f;
            g.dropPos = 0;
            g.dropLineY = h.rc.bottom;
            return;
        }
        if (h.type == HT::PlDrop) {
            g.dropF = h.f;
            g.dropPos = 0;
            return;
        }
    }
    int best = -1, bestBottom = INT_MIN;
    for (auto& h : g.hits)
        if (h.type == HT::SetCard && h.pane == 1 && h.f >= 0 && h.rc.top <= pt.y && h.rc.bottom > bestBottom) {
            best = h.f;
            bestBottom = h.rc.bottom;
        }
    if (best >= 0) {
        g.dropF = best;
        g.dropPos = (int)g.plItems[best].size();
        g.dropLineY = bestBottom - S(6);
    }
}

// ---------------------------------------------------------------- Einstellungen: Ziehen & Ablegen

// Wohin würde die gezogene App fallen? dropF >= 0: Ordner, -2: rechts = aus dem Ordner nehmen
static void UpdateDrop() {
    if (g.dragMusic) { UpdateDropMusic(); return; }
    POINT pt = g.mousePt;
    g.dropF = g.dropPos = g.dropLineY = -1;
    if (pt.x >= g.paneMid) {
        // rechts ablegen = aus dem Favoriten-Feld bzw. aus dem Ordner nehmen
        if (g.dragFromFav >= 0 || !g.apps[g.dragApp].folder.empty()) g.dropF = -2;
        return;
    }
    if (pt.y >= g.lTop && pt.y < g.lBot)
        for (auto& h : g.hits)
            if (h.type == HT::FavSlot && PtInRect(&h.rc, pt)) {
                g.dropF = -3;
                g.dropPos = h.pos;
                return;
            }
    if (g.dragFromFav >= 0) return;  // aus einem Favoriten-Feld nur in andere Felder oder nach rechts
    pt.y = std::max((LONG)g.lTop, std::min(pt.y, (LONG)g.lBot - 1));
    for (auto& h : g.hits) {
        if (h.pane != 1 || pt.y < h.rc.top || pt.y >= h.rc.bottom) continue;
        if (h.type == HT::SetAppRow) {
            bool before = pt.y < (h.rc.top + h.rc.bottom) / 2;
            g.dropF = h.f;
            g.dropPos = before ? h.pos : h.pos + 1;
            g.dropLineY = before ? h.rc.top : h.rc.bottom;
            return;
        }
        if (h.type == HT::SetFolderHead) {
            g.dropF = h.f;
            g.dropPos = 0;
            g.dropLineY = h.rc.bottom;
            return;
        }
        if (h.type == HT::SetDrop) {
            g.dropF = h.f;
            g.dropPos = 0;
            return;
        }
    }
    // Lücke zwischen Ordnern bzw. unten in einem Ordner: ans Ende des Ordners darüber
    int best = -1, bestBottom = INT_MIN;
    for (auto& h : g.hits)
        if (h.type == HT::SetCard && h.pane == 1 && h.f >= 0 && h.rc.top <= pt.y && h.rc.bottom > bestBottom) {
            best = h.f;
            bestBottom = h.rc.bottom;
        }
    if (best >= 0) {
        g.dropF = best;
        g.dropPos = (int)g.allFolders[best].items.size();
        g.dropLineY = bestBottom - S(6);
    }
}

static void EndDrag() {
    bool had = g.dragCand;
    g.dragCand = g.dragging = false;
    g.dragApp = g.dragFromF = g.dropF = g.dropPos = g.dropLineY = g.dragFromFav = g.dragFromPos = -1;
    g.dragMusic = false;
    KillTimer(g.hwnd, T_DRAG);
    if (had && GetCapture() == g.hwnd) ReleaseCapture();
}

// Medientaste drücken (wie auf der Tastatur): Windows leitet sie an die laufende Wiedergabe weiter,
// auch an ein per Bluetooth verbundenes Handy (AVRCP)
static void SendMediaKey(WORD vk) {
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = vk;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
    Media_Command(MC_NONE);  // gleich danach nach einer (neuen) Sitzung schauen
}

static void Activate(int hitIdx, bool right) {
    if (hitIdx < 0 || hitIdx >= (int)g.hits.size()) {
        if (right && g.view != View::Settings) MainMenu();
        return;
    }
    Hit h = g.hits[hitIdx];
    if (right) {
        if (IsSongHit(h.type)) { SongMenu(h.idx, h.f, h.pos); return; }
        if ((h.type == HT::NpCover || h.type == HT::BarCover) && g.pst.active) {  // Cover: Menü des laufenden Songs
            int si = Music_Find(g.pst.songId);
            if (si >= 0) {
                int pos = (int)(std::find(g.mAll.begin(), g.mAll.end(), si) - g.mAll.begin());
                SongMenu(si, -1, pos < (int)g.mAll.size() ? pos : 0);
            }
            return;
        }
        if ((h.type == HT::MCard || h.type == HT::MAlbum) && h.idx >= 0) {
            if (ConfirmBox(L"Playlist \u201E" + PlaylistName(h.idx) + L"\u201C löschen? Die Songs bleiben erhalten.")) {
                Music_Playlists().erase(Music_Playlists().begin() + h.idx);
                MusicChanged();
            }
            return;
        }
        if (h.type == HT::LinkChip) { LinkMenu(h.idx); return; }
        if (h.type == HT::CalEvent) { EventMenu(h.idx); return; }
        if (h.type == HT::Slot || h.type == HT::Cell || h.type == HT::GameRow || h.type == HT::AppSizeRow || h.type == HT::SetAppRow || h.type == HT::AllAppRow ||
            ((h.type == HT::Fav || h.type == HT::FavSlot) && h.idx >= 0)) AddMenu(h.idx);
        else if (h.type == HT::Card || h.type == HT::Mini) FolderMenu(FolderOfApp(g.folders[h.idx].items[0]));
        else if (h.type == HT::SetFolderHead || h.type == HT::SetFolderMenu) FolderMenu(h.f);
        else if (h.type == HT::Bluetooth) BluetoothMenu();
        else if (g.view != View::Settings) MainMenu();
        return;
    }
    switch (h.type) {
    case HT::TimerBtn:
        TimerButton(h.idx);
        Refresh();
        break;
    case HT::Gear:
        OpenSettings(0);
        break;
    // ---------- Musik
    case HT::PlayMode: {
        // jeder Klick: Aus -> Zufall -> Playlist wiederholen -> Song wiederholen -> Aus
        int mode = g.repeat == 1 ? 3 : g.shuffle ? 1 : g.repeat == 2 ? 2 : 0;
        mode = (mode + 1) % 4;
        g.shuffle = mode == 1;
        g.repeat = mode == 2 ? 2 : mode == 3 ? 1 : 0;
        Player_SetModes(g.shuffle, g.repeat);
        IniSet(L"Einstellungen", L"Zufall", g.shuffle ? L"1" : L"0");
        IniSet(L"Einstellungen", L"Wiederholen", std::to_wstring(g.repeat).c_str());
        Render();
        break;
    }
    case HT::MusicOpen:  // Bibliothek an/aus: zweiter Klick führt zurück zu den Apps
        g.query.clear();
        if (MusicView()) ShowView(View::Folders);
        else ShowView(View::Music);
        break;
    case HT::MusicBar:
        if (g.view != View::Music) { g.query.clear(); ShowView(View::Music); }
        break;
    case HT::LibTab:  // nochmal anklicken -> zurück zu "Alle Songs"
        g.libTab = g.libTab == h.idx ? 0 : h.idx;
        StopScrollAnim();
        g.scroll = 0;
        g.hot = -1;
        Refresh();
        break;
    case HT::FavBtn:
        if (g.pst.active) {
            ToggleFav(g.pst.songId);
            Refresh();
        }
        break;
    case HT::BarCover:  // Cover in der Musikleiste -> "Läuft gerade"
        g.npReturn = g.view == View::Search ? View::Folders : g.view;
        g.npFolder = g.curFolder;
        g.query.clear();
        ShowView(View::NowPlaying);
        break;
    case HT::NpCover:   // großes Cover: zurück
        LeaveNowPlaying();
        break;
    case HT::NpLyrics:  // Zeile anklicken -> dorthin spulen
        for (auto& lr : g.lyrRects)
            if (PtInRect(&lr.first, g.mousePt) && g.pst.dur > 0) {
                g.lyrManual = false;
                KillTimer(g.hwnd, T_LYRBACK);
                Player_Seek(std::max(0.0, lr.second) / g.pst.dur);
                g.pst = Player_State();
                Render();
                break;
            }
        break;
    case HT::MProgress: {
        POINT pt = g.mousePt;
        double fr = (pt.x - h.rc.left) / (double)std::max(1L, h.rc.right - h.rc.left);
        Player_Seek(fr);
        g.pst = Player_State();
        Render();
        break;
    }
    case HT::MSize:
        g.musicSize = h.idx;
        IniSet(L"Einstellungen", L"MusikGroesse", std::to_wstring(h.idx).c_str());
        Render();
        break;
    case HT::MusicManage:
        g.query.clear();
        g.creatingFolder = false;
        g.namingFor = -1;
        ShowView(View::MusicSettings);
        break;
    case HT::VolIcon:
        if (g.volume > 0) { g.volumeBeforeMute = g.volume; g.volume = 0; }
        else g.volume = g.volumeBeforeMute > 0 ? g.volumeBeforeMute : 80;
        Player_SetVolume(VolumeGain(g.volume));
        IniSet(L"Einstellungen", L"Lautstaerke", std::to_wstring(g.volume).c_str());
        Render();
        break;
    case HT::MAlbum:
    case HT::MCard:
    case HT::MMore:
        g.curPlaylist = h.idx;
        ShowView(View::MusicList);
        break;
    case HT::MTile:
    case HT::MRow:
        PlayList(h.f, h.pos);
        break;
    case HT::MPlayAll:
        PlayList(h.idx, 0);
        break;
    case HT::MReload:  // Songs vom Handy (oder von Hand hineinkopierte) und Handy-Änderungen übernehmen, sobald alles da ist
        if (g.reload == 1) break;
        g.reload = 1;
        g.reloadTick = g.reloadStable = 0;
        g.reloadSnap = 0;
        SetTimer(g.hwnd, T_RELOAD, 100, nullptr);
        Render();
        break;
    case HT::MSort:
        g.sortPlays = !g.sortPlays;
        IniSet(L"Musik", L"NachWiedergaben", g.sortPlays ? L"1" : L"0");
        BuildMusic();
        Refresh();
        break;
    case HT::MusicJob:
        Music_ClearFinishedJobs();
        Refresh();
        break;
    case HT::PlUp:
    case HT::PlDown:
        if ((h.type == HT::PlUp && h.pos > 0) || (h.type == HT::PlDown && h.pos < h.n - 1)) {
            auto& pls = Music_Playlists();
            std::swap(pls[h.f], pls[h.f + (h.type == HT::PlUp ? -1 : 1)]);
            MusicChanged();
        }
        break;
    case HT::PlDelete:
        if (ConfirmBox(L"Playlist \u201E" + PlaylistName(h.f) + L"\u201C löschen? Die Songs bleiben erhalten.")) {
            Music_Playlists().erase(Music_Playlists().begin() + h.f);
            MusicChanged();
        }
        break;
    case HT::AllSongAdd:
        AddToPlaylistMenu(h.idx);
        break;
    case HT::AllSongEdit:
        OpenSongEdit(h.idx);
        break;
    case HT::EditField:
        g.editField = h.idx;
        g.caretOn = true;
        Render();
        break;
    case HT::EditBtn:
        if (h.idx == 0) {
            PickLocalCover();
            Refresh();
        } else if (h.idx == 1) {
            g.coverSearching = true;
            g.coverResults.clear();
            g.coverNone = false;
            Music_SearchCovers(g.editId, g.editArtist, g.editTitle);
            Refresh();
        } else if (h.idx == 2) {
            Music_UpdateMeta(g.editId, g.editTitle, g.editArtist);
            BuildMusic();
            LeaveSongEdit();
        } else if (h.idx == 4 || h.idx == 5) {  // Probehören: ab Anfang bzw. die letzten 5 s vor dem Ende
            g.trimPrevFrom = h.idx == 4 ? g.trimA : std::max(g.trimA, g.trimB - 5);
            Player_Preview(g.editId, g.trimPrevFrom, g.trimB);
            StartDuckWatch(g.hwnd);
            g.pst = Player_State();
            Render();
        } else if (h.idx == 6 || h.idx == 7 || h.idx == 8) {
            bool undo = h.idx == 8;
            if (undo && !Music_UndoCount(g.editId)) break;
            if (!undo && g.trimA < 0.05 && g.trimB > g.trimLen - 0.05) break;  // nichts ausgewählt
            if (Player_State().active) Player_Stop();  // Datei freigeben
            g.pst = PlayerState();
            bool ok = undo ? Music_Undo(g.editId) : Music_Trim(g.editId, g.trimA, g.trimB, h.idx == 6);
            if (ok) {
                g.trimLen = Music_Length(g.editId);
                g.trimA = 0;
                g.trimB = g.trimLen;
                g.trimSel = 0;
                g.wave.clear();
                Music_LoadWave(g.editId, 400);
                BuildMusic();
            } else {
                g.menuOpen = true;
                MessageBoxW(g.hwnd, undo ? L"Rückgängig hat nicht geklappt." : L"Schneiden hat nicht geklappt (Auswahl zu kurz oder zu lang?).",
                            L"HoverBar", MB_OK | MB_ICONWARNING | MB_TOPMOST);
                g.menuOpen = false;
            }
            Refresh();
        } else if (h.idx == 3) {
            int si = Music_Find(g.editId);
            if (si >= 0 && ConfirmBox(L"\u201E" + Music_Songs()[si].title + L"\u201C endgültig löschen? Die MP3-Datei wird entfernt.")) {
                Music_Delete(g.editId);
                BuildMusic();
                LeaveSongEdit();
            }
        }
        break;
    case HT::CoverPick:
        if (h.idx < (int)g.coverResults.size()) {
            Music_SetCoverFromFile(g.editId, g.coverResults[h.idx]);
            g.coverResults.clear();
            Refresh();
        }
        break;
    case HT::Tab:
        if (g.setTab != h.idx) {
            g.setTab = h.idx;
            g.creatingFolder = false;
            g.namingFor = -1;
            g.query.clear();
            ShowView(View::Settings);
        }
        break;
    case HT::SetToggle:
        if (h.idx == 0) {
            g.blur = !g.blur;
            IniSet(L"Einstellungen", L"Unschaerfe", g.blur ? L"1" : L"0");
            if (g.blur) Recapture();
        } else if (h.idx == 1) {
            SetAutostart(!AutostartEnabled());
        } else if (h.idx == 3) {
            g.fontBold = !g.fontBold;
            IniSet(L"Einstellungen", L"Fett", g.fontBold ? L"1" : L"0");
            SetDpi(g.dpi, true);
        } else if (h.idx == 10 || h.idx == 11) {  // Musikquelle (die beiden Schalter schließen sich aus)
            int src = h.idx == 11 ? (g.musicSrc == 1 ? 0 : 1) : (g.musicSrc == 0 ? 1 : 0);
            SetMusicSource(src);
        } else if (h.idx >= 16 && h.idx <= 19) {  // Spiele-Quellen
            Games_SetSourceOn(h.idx - 16, !Games_SourceOn(h.idx - 16));
            UpdateGameWatch();
        } else if (h.idx == 15) {  // Musik-Player ganz an/aus
            g.musicOn = !g.musicOn;
            IniSet(L"Funktionen", L"MusikPlayer", g.musicOn ? L"1" : L"0");
            if (!g.musicOn) {
                if (!g.appsOn) { g.appsOn = true; IniSet(L"Funktionen", L"AppLauncher", L"1"); }  // eins von beiden bleibt an
                if (Player_State().active) Player_Stop();
                g.pst = PlayerState();
                KillTimer(g.hwnd, T_DUCK);
                Media_Close();
                g.media = MediaInfo();
                if (g.mediaArt) { DeleteObject(g.mediaArt); g.mediaArt = nullptr; }
                UnregisterHotKey(g.hwnd, 5);
            } else {
                RegisterGrabKey();
                if (SpotifyMode() && g.open) Media_Open(g.hwnd, S(kBarArt));
            }
        } else if (h.idx == 23) {
            g.calendarOn = !g.calendarOn;
            IniSet(L"Funktionen", L"Kalender", g.calendarOn ? L"1" : L"0");
        } else if (h.idx == 22) {
            g.appsOn = !g.appsOn;
            IniSet(L"Funktionen", L"AppLauncher", g.appsOn ? L"1" : L"0");
            if (g.appsOn) {
                if (!g.loaded && !g.loading) StartLoad();
            } else if (!g.musicOn) {  // sonst bliebe nichts übrig
                g.musicOn = true;
                IniSet(L"Funktionen", L"MusikPlayer", L"1");
                RegisterGrabKey();
            }
        } else if (h.idx == 20 || h.idx == 21) {
            bool& v = h.idx == 20 ? g.tileCovers : g.songLines;
            v = !v;
            IniSet(L"Einstellungen", h.idx == 20 ? L"KachelCover" : L"Trennlinien", v ? L"1" : L"0");
        } else if (h.idx == 12) {
            g.duckOn = !g.duckOn;
            IniSet(L"Funktionen", L"LeiserBeiVideos", g.duckOn ? L"1" : L"0");
            if (!g.duckOn) { KillTimer(g.hwnd, T_DUCK); sDuck = 1.0; sDucked = false; Player_SetDuck(1.0); }
            else if (Player_State().active) StartDuckWatch(g.hwnd);
        } else if (h.idx == 13) {
            g.musicKeys = !g.musicKeys;
            IniSet(L"Funktionen", L"MusikTasten", g.musicKeys ? L"1" : L"0");
        } else if (h.idx == 14) {
            g.grabKey = !g.grabKey;
            IniSet(L"Funktionen", L"LinkTaste", g.grabKey ? L"1" : L"0");
            UnregisterHotKey(g.hwnd, 5);
            RegisterGrabKey();
        } else {
            g.onlyDesktop = !g.onlyDesktop;
            IniSet(L"Einstellungen", L"NurDesktop", g.onlyDesktop ? L"1" : L"0");
        }
        Refresh();
        break;
    case HT::SetSwatch:
        if (h.idx < kSwatchCount) {
            g.tint = kSwatches[h.idx].tint;
            SaveTint();
        } else {
            PickColor();
        }
        Render();
        break;
    case HT::SetSpeed:
        g.animSpeed = h.idx;
        IniSet(L"Einstellungen", L"Tempo", std::to_wstring(h.idx).c_str());
        Render();
        break;
    case HT::CtlKeyBtn:  // nochmal klicken = abbrechen
        g.ctlCapture = g.ctlCapture == h.idx ? -1 : h.idx;
        Render();
        break;
    case HT::GrabKeyBtn:  // neue Taste für "Link laden" aufnehmen (nochmal klicken = abbrechen)
        g.grabCapture = !g.grabCapture;
        UnregisterHotKey(g.hwnd, 5);
        if (!g.grabCapture) RegisterGrabKey();
        Render();
        break;
    case HT::SetButton:
        if (h.idx == 2) { FontMenu(); break; }
        if (h.idx == 3) {  // neue Taste aufnehmen
            g.hotCapture = !g.hotCapture;
            if (g.hotCapture) UnregisterHotKey(g.hwnd, 1);
            else if (g.hotVk) RegisterHotKey(g.hwnd, 1, g.hotMods | MOD_NOREPEAT, g.hotVk);
            Render();
            break;
        }
        if (h.idx == 0) PickImage();
        else if (!g.imgPath.empty()) {
            g.imgPath.clear();
            g.imgLoaded.clear();
            IniSet(L"Einstellungen", L"Bild", nullptr);
        }
        if (g.blur) Recapture();
        Refresh();
        break;
    case HT::SetFolderUp:
        if (h.pos > 0) { MoveFolder(h.f, -1); Refresh(); }
        break;
    case HT::SetFolderDown:
        if (h.pos < h.n - 1) { MoveFolder(h.f, +1); Refresh(); }
        break;
    case HT::SetFolderDelete:
        if (ConfirmDeleteFolder(g.allFolders[h.f].name)) { DeleteFolder(g.allFolders[h.f].name); Refresh(); }
        break;
    case HT::SetFolderMenu:
        FolderMenu(h.f);
        break;
    case HT::SetFolderGame:
        if (h.f < (int)g.allFolders.size()) {
            const std::wstring& n = g.allFolders[h.f].name;
            IniSet(L"Spielmodus", n, FolderGameMode(n) ? L"0" : L"1");
            UpdateGameWatch();
            Refresh();
        }
        break;
    case HT::GameRow:
    case HT::AppSizeRow:
        Launch(h.idx);
        break;
    case HT::Nav:
        GoToPage(h.idx);
        break;
    case HT::LinkChip:
        if (h.idx < (int)g.links.size()) OpenLink(g.links[h.idx].url);
        break;
    case HT::LinkAdd:
        AskAddLink();
        break;
    case HT::ToolBtn:
        Music_ToolUpdate(g.hwnd);
        Render();
        break;
    case HT::UpdateBtn: {
        UpdateStatus us = Update_Status();
        if (us.state == UPD_AVAILABLE) {
            if (ConfirmBox(L"HoverBar " + us.latest + L" von GitHub herunterladen und installieren?\n\nDie Datei wird per Prüfsumme (SHA-256) kontrolliert. "
                           L"Danach startet HoverBar neu.")) Update_Install(g.hwnd);
        } else if (us.state == UPD_READY) {
            Update_Restart();
            DestroyWindow(g.hwnd);
        } else {
            Update_Check(g.hwnd);
        }
        Render();
        break;
    }
    case HT::FolderBtn:
        PickSongFolder();
        Refresh();
        break;
    case HT::PlugBtn: {
        if (h.idx == 22) {  // Handy verbinden: QR-Code
            ShowPhoneQr();
            break;
        }
        if (h.idx == 20 || h.idx == 21) {  // Musik-Sync
            if (Music_SyncStatus().state == 1) break;
            if (h.idx == 21) {
                if (ConfirmBox(L"Musik-Sync ausschalten?\n\nDie Songs bleiben im Cloud-Ordner, HoverBar spielt sie weiter von dort. "
                               L"Die Bibliotheksdatei wird nicht mehr aktualisiert.")) {
                    g.syncOn = false;
                    IniSet(L"Erweiterungen", L"MusikSync", L"0");
                    Music_SetSyncExport(false);
                }
            } else if (g.syncOn) {  // abgleichen: verstreute Songs in den Ordner holen, Bibliothek neu schreiben
                Music_Init(g.hwnd);
                Music_SetFavorites(g.favIds);
                if (Player_State().active) { Player_Stop(); g.pst = PlayerState(); }
                Music_SyncSetup(g.hwnd, Music_Folder());
            } else {
                CloudSet(0, L"");
                CloudWaitStop();  // von Hand gestartet: nicht mehr automatisch warten
                SyncSetupFlow();
            }
            Refresh();
            break;
        }
        if (h.idx == 12 || h.idx == 13) {  // Timer hinzufügen / öffnen / entfernen
            if (h.idx == 12 && g.timerOn) { GoToPage(5); break; }
            g.timerOn = h.idx == 12;
            IniSet(L"Erweiterungen", L"Timer", g.timerOn ? L"1" : L"0");
            if (!g.timerOn) {
                if (g.timerRinging) StopRinging();
                g.timerEnd = 0;
                g.timerLeft = 0;
                KillTimer(g.hwnd, T_COUNT);
            }
            Refresh();
            break;
        }
        if (h.idx == 10 || h.idx == 11) {  // Claude-Chat hinzufügen / öffnen / entfernen
            if (h.idx == 11) {
                if (ConfirmBox(L"Claude-Chat wieder entfernen?\nDer Claude-Knopf links verschwindet, die gespeicherte Anmeldung bei claude.ai wird gelöscht.")) {
                    g.claudeOn = false;
                    IniSet(L"Erweiterungen", L"Claude", L"0");
                    Chat_Free();
                    PWSTR lp = nullptr;
                    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &lp))) {  // Profil der Webansicht (Anmeldung, Zwischenspeicher)
                        std::wstring d = std::wstring(lp) + L"\\HoverBar\\webview";
                        d.push_back(L'\0');
                        SHFILEOPSTRUCTW op = {};
                        op.wFunc = FO_DELETE;
                        op.pFrom = d.c_str();
                        op.fFlags = FOF_NO_UI | FOF_NOCONFIRMATION;
                        SHFileOperationW(&op);
                    }
                    CoTaskMemFree(lp);
                    if (g.view == View::Chat) { ShowView(View::Folders); break; }
                }
            } else if (!g.claudeOn) {
                if (ConfirmBox(L"Claude-Chat hinzufügen?\n\nclaude.ai wird in einer eigenen Seite der Leiste angezeigt (WebView2, bei Windows 11 dabei). "
                               L"Du brauchst ein Konto bei claude.ai. Beim Öffnen der Seite wird claude.ai geladen; sonst wird nichts übertragen.")) {
                    g.claudeOn = true;
                    IniSet(L"Erweiterungen", L"Claude", L"1");
                }
            } else {
                GoToPage(4);
                break;
            }
            Refresh();
            break;
        }
        PlugStatus ps = WizTree_Status();
        if (ps.state == PLUG_WORKING || ps.state == PLUG_DOWNLOADING) break;
        if (h.idx == 1) {
            if (ConfirmBox(L"WizTree wieder entfernen?\nDer Speicher-Knopf links verschwindet dann.")) {
                if (!WizTree_Remove()) {
                    g.menuOpen = true;
                    MessageBoxW(g.hwnd, L"WizTree konnte nicht entfernt werden – läuft es noch?", L"HoverBar", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
                    g.menuOpen = false;
                }
            }
        } else if (WizTree_Exe().empty()) {
            if (ConfirmBox(L"WizTree von diskanalyzer.com herunterladen (ca. 8 MB)?\n\nWizTree ist für private Nutzung kostenlos. "
                           L"Für die Nutzung in Firmen braucht man eine Lizenz.")) WizTree_Install(g.hwnd);
        } else {
            WizTree_Open(L"C:\\");
            CloseBar(L"WizTree");
            break;
        }
        Refresh();
        break;
    }
    case HT::DriveOpen: {
        wchar_t root[4] = {(wchar_t)(L'A' + h.idx), L':', L'\\', 0};
        if (WizTree_Open(root)) CloseBar(L"WizTree");
        break;
    }
    case HT::ChatNew:
        Chat_Navigate(L"https://claude.ai/new");
        break;
    case HT::StorageRefresh: {
        // Laufwerke werden ohnehin live gelesen; App-Größen, Launcher-Daten und Programmliste neu einlesen/messen
        Apps_RefreshSizes();
        for (auto& a : g.apps)
            if (!a.hidden) Apps_Size({a.id, a.file, a.name}, a.target);  // stellt veraltete Messungen in die Warteschlange
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t b[8];
        swprintf(b, 8, L"%02d:%02d", t.wHour, t.wMinute);
        g.storageStamp = b;
        Refresh();
        break;
    }
    case HT::StorageApps:
        g.sortBySize = true;
        g.onlyRunning = false;
        g.query.clear();
        ShowView(View::AllApps);
        break;
    case HT::CalNav:
        if (h.idx == 2) {
            int t = Today();
            g.calY = t / 10000;
            g.calM = t / 100 % 100;
            g.calSel = t;
        } else {
            g.calM += h.idx == 1 ? 1 : -1;
            if (g.calM < 1) { g.calM = 12; g.calY--; }
            if (g.calM > 12) { g.calM = 1; g.calY++; }
        }
        Refresh();
        break;
    case HT::CalDay:
        g.calSel = h.idx;
        if (h.f == 1) { g.calY = h.idx / 10000; g.calM = h.idx / 100 % 100; }  // Tag aus Nachbarmonat: dorthin blättern
        Refresh();
        break;
    case HT::CalAdd: {
        State::CalEntry e = {g.calSel, L"", L""};
        if (AskEvent(g.calSel, e)) {
            g.events.push_back(e);
            SaveEvents();
            Refresh();
        }
        break;
    }
    case HT::CalEvent:
        EventMenu(h.idx);
        break;
    case HT::RecentItem:
        if (h.idx < (int)g.recent.size()) {
            auto e = g.recent[h.idx];
            if (e.link) OpenLink(e.url);
            else if (e.app >= 0) Launch(e.app);
        }
        break;
    case HT::TitleBtn:
        g.query.clear();
        ShowView(View::AllApps);
        break;
    case HT::SortBtn:
        g.onlyRunning = h.idx == 2;
        if (h.idx < 2) g.sortBySize = h.idx == 1;
        RefreshRunning();
        SortAppSizes();
        g.scroll = 0;
        Refresh();
        break;
    case HT::KillBtn:
        EndApp(h.idx);
        break;
    case HT::AllAppAdd:
        AddMenu(h.idx);
        break;
    case HT::NewFolder:
        if (!g.creatingFolder) BeginNaming(-1);
        break;
    case HT::SearchR:
        if (g.creatingFolder) { g.creatingFolder = false; g.namingFor = -1; g.query.clear(); Refresh(); }
        break;
    case HT::Fav:
        if (h.idx >= 0) Launch(h.idx);
        else OpenSettings(1);  // freies Feld: gleich zur Auswahl
        break;
    case HT::Slot:
    case HT::Cell:
        Launch(h.idx);
        break;
    case HT::Card:
    case HT::Mini:
        ShowView(View::Folder, h.idx);
        break;
    case HT::Back:
        if (g.view == View::SongEdit) { LeaveSongEdit(); break; }
        if (g.view == View::NowPlaying) { LeaveNowPlaying(); break; }
        if (g.view == View::MusicList || g.view == View::MusicSettings) { g.query.clear(); g.creatingFolder = false; g.namingFor = -1; ShowView(View::Music); break; }
        g.query.clear();
        g.creatingFolder = false;
        g.namingFor = -1;
        ShowView(View::Folders);
        break;
    case HT::Bluetooth:
        OpenBluetoothPanel();
        break;
    case HT::Wifi:  // Windows-Netzwerkauswahl
        ShellExecuteW(nullptr, L"open", L"ms-availablenetworks:", nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case HT::Battery:
        ShellExecuteW(nullptr, L"open", L"ms-settings:batterysaver", nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case HT::MediaPrev:
        if (g.pst.active) Player_Prev();
        else if (SpotifyMode() && g.media.has) Media_Command(MC_PREV);
        break;
    case HT::MediaNext:
        if (g.pst.active) Player_Next();
        else if (SpotifyMode() && g.media.has) Media_Command(MC_NEXT);
        break;
    case HT::MediaPlay:
        if (g.pst.active) {
            Player_Toggle();
            StartDuckWatch(g.hwnd);
            g.pst.playing = !g.pst.playing;
            Render();
            break;
        }
        if (SpotifyMode()) {  // Spotify: Play/Pause (startet Spotify, falls zu)
            Media_Command(MC_PLAYPAUSE);
            if (g.media.has) { g.media.playing = !g.media.playing; Render(); }
            break;
        }
        if (!g.mAll.empty()) PlayList(-1, 0);  // noch nichts geladen: alle Songs
        break;
    case HT::MediaPhone:
        g.mediaNote = L"Suche gekoppelte Handys …";
        g.noteUntil = GetTickCount64() + 4000;
        Render();
        Media_Command(MC_LISTPHONES);
        break;
    case HT::Menu:
        MainMenu();
        break;
    default:
        break;
    }
}

static std::wstring& EditTarget() {
    if (g.view == View::SongEdit) return g.editField == 2 ? g.editArtist : g.editTitle;
    return g.query;
}

// Schreibmarke gehört zum gerade aktiven Text; bei Feldwechsel ans Ende setzen
static void SyncCaret() {
    std::wstring* t = &EditTarget();
    if (t != g.caretOwner) {
        g.caretOwner = t;
        g.caret = g.anchor = (int)t->size();
        g.fieldOff = 0;
    }
    int n = (int)t->size();
    g.caret = std::max(0, std::min(g.caret, n));
    g.anchor = std::max(0, std::min(g.anchor, n));
}

static bool HasSelection() { return g.caret != g.anchor; }

static void InsertText(const std::wstring& s) {
    SyncCaret();
    std::wstring& t = EditTarget();
    int a = std::min(g.caret, g.anchor), b = std::max(g.caret, g.anchor);
    t.replace(a, b - a, s);
    g.caret = g.anchor = a + (int)s.size();
}

static bool IsWordChar(wchar_t c) { return IsCharAlphaNumericW(c) || c == L'_'; }
static int WordLeft(const std::wstring& t, int i) {
    while (i > 0 && !IsWordChar(t[i - 1])) i--;
    while (i > 0 && IsWordChar(t[i - 1])) i--;
    return i;
}
static int WordRight(const std::wstring& t, int i) {
    int n = (int)t.size();
    while (i < n && !IsWordChar(t[i])) i++;
    while (i < n && IsWordChar(t[i])) i++;
    return i;
}

// Zeichenposition zu einer x-Koordinate im aktiven Feld
static int IndexAtX(int x) {
    const std::wstring& t = EditTarget();
    HDC dc = GetDC(nullptr);
    HGDIOBJ o = SelectObject(dc, g.fText);
    int rel = x - (g.activeField.left + S(14)) + g.fieldOff, best = 0, prev = 0;
    for (int i = 1; i <= (int)t.size(); i++) {
        SIZE e = {};
        GetTextExtentPoint32W(dc, t.c_str(), i, &e);
        if (rel >= (prev + e.cx) / 2) best = i;
        prev = e.cx;
    }
    SelectObject(dc, o);
    ReleaseDC(nullptr, dc);
    return best;
}

static void CopySelection(bool cut) {
    SyncCaret();
    if (!HasSelection()) return;
    std::wstring& t = EditTarget();
    int a = std::min(g.caret, g.anchor), b = std::max(g.caret, g.anchor);
    std::wstring s = t.substr(a, b - a);
    if (OpenClipboard(g.hwnd)) {
        EmptyClipboard();
        HGLOBAL hm = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
        if (hm) {
            memcpy(GlobalLock(hm), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(hm);
            if (!SetClipboardData(CF_UNICODETEXT, hm)) GlobalFree(hm);
        }
        CloseClipboard();
    }
    if (cut) InsertText(L"");
}

// Tasten zum Bearbeiten: Pfeile, Pos1/Ende, Entf, Rück, Strg+A/C/X/V – mit Umschalt markieren
static bool EditKey(WPARAM vk) {
    bool ctrl = GetKeyState(VK_CONTROL) & 0x8000, shift = GetKeyState(VK_SHIFT) & 0x8000;
    SyncCaret();
    std::wstring& t = EditTarget();
    int n = (int)t.size();
    auto moveTo = [&](int i) {
        g.caret = std::max(0, std::min(i, n));
        if (!shift) g.anchor = g.caret;
    };
    switch (vk) {
    case VK_LEFT:
        if (!shift && HasSelection()) moveTo(std::min(g.caret, g.anchor));
        else moveTo(ctrl ? WordLeft(t, g.caret) : g.caret - 1);
        break;
    case VK_RIGHT:
        if (!shift && HasSelection()) moveTo(std::max(g.caret, g.anchor));
        else moveTo(ctrl ? WordRight(t, g.caret) : g.caret + 1);
        break;
    case VK_HOME: moveTo(0); break;
    case VK_END: moveTo(n); break;
    case VK_BACK:
        if (!HasSelection()) {
            if (g.caret == 0) return true;
            g.anchor = ctrl ? WordLeft(t, g.caret) : g.caret - 1;
        }
        InsertText(L"");
        break;
    case VK_DELETE:
        if (!HasSelection()) {
            if (g.caret >= n) return true;
            g.anchor = ctrl ? WordRight(t, g.caret) : g.caret + 1;
        }
        InsertText(L"");
        break;
    case 'A':
        if (!ctrl) return false;
        g.anchor = 0;
        g.caret = n;
        break;
    case 'C':
        if (!ctrl) return false;
        CopySelection(false);
        return true;
    case 'X':
        if (!ctrl) return false;
        CopySelection(true);
        break;
    default:
        return false;
    }
    g.caretOn = true;
    SetTimer(g.hwnd, T_CARET, 530, nullptr);
    OnQueryChanged();
    return true;
}

// Link als Song laden. Fehlt yt-dlp (es liegt nicht im Programmordner, sondern wird einmal heruntergeladen),
// fragt HoverBar nach und lädt den Link danach automatisch.
static void QueueLink(const std::wstring& url) {
    if (Music_ToolAvailable()) {
        Music_Init(g.hwnd);
        Music_AddUrl(url);
        return;
    }
    ToolStatus ts = Music_ToolStatus();
    if (ts.state == TOOL_CHECKING || ts.state == TOOL_DOWNLOADING || ts.state == TOOL_VERIFYING) { g.pendingUrl = url; return; }  // läuft schon
    Log(L"yt-dlp fehlt: frage nach dem Download");
    bool wasOpen = g.open;
    g.menuOpen = true;
    int r = MessageBoxW(wasOpen ? g.hwnd : nullptr,
                        L"Zum Umwandeln von YouTube-Links braucht HoverBar das Programm yt-dlp. Es fehlt noch.\n\n"
                        L"Jetzt von GitHub herunterladen (ca. 18 MB, mit Prüfsumme)? Der Link wird danach automatisch geladen.",
                        L"HoverBar", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND);
    g.menuOpen = false;
    if (r == IDYES) {
        g.pendingUrl = url;
        Music_ToolUpdate(g.hwnd);
    }
}

static bool Focusable(HT t) {
    switch (t) {
    case HT::None: case HT::SetCard: case HT::SetSection: case HT::SetRowLabel: case HT::Pane: case HT::PaneTitle: case HT::MusicEmpty:
    case HT::EditInfo: case HT::EditLabel: case HT::PlugCard: case HT::NavBar: case HT::MusicBar: case HT::RecentCard: case HT::CalHead:
    case HT::CalInfo: case HT::GameHead: case HT::SetHiddenHead: case HT::SetDrop: case HT::PlDrop: case HT::TimerWheel: case HT::TimerShow: return false;
    default: return true;
    }
}

// Pfeiltasten: die nächste anklickbare Fläche in dieser Richtung markieren und in den sichtbaren Bereich scrollen
static void KeyNav(WPARAM vk) {
    int dx = vk == VK_LEFT ? -1 : vk == VK_RIGHT ? 1 : 0, dy = vk == VK_UP ? -1 : vk == VK_DOWN ? 1 : 0;
    int cur = g.hot >= 0 && g.hot < (int)g.hits.size() && Focusable(g.hits[g.hot].type) ? g.hot : -1, best = -1;
    long bestScore = LONG_MAX;
    for (int i = 0; i < (int)g.hits.size(); i++) {
        const Hit& h = g.hits[i];
        if (i == cur || !Focusable(h.type)) continue;
        long score;
        if (cur < 0) {  // Einstieg: oberste Fläche des Inhalts
            if (IsHeaderHit(h.type) || h.rc.top < ContentTop()) continue;
            score = h.rc.top * 4 + h.rc.left;
        } else {
            const RECT& a = g.hits[cur].rc;
            long x = (h.rc.left + h.rc.right - a.left - a.right) / 2, y = (h.rc.top + h.rc.bottom - a.top - a.bottom) / 2;
            long along = x * dx + y * dy;
            if (along <= 0) continue;
            // Abstand quer zur Richtung: 0, solange sich die Flächen überlappen
            long gap = dx ? std::max(0L, std::max(h.rc.top - a.bottom, a.top - h.rc.bottom)) : std::max(0L, std::max(h.rc.left - a.right, a.left - h.rc.right));
            score = along + gap * 4 + std::labs(dx ? y : x) / 4;
        }
        if (score < bestScore) { bestScore = score; best = i; }
    }
    if (best < 0) return;
    g.kbdNav = true;
    const Hit t = g.hits[best];
    g.hot = best;
    if (!IsHeaderHit(t.type)) {
        RECT c = PaneClip(t.pane);
        int d = t.rc.top < c.top ? t.rc.top - c.top - S(8) : t.rc.bottom > c.bottom ? t.rc.bottom - c.bottom + S(8) : 0;
        if (d) {
            StopScrollAnim();
            (t.pane == 2 ? g.scrollR : g.scroll) += d;
            ClampScroll();
            Layout();
            for (int i = 0; i < (int)g.hits.size(); i++) {
                const Hit& h = g.hits[i];
                if (h.type == t.type && h.idx == t.idx && h.f == t.f && h.pos == t.pos) { g.hot = i; break; }
            }
        }
    }
    Render();
}

static void OnKeyDown(WPARAM vk) {
    bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
    bool typed = !g.query.empty() || g.view == View::SongEdit || g.namingFor >= 0 || g.creatingFolder;  // links/rechts gehören dann dem Textfeld
    bool trim = g.view == View::SongEdit && g.editField == 0 && g.trimSel;
    if (vk == VK_UP || vk == VK_DOWN || ((vk == VK_LEFT || vk == VK_RIGHT) && !typed && !trim)) { KeyNav(vk); return; }
    if ((vk == VK_RETURN || vk == VK_APPS) && g.kbdNav && g.hot >= 0 && g.hot < (int)g.hits.size()) {
        Activate(g.hot, vk == VK_APPS);  // Menü-Taste = Rechtsklick (z. B. Song bearbeiten)
        return;
    }
    if (vk == VK_ESCAPE) {
        if (g.view == View::SongEdit) LeaveSongEdit();
        else if (g.view == View::NowPlaying) LeaveNowPlaying();
        else if (g.namingFor >= 0 || g.creatingFolder) CancelNaming();
        else if (g.view == View::Music && g.query.empty() && g.libTab != 0) { g.libTab = 0; g.scroll = 0; Refresh(); }
        else if (!g.query.empty()) { g.query.clear(); OnQueryChanged(); }
        else if (g.view == View::MusicList || g.view == View::MusicSettings) ShowView(View::Music);
        else if (g.view == View::Folder || g.view == View::Settings || g.view == View::Music || g.view == View::AllApps || g.view == View::Calendar || g.view == View::Storage || g.view == View::Chat || g.view == View::Timer) {
            if (!g.appsOn && g.view == View::Music) CloseBar(L"Esc");
            else ShowView(View::Folders);
        }
        else CloseBar(L"Esc");
    } else if (vk == VK_SPACE && g.view == View::NowPlaying) {  // Leertaste: Play/Pause
        if (g.pst.active) { Player_Toggle(); g.pst = Player_State(); Render(); }
    } else if (vk == VK_RETURN) {
        if (g.view == View::SongEdit) {
            Music_UpdateMeta(g.editId, g.editTitle, g.editArtist);
            BuildMusic();
            LeaveSongEdit();
        } else if (g.view == View::Music) {
            std::wstring u = g.query;
            if (!u.empty()) {
                g.query.clear();
                QueueLink(u);
                Refresh();
            }
        } else if (g.namingFor >= 0 || g.creatingFolder) FinishNaming();
        else if (g.view != View::Settings && !g.results.empty()) Launch(g.results[0]);
    } else if ((vk == VK_LEFT || vk == VK_RIGHT) && g.view == View::SongEdit && g.editField == 0 && g.trimSel) {
        // Linie fein verschieben: 10 ms, mit Umschalt 100 ms
        double step = ((GetKeyState(VK_SHIFT) & 0x8000) ? 0.1 : 0.01) * (vk == VK_LEFT ? -1 : 1);
        if (g.trimSel == 1) g.trimA = std::max(0.0, std::min(g.trimA + step, g.trimB - 0.1));
        else g.trimB = std::min(g.trimLen, std::max(g.trimB + step, g.trimA + 0.1));
        Render();
    } else if (vk == VK_TAB && g.view == View::SongEdit) {
        g.editField = g.editField == 1 ? 2 : 1;
        Render();
    } else if (EditKey(vk)) {
        return;
    } else if (vk == 'V' && ctrl) {
        if (OpenClipboard(g.hwnd)) {
            HANDLE hd = GetClipboardData(CF_UNICODETEXT);
            if (hd) {
                if (auto* txt = (const wchar_t*)GlobalLock(hd)) {
                    std::wstring s = txt;
                    GlobalUnlock(hd);
                    s = s.substr(0, s.find_first_of(L"\r\n"));
                    InsertText(s);
                }
            }
            CloseClipboard();
            OnQueryChanged();
        }
    }
}

static bool TypingAllowed() {
    // Design-Seite hat kein Textfeld
    return !(g.view == View::Settings && g.setTab != 1) && g.view != View::NowPlaying && g.view != View::Timer;
}

// Lautstärkeregler aus-/einklappen je nachdem, ob die Maus darauf ist (einklappen mit kurzer Verzögerung)
static void UpdateVolHover() {
    bool want = g.sliderDrag == 3 ||
                (g.hot >= 0 && g.hot < (int)g.hits.size() && (g.hits[g.hot].type == HT::VolIcon || g.hits[g.hot].type == HT::BarVol));
    if (want) {
        g.volLeaveAt = 0;
        if (g.volTarget != 1) { g.volTarget = 1; SetTimer(g.hwnd, T_VOL, 10, nullptr); }
    } else if (g.volTarget == 1 && !g.volLeaveAt) {
        g.volLeaveAt = GetTickCount64() + 450;
        SetTimer(g.hwnd, T_VOL, 10, nullptr);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g.hwnd = hwnd;
        SetDpi(GetDpiForWindow(hwnd));
        SetTimer(hwnd, T_POLL, kPollMs, nullptr);
        // Taste anmelden; hält der Wächter sie noch kurz, gleich nochmal versuchen
        if (g.hotVk && !RegisterHotKey(hwnd, 1, g.hotMods | MOD_NOREPEAT, g.hotVk)) SetTimer(hwnd, T_HOTKEY, 300, nullptr);
        if (!RegisterGrabKey())  // markierten Link als Song laden
            Log((L"Link-Taste konnte nicht belegt werden (Fehler " + std::to_wstring(GetLastError()) + L") – ein anderes Programm nutzt die Tastenkombination").c_str());
        Media_SetSpotifyOnly(SpotifyMode());
        Games_Init(hwnd, g.ini);
        LoadLinks();  // Favoriten-Links, fehlende Website-Symbole im Hintergrund holen
        for (auto& l : g.links) StartFavicon(l.host);
        if (g.claudeOn) StartFavicon(L"claude.ai");  // Claude-Symbol für die Seitenleiste
        SetTimer(hwnd, T_GAMES, 60000, nullptr);  // Spielzeit messen: einmal pro Minute
        return 0;

    case WM_TIMER:
        if (wp == T_POLL) {
            if (g.open) return 0;
            POINT pt;
            if (!GetCursorPos(&pt)) return 0;
            HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = {sizeof(mi)};
            GetMonitorInfoW(mon, &mi);
            int corner = MulDiv(60, (int)GetDpiForSystem(), 96);  // Ecken frei lassen (Schließen-Knopf, Uhr)
            bool atEdge = pt.x >= mi.rcMonitor.right - 1 && pt.y > mi.rcMonitor.top + corner &&
                          pt.y < mi.rcMonitor.bottom - corner;
            bool button = (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000;
            if (!atEdge) { g.armed = true; g.edgeSince = 0; return 0; }
            if (!g.armed || button) return 0;
            DWORD now = GetTickCount();
            if (!g.edgeSince) { g.edgeSince = now; return 0; }
            if (now - g.edgeSince >= kDwellMs) {
                g.edgeSince = 0;
                g.armed = false;
                if (OpenAllowed(mon)) OpenBar(pt);
            }
        } else if (wp == T_WATCH) {
            // Bleibt offen, bis die Maus an den linken Rand geht (oder Esc / App-Start / Alt+Tab)
            if (!g.open || g.closing || g.menuOpen || g.animating || g.dragCand || g.sliderDrag >= 0) return 0;
            POINT pt;
            GetCursorPos(&pt);
            if (pt.x < g.monLeft + g.monW * 15 / 100) { CloseBar(L"Maus am linken Rand (15 %)"); return 0; }
            if (++g.watchTick % 10 == 0 && FullscreenActive()) CloseBar(L"Vollbild");
        } else if (wp == T_CARET) {
            g.caretOn = !g.caretOn;
            if (g.open && !g.closing && TypingAllowed()) Render();
        } else if (wp == T_DRAG) {
            if (!g.dragging) { KillTimer(hwnd, T_DRAG); return 0; }
            int edge = S(40), step = 0;
            if (g.mousePt.x < g.paneMid) {
                if (g.mousePt.y < g.lTop + edge) step = -S(14);
                else if (g.mousePt.y > g.lBot - edge) step = S(14);
            }
            if (step) {
                int before = g.scroll;
                g.scroll += step;
                ClampScroll();
                if (g.scroll != before) { Layout(); UpdateDrop(); Render(); }
            }
        } else if (wp == T_SCROLL) {
            // weich zum Ziel gleiten (unabhängig von der Bildrate)
            LARGE_INTEGER now, fq;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&fq);
            double dt = (now.QuadPart - g.scrollT.QuadPart) / (double)fq.QuadPart;
            g.scrollT = now;
            dt = std::min(dt, 0.05);                  // nach einem Hänger nicht springen
            // Kritisch gedämpfte Feder: Die Geschwindigkeit ändert sich stetig, auch wenn neue Rasten dazukommen –
            // dadurch eine durchgehende Bewegung statt "Stoß, Abbremsen, Stoß" pro Raste.
            const double w = 17.0;                    // Steifigkeit (~0,25 s bis zur Ruhe)
            auto spring = [&](double& x, double& v, double target) {
                int steps = std::max(1, (int)std::ceil(dt / 0.004));  // kleine Teilschritte = stabil und weich
                double h = dt / steps;
                for (int i = 0; i < steps; i++) {
                    double a = w * w * (target - x) - 2 * w * v;
                    v += a * h;
                    x += v * h;
                }
            };
            spring(g.scrollF, g.scrollV, g.scrollTo);
            spring(g.scrollRF, g.scrollRV, g.scrollRTo);
            bool done = std::fabs(g.scrollTo - g.scrollF) < 0.5 && std::fabs(g.scrollV) < 8 &&
                        std::fabs(g.scrollRTo - g.scrollRF) < 0.5 && std::fabs(g.scrollRV) < 8;
            if (done) { g.scrollF = g.scrollTo; g.scrollRF = g.scrollRTo; g.scrollV = g.scrollRV = 0; }
            g.scroll = (int)std::lround(g.scrollF);
            g.scrollR = (int)std::lround(g.scrollRF);
            ClampScroll();  // Feder darf am Rand nicht übers Ende hinausschwingen
            Layout();
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            g.hot = HitTest(pt);
            Render();
            if (done) {
                KillTimer(hwnd, T_SCROLL);
                g.scrollAnim = false;
                if (g.renderFrames)
                    Log(L"Scrollen: " + std::to_wstring(g.renderFrames) + L" Bilder in " +
                        std::to_wstring((int)((now.QuadPart - sScrollStart.QuadPart) * 1000 / fq.QuadPart)) + L" ms, Ø " +
                        std::to_wstring((int)(g.renderMsSum / g.renderFrames * 10) / 10.0).substr(0, 5) + L" ms pro Bild | Szene " + std::to_wstring((int)(g.stageMs[0] / g.renderFrames)) + L" Compose " + std::to_wstring((int)(g.stageMs[1] / g.renderFrames)) + L" Symbole " + std::to_wstring((int)(g.stageMs[2] / g.renderFrames)) + L" Text " + std::to_wstring((int)(g.stageMs[3] / g.renderFrames)) + L" Fenster " + std::to_wstring((int)(g.stageMs[4] / g.renderFrames)));
            }
        } else if (wp == T_PLAYER) {
            if (g.open && GetTickCount64() - g.statusAt > 10000) {  // WLAN/Akku alle 10 s
                int n = g.net, nb = g.netBars, b = g.batt;
                bool c = g.battCharging, inet = g.netInternet;
                RefreshStatus();
                if (n != g.net || nb != g.netBars || b != g.batt || c != g.battCharging || inet != g.netInternet) Refresh();
            }
            // Fortschritt des eigenen Players
            PlayerState ps = Player_State();
            if (ps.active && ps.songId != g.lastRecentId) {  // neuer Song -> "Zuletzt gespielt"
                g.lastRecentId = ps.songId;
                AddRecentSong(ps.songId);
            }
            bool changed = ps.active != g.pst.active || ps.playing != g.pst.playing || ps.songId != g.pst.songId ||
                           (int)ps.pos != (int)g.pst.pos || (int)ps.dur != (int)g.pst.dur ||
                           (g.view == View::NowPlaying && LyricIndex(ps.pos) != LyricIndex(g.pst.pos));
            g.pst = ps;
            if (changed && g.open && !g.closing && !g.animating && !g.dragging) {
                Layout();
                Render();
            }
        } else if (wp == T_COUNT) {
            if (g.timerEnd && !TimerRemaining()) {  // abgelaufen: klingeln und die Leiste mit dem Timer zeigen
                g.timerEnd = 0;
                PlayerState ps = Player_State();
                if (ps.active && ps.playing) Player_Toggle();
                StartRinging();
                if (g.open && !g.closing) ShowView(View::Timer);
                else if (!g.open && !g.animating) {
                    RECT wa;
                    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
                    OpenBar({wa.right - 2, (wa.top + wa.bottom) / 2});
                }
            } else if (g.timerRinging && GetTickCount64() - g.ringSince > 120000) {  // nach 2 Minuten von selbst still
                StopRinging();
                if (g.open && !g.closing && g.view == View::Timer) Refresh();
            } else if (g.open && !g.closing && !g.animating && g.view == View::Timer && !g.timerRinging) {
                Render();
            }
        } else if (wp == T_RELOAD) {
            g.reloadTick++;
            if (g.reload == 1 && g.reloadTick % 5 == 0) {  // alle 0,5 s: kommt noch etwas an?
                bool busy = false;
                unsigned long long snap = Music_FolderState(busy);
                g.reloadStable = !busy && snap == g.reloadSnap ? g.reloadStable + 1 : 0;
                g.reloadSnap = snap;
                if (g.reloadStable >= 4) {  // 2 s Ruhe: einlesen
                    Music_ImportFolder();
                    Music_ImportEdits();
                    BuildMusic();
                    g.reload = 2;
                    g.reloadTick = 0;
                    Layout();
                }
            } else if (g.reload == 2 && g.reloadTick >= 15) {  // voller Balken bleibt 1,5 s stehen
                g.reload = 0;
                KillTimer(hwnd, T_RELOAD);
            }
            if (g.open && !g.closing && !g.animating && !g.dragging) Render();
        } else if (wp == T_PIDLE) {
            PlayerState ps = Player_State();
            if (!ps.active || g.open) { KillTimer(hwnd, T_PIDLE); g.pausedMinutes = 0; }
            else if (ps.playing) g.pausedMinutes = 0;
            else if (++g.pausedMinutes >= 10) {
                Player_Stop();
                KillTimer(hwnd, T_PIDLE);
                TrimMemory();
            }
        } else if (wp == T_RUN) {  // laufende Apps in der Liste auffrischen
            if (!g.open || g.closing || g.view != View::AllApps) { KillTimer(hwnd, T_RUN); return 0; }
            if (RefreshRunning()) {
                if (g.onlyRunning) SortAppSizes();
                Refresh();
            }
        } else if (wp == T_CLOUDWAIT) {  // wartet auf die Anmeldung im Cloud-Programm
            CloudWaitTick();
        } else if (wp == T_CHATFREE) {  // 10 s nach dem Schließen nicht wieder geöffnet: Claude-Webansicht beenden (gibt den Speicher frei)
            KillTimer(hwnd, T_CHATFREE);
            if (!g.open) { Chat_Free(); TrimMemory(); }
        } else if (wp == T_LYRBACK) {  // 2 s nicht mehr im Songtext gedreht: zurück zur aktuellen Zeile
            KillTimer(hwnd, T_LYRBACK);
            g.lyrManual = false;
            if (g.open && !g.closing && g.view == View::NowPlaying) Render();
        } else if (wp == T_NPDROP) {  // "Läuft gerade" nach unten wegschieben und ausblenden, dann zurück
            double end = g.h * 0.45;
            g.npDrop += std::max((double)S(7), g.npDrop * 0.22);
            g.npAlpha = (float)std::max(0.0, 1.0 - g.npDrop / end);
            bool done = g.npDrop >= end || !g.open || g.closing || g.view != View::NowPlaying;
            if (done) {
                KillTimer(hwnd, T_NPDROP);
                g.npDrop = 0;
                g.npAlpha = 1.f;
                g.npDropping = false;
                if (g.open && !g.closing && g.view == View::NowPlaying) ShowView(View::Music);  // eingeklappt -> Bibliothek
            } else if (!g.animating) {
                Layout();
                Render();
            }
        } else if (wp == T_LYR) {  // Songtext weich mitscrollen
            if (!g.open || g.closing || g.view != View::NowPlaying) { KillTimer(hwnd, T_LYR); g.lyrAnim = false; return 0; }
            double d = g.lyrTarget - g.lyrScroll;
            if (std::fabs(d) < 0.5) { g.lyrScroll = g.lyrTarget; KillTimer(hwnd, T_LYR); g.lyrAnim = false; }
            else g.lyrScroll += d * 0.14;
            if (!g.animating && !g.dragging) Render();
        } else if (wp == T_VOL) {  // Lautstärkeregler weich aus-/einklappen
            if (g.volLeaveAt && GetTickCount64() >= g.volLeaveAt) { g.volLeaveAt = 0; g.volTarget = 0; }
            double d = g.volTarget - g.volExp;
            g.volExp = std::fabs(d) < 0.01 ? g.volTarget : g.volExp + d * 0.28;
            if (g.open && !g.closing) {
                Layout();
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                g.hot = HitTest(pt);
                Render();
            }
            if (g.volExp == g.volTarget && !g.volLeaveAt) KillTimer(hwnd, T_VOL);
        } else if (wp == T_GAMES) {
            Games_Tick();
        } else if (wp == T_DUCK) {
            PlayerState ps = Player_State();
            if (!ps.active) {
                KillTimer(hwnd, T_DUCK);
                sDuck = 1.0;
                sDucked = false;
                Player_SetDuck(1.0);
                return 0;
            }
            if (!ps.playing) return 0;
            if (Music_OtherMediaPlaying()) { sDucked = true; sDuckQuiet = 0; }
            else if (sDucked && ++sDuckQuiet >= 5) sDucked = false;  // 1,5 s Ruhe, dann wieder lauter
            double target = sDucked ? 0.1 : 1.0;
            sDuck = target < sDuck ? std::max(target, sDuck - 0.27) : std::min(target, sDuck + 0.1);  // schnell leiser, sanft lauter
            Player_SetDuck(sDuck);
        } else if (wp == T_GRAB) {
            bool changed = GetClipboardSequenceNumber() != sClipSeq;
            if (sGrabStage != 1 && !changed && ++sGrabTicks < (sGrabStage == 2 ? 12 : 8)) return 0;  // kurz auf die Kopie warten (nichts markiert -> Adressleiste)
            KillTimer(hwnd, T_GRAB);
            std::wstring t;
            bool got = changed && ClipText(t);
            if (changed) {  // Zwischenablage wiederherstellen
                if (sClipHad) SetClipText(sClipOld);
                else if (OpenClipboard(nullptr)) { EmptyClipboard(); CloseClipboard(); }
            }
            sClipSeq = GetClipboardSequenceNumber();
            sClipOld.clear();
            // 0: markierter Link; 1: Adresse der offenen Seite (Adressleiste lesen); 2: Strg+L, Strg+C (Notweg)
            t = got ? AsLink(t) : L"";
            if (sGrabStage == 2) SendKeys({{VK_ESCAPE, false}, {VK_ESCAPE, true}});  // Adressleiste wieder verlassen
            if (t.empty() && sGrabStage < 2) t = AsLink(BrowserUrl(sGrabFg));
            if (t.empty() && sGrabStage < 2 && IsBrowserWindow(sGrabFg)) {
                if (sGrabStage == 0) { sGrabStage = 1; sGrabRetry = 0; }
                if (++sGrabRetry <= 3) {  // Chrome-Browser brauchen oft einen Moment, bis die Adressleiste lesbar ist
                    Log(L"Alt+Ende: Adressleiste noch nicht lesbar, versuche es nochmal");
                    SetTimer(hwnd, T_GRAB, 250, nullptr);
                    return 0;
                }
                Log(L"Alt+Ende: Notweg Strg+L, Strg+C");
                sGrabStage = 2;
                sGrabTicks = 1;
                sClipHad = ClipText(sClipOld);
                sClipSeq = GetClipboardSequenceNumber();
                SendKeys({{VK_CONTROL, false}, {'L', false}, {'L', true}, {'C', false}, {'C', true}, {VK_CONTROL, true}});
                SetTimer(hwnd, T_GRAB, 60, nullptr);
                return 0;
            }
            sGrabStage = 0;
            sGrabRetry = 0;
            sGrabTicks = 0;
            bool link = !t.empty();
            if (link && Music_ToolAvailable()) {
                Music_Init(hwnd);
                Music_AddUrl(t);
                Log((L"Alt+Ende: lade " + t).c_str());
                MessageBeep(MB_OK);
                if (g.open) { BuildMusic(); Render(); }
            } else if (link) {  // Link gefunden, aber yt-dlp fehlt: nachfragen und danach laden
                Log((L"Alt+Ende: Link " + t + L", aber yt-dlp fehlt").c_str());
                QueueLink(t);
            } else {
                Log(L"Alt+Ende: kein Link markiert");
                MessageBeep(MB_ICONHAND);
            }
        } else if (wp == T_ENDHOLD) {
            if (GetAsyncKeyState(g.hotVk) & 0x8000) return 0;  // noch gehalten
            KillTimer(hwnd, T_ENDHOLD);
            sHolding = false;
            for (int id = 2; id <= 9; id++)
                if (id != 5) UnregisterHotKey(hwnd, id);  // 5 = Alt+Ende (Link laden) bleibt
            if (sHoldPassed) {
                if (g.hotVk && !g.hotCapture) RegisterHotKey(hwnd, 1, g.hotMods | MOD_NOREPEAT, g.hotVk);
            } else if (!sHoldCombo && !g.animating) {
                if (g.open) CloseBar(L"Taste");
                else {
                    POINT pt;
                    GetCursorPos(&pt);
                    Log(L"Taste gedrückt -> öffnen");
                    OpenBar(pt);
                }
            }
        } else if (wp == T_HOTKEY) {
            KillTimer(hwnd, T_HOTKEY);
            static int tries = 0;
            if (g.hotVk && !g.hotCapture && !sHolding && !RegisterHotKey(hwnd, 1, g.hotMods | MOD_NOREPEAT, g.hotVk) &&
                GetLastError() == ERROR_HOTKEY_ALREADY_REGISTERED && ++tries < 20)
                SetTimer(hwnd, T_HOTKEY, 300, nullptr);  // Wächter gibt die Taste gleich frei
            else
                tries = 0;
        } else if (wp == T_TIP) {
            KillTimer(hwnd, T_TIP);
            if (g.open && !g.closing && g.hot >= 0) { g.tipShown = true; Render(); }
        } else if (wp == T_REDRAW) {
            Render();
        } else if (wp == T_FREE) {
            KillTimer(hwnd, T_FREE);
            if (!g.open) TrimMemory();  // nach dem Ende des Musik-Threads noch einmal aufräumen
        }
        return 0;

    case WM_APP_LIST: {
        auto* m = (ListMsg*)lp;
        if (m->gen != g_gen) {
            for (auto& a : *m->apps) {
                if (a.icon) DeleteObject(a.icon);
                if (a.iconSmall) DeleteObject(a.iconSmall);
            }
        } else {
            g.lastLoad = GetTickCount64();
            for (auto& a : g.apps) {
                if (a.icon) DeleteObject(a.icon);
                if (a.iconSmall) DeleteObject(a.iconSmall);
            }
            g.apps.swap(*m->apps);
            g.loaded = true;
            BuildFolders();
            UpdateSearch();
            if (g.open) { Layout(); ClampScroll(); Layout(); Redraw(); }
        }
        delete m->apps;
        delete m;
        return 0;
    }
    case WM_APP_ICON: {
        auto* m = (IconMsg*)lp;
        if (m->gen == g_gen && m->idx < (int)g.apps.size() && !g.apps[m->idx].icon && !g.apps[m->idx].iconSmall) {
            g.apps[m->idx].icon = m->bmp;
            g.apps[m->idx].iconSmall = m->smallBmp;
            if (g.open) Redraw();
        } else {
            if (m->bmp) DeleteObject(m->bmp);
            if (m->smallBmp) DeleteObject(m->smallBmp);
        }
        delete m;
        return 0;
    }
    case WM_APP_SHOW: {
        if (g.animating) return 0;
        POINT pt;
        GetCursorPos(&pt);
        if (g.open) CloseBar(L"--show");
        else {
            OpenBar(pt);
            if (wp == 10 && g.open) ShowView(View::Music);   // --musicview
            else if (wp == 11 && g.open) OpenSongEdit(0);   // --songedit (Test)
            else if (wp == 13 && g.open) ShowView(View::AllApps);  // --allapps (Test)
            else if (wp == 14 && g.open) GoToPage(2);              // --calendar (Test)
            else if (wp == 15 && g.open) GoToPage(3);              // --storage (Test)
            else if (wp == 16 && g.open) GoToPage(4);              // --chat (Test)
            else if (wp == 17 && g.open) { g.npReturn = View::Music; ShowView(View::NowPlaying); }  // --nowplaying (Test)
            else if (wp == 12 && g.open) {                  // --games (Test): Spiele-Ordner öffnen
                for (int k = 0; k < (int)g.folders.size(); k++)
                    if (g.folders[k].name == L"Spiele") ShowView(View::Folder, k);
            }
            else if (wp >= 1 && g.open) OpenSettings((int)wp - 1);  // --settings / --folders / --musicsettings
        }
        return 0;
    }
    case WM_APP_MEDIA: {
        auto* mi = (MediaInfo*)lp;
        if (mi->artChanged) {
            if (g.mediaArt) DeleteObject(g.mediaArt);
            g.mediaArt = g.open ? mi->art : nullptr;
            if (!g.open && mi->art) DeleteObject(mi->art);
        } else if (mi->art) {
            DeleteObject(mi->art);
        }
        mi->art = nullptr;
        g.media = *mi;
        delete mi;
        if (g.open && !g.closing) Render();
        return 0;
    }
    case WM_APP_PHONES: {
        auto* pl = (PhoneList*)lp;
        g.mediaNote.clear();
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Musik vom Handy über den PC abspielen:");
        if (pl->devices.empty())
            AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Kein passendes Handy gefunden – zuerst per Bluetooth koppeln");
        for (int i = 0; i < (int)pl->devices.size() && i < 20; i++)
            AppendMenuW(m, MF_STRING | (pl->devices[i].id == pl->connectedId ? MF_CHECKED : 0), 100 + i, pl->devices[i].name.c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        if (!pl->connectedId.empty()) AppendMenuW(m, MF_STRING, 1, L"Handy trennen");
        AppendMenuW(m, MF_STRING, 2, L"Neues Handy koppeln …");
        int cmd = g.open ? TrackMenu(m) : (DestroyMenu(m), 0);
        if (cmd >= 100 && cmd < 100 + (int)pl->devices.size()) {
            auto& d = pl->devices[cmd - 100];
            if (d.id != pl->connectedId) {
                g.mediaNote = L"Verbinde mit " + d.name + L" …";
                g.noteUntil = GetTickCount64() + 15000;
                Media_ConnectPhone(d.id, d.name);
            }
        } else if (cmd == 1) {
            Media_Command(MC_DISCONNECT);
        } else if (cmd == 2) {
            ShellExecuteW(nullptr, L"open", L"ms-settings-connectabledevices:devicediscovery", nullptr, nullptr, SW_SHOWNORMAL);
        }
        delete pl;
        if (g.open) Render();
        return 0;
    }
    case WM_APP_PHONESTATE: {
        auto* ps = (PhoneState*)lp;
        g.phoneConnected = ps->connected;
        g.phoneName = ps->name;
        g.mediaNote = !ps->error.empty() ? ps->error
                    : ps->connected ? L"Verbunden – starte jetzt die Musik am Handy"
                                    : L"Handy getrennt";
        g.noteUntil = GetTickCount64() + 6000;
        delete ps;
        if (g.open && !g.closing) Render();
        return 0;
    }
    case WM_HOTKEY:
        if (wp >= 6 && wp <= 9) {  // Ende + "+" / "-": Lautstärke in 5er-Schritten
            sHoldCombo = true;
            bool up = wp <= 7;
            if (!Player_State().active && SpotifyMode()) {
                SendMediaKey(up ? VK_VOLUME_UP : VK_VOLUME_DOWN);  // Spotify: Windows-Lautstärke
            } else {
                g.volume = std::max(0, std::min(100, g.volume + (up ? 5 : -5)));
                if (g.volume > 0) g.volumeBeforeMute = g.volume;
                Player_SetVolume(VolumeGain(g.volume));
                IniSet(L"Einstellungen", L"Lautstaerke", std::to_wstring(g.volume).c_str());
                Log(L"Lautstärke: " + std::to_wstring(g.volume) + L" %");
            }
            if (g.open && !g.closing) Render();
            return 0;
        }
        if (wp >= 2 && wp <= 4) {
            sHoldCombo = true;
            bool own = Player_State().active;
            if (!own && SpotifyMode()) {  // Spotify steuern (auch bei geschlossener Leiste)
                Media_Command(wp == 2 ? MC_PLAYPAUSE : wp == 3 ? MC_NEXT : MC_PREV);
                return 0;
            }
            // nur der eigene Musik-Player, keine Videos/Browser/andere Medien
            if (wp == 2) {
                if (own) Player_Toggle();
                else {  // noch nichts geladen: alle Songs abspielen
                    std::vector<std::wstring> ids;
                    for (const auto& s : Music_Songs()) ids.push_back(s.id);
                    if (!ids.empty()) Player_Play(ids, 0);
                }
                StartDuckWatch(hwnd);
            }
            if (wp == 3 && own) Player_Next();
            if (wp == 4 && own) Player_Prev();
            if (g.open) { g.pst = Player_State(); Render(); }
            return 0;
        }
        if (wp == 5) {
            if (sGrabTicks > 0) return 0;
            sGrabFg = GetForegroundWindow();
            sGrabStage = 0;
            sGrabRetry = 0;
            Log(IsBrowserWindow(sGrabFg) ? L"Alt+Ende gedrückt (Browser vorne)" : L"Alt+Ende gedrückt (kein bekannter Browser vorne)");
            // Auswahl im vorderen Programm kopieren: Strg drücken, Alt loslassen (kein Menü), C, Strg los
            sClipHad = ClipText(sClipOld);
            sClipSeq = GetClipboardSequenceNumber();
            std::vector<INPUT> in;
            auto key = [&](WORD vk, bool up) {
                INPUT i = {};
                i.type = INPUT_KEYBOARD;
                i.ki.wVk = vk;
                i.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
                in.push_back(i);
            };
            key(VK_CONTROL, false);
            if (g.grabMods & MOD_ALT) key(VK_MENU, true);
            if (g.grabMods & MOD_SHIFT) key(VK_SHIFT, true);
            if (g.grabMods & MOD_WIN) key(VK_LWIN, true);
            key('C', false);
            key('C', true);
            key(VK_CONTROL, true);
            SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
            sGrabTicks = 1;
            SetTimer(hwnd, T_GRAB, 50, nullptr);
            return 0;
        }
        if (wp != 1 || sHolding) return 0;
        // Taste gehalten: solange Pos1/Bild auf/Bild ab abfangen, beim Loslassen entscheiden
        sHolding = true;
        sHoldCombo = false;
        sHoldPassed = false;
        if (g.musicKeys && g.musicOn) {
            for (int i = 0; i < 3; i++) RegisterHotKey(hwnd, 2 + i, MOD_NOREPEAT, g.ctlKey[i]);  // Play/Pause, weiter, zurück
            RegisterHotKey(hwnd, 6, 0, g.ctlKey[3]);  // lauter (mit Wiederholung)
            RegisterHotKey(hwnd, 8, 0, g.ctlKey[4]);  // leiser
            if (g.ctlKey[3] == VK_OEM_PLUS) RegisterHotKey(hwnd, 7, 0, VK_ADD);         // Ziffernblock zusätzlich
            if (g.ctlKey[4] == VK_OEM_MINUS) RegisterHotKey(hwnd, 9, 0, VK_SUBTRACT);
        }
        // Vollbild-Spiel, oder man tippt gerade im Claude-Chat: Ende-Taste normal weiterreichen
        if ((!g.open && FullscreenActive()) || (g.open && Chat_IsOwnWindow(GetForegroundWindow()))) {
            // Spiel/Video im Vollbild: Taste nicht schlucken, sondern ans Spiel weiterreichen (Loslassen kommt dort von selbst an)
            sHoldPassed = true;
            UnregisterHotKey(hwnd, 1);
            INPUT in = {};
            in.type = INPUT_KEYBOARD;
            in.ki.wVk = (WORD)g.hotVk;
            in.ki.dwFlags = KEYEVENTF_EXTENDEDKEY * (g.hotVk == VK_END || g.hotVk == VK_NEXT || g.hotVk == VK_PRIOR || g.hotVk == VK_HOME || g.hotVk == VK_INSERT || g.hotVk == VK_DELETE);
            SendInput(1, &in, sizeof(INPUT));
        }
        SetTimer(hwnd, T_ENDHOLD, 30, nullptr);
        return 0;
    case WM_APP_MUSIC:
        if (wp == MUSIC_PLAYED) {
            auto* id = (std::wstring*)lp;
            int i = Music_Find(*id);
            delete id;
            if (i >= 0) {
                Music_Songs()[i].plays++;
                Music_Save();
                // Reihenfolge erst beim nächsten Öffnen neu: Zeilen sollen nicht unter der Maus wegspringen
            }
        } else if (wp == MUSIC_SONG) {
            auto* s = (Song*)lp;
            int i = Music_Find(s->id);
            if (i >= 0) { s->plays = Music_Songs()[i].plays; Music_Songs()[i] = *s; }
            else Music_Songs().push_back(*s);
            delete s;
            Music_Save();
            BuildMusic();
            if (g.open && !g.closing) Refresh();
        } else if (wp == MUSIC_SYNC) {  // Musik-Sync: Songs sind verschoben -> neue Pfade übernehmen, Bibliotheksdatei schreiben
            auto* r = (SyncResult*)lp;
            for (auto& mv : r->moved) {
                int i = Music_Find(mv.first);
                if (i >= 0) Music_Songs()[i].file = mv.second;
            }
            Music_Save();  // schreibt auch HoverBar-Bibliothek.json
            Music_ExportLibrary();
            BuildMusic();
            delete r;
            if (g.open && !g.closing) Refresh();
            if (g.syncShowQr) {  // Einrichtung fertig: QR-Code für die Handy-App zeigen
                g.syncShowQr = false;
                ShowPhoneQr();
            }
        } else if (wp == MUSIC_JOBS) {
            if (g.open && !g.closing && g.view == View::Settings) Render();  // Fortschritt der Musik-Sync-Karte
            if (g.open && !g.closing && g.view == View::Music) Refresh();
        } else if (wp == MUSIC_TOOL) {  // yt-dlp: neuer Stand (Fortschritt, fertig, Fehler)
            if (!g.pendingUrl.empty()) {  // ein Link wartet auf den Download
                ToolStatus ts = Music_ToolStatus();
                if (ts.state == TOOL_DONE || ts.state == TOOL_ERROR || ts.state == TOOL_IDLE) {
                    std::wstring u = g.pendingUrl;
                    g.pendingUrl.clear();
                    if (ts.state == TOOL_DONE && Music_ToolAvailable()) {
                        Music_Init(g.hwnd);
                        Music_AddUrl(u);
                        Log((L"yt-dlp geladen, lade jetzt " + u).c_str());
                        MessageBeep(MB_OK);
                    } else {
                        Log(L"yt-dlp konnte nicht geladen werden");
                        MessageBeep(MB_ICONHAND);
                    }
                }
            }
            if (g.open && !g.closing && (g.view == View::Settings || g.view == View::Music)) Render();
        } else if (wp == MUSIC_WAVE) {
            auto* r = (WaveResult*)lp;
            if (r->songId == g.editId && g.view == View::SongEdit) {
                float mx = 0.01f;
                for (float v : r->peaks) mx = std::max(mx, v);
                g.wave = r->peaks;
                for (float& v : g.wave) v = std::pow(v / mx, 1.6f);  // strecken, leise Stellen deutlicher absetzen
                if (g.open && !g.closing) Render();
            }
            delete r;
        } else if (wp == MUSIC_LYRICS) {
            auto* r = (LyricsResult*)lp;
            if (r->songId == g.lyrId) {
                g.lyr = r->lines;
                g.lyrSynced = r->synced;
                g.lyrH.clear();
                g.lyrState = 2;
                if (g.open && !g.closing && g.view == View::NowPlaying) Render();
            }
            delete r;
        } else if (wp == MUSIC_COVERS) {
            auto* r = (CoverResults*)lp;
            if (r->songId == g.editId && g.view == View::SongEdit) {
                g.coverResults = r->files;
                g.coverSearching = false;
                g.coverNone = r->files.empty();
                if (g.open && !g.closing) Refresh();
            }
            delete r;
        }
        return 0;
    case WM_COPYDATA: {  // Befehl von "HoverBar.exe --songordner <Pfad>"
        auto* cd = (COPYDATASTRUCT*)lp;
        if (cd && cd->dwData == 1 && cd->cbData >= sizeof(wchar_t)) {
            std::wstring p((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t) - 1);
            SetSongFolder(p, true);
            if (g.open && !g.closing) Refresh();
        } else if (cd && cd->dwData == 2 && cd->cbData >= sizeof(wchar_t)) {  // "--musiksync <Ordner>": Musik-Sync ohne Rückfragen einrichten (Tests)
            std::wstring p((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t) - 1);
            Music_Init(hwnd);
            if (SetSongFolder(p, false) >= 0) {
                g.syncOn = true;
                g.syncDir = p;
                IniSet(L"Erweiterungen", L"MusikSync", L"1");
                IniSet(L"Erweiterungen", L"MusikSyncOrdner", p.c_str());
                Music_SetFavorites(g.favIds);
                Music_SetSyncExport(true);
                if (Player_State().active) { Player_Stop(); g.pst = PlayerState(); }
                Music_SyncSetup(hwnd, p);
            }
        } else if (cd && cd->dwData == 3) {  // "--musiksync-aus"
            g.syncOn = false;
            IniSet(L"Erweiterungen", L"MusikSync", L"0");
            Music_SetSyncExport(false);
        }
        return TRUE;
    }
    case WM_APP_CLOUD:  // Cloud-Programm: Installation beendet / Fortschritt
        {
            int cs;
            {
                std::lock_guard<std::mutex> lk(sCloudMu);
                cs = sCloudState;
            }
            if (cs == 2 && !sCloudName.empty() && sCloudWait.empty()) {  // installiert: starten, auf die Anmeldung warten, dann automatisch weiter
                bool started = LaunchCloudApp(CloudLaunchKeyword(sCloudName));
                Log(std::wstring(L"Cloud-Programm gestartet: ") + sCloudName + (started ? L"" : L" (Verknüpfung nicht gefunden)"));
                CloudSet(2, sCloudName + (started ? L" ist installiert und gestartet. " : L" ist installiert. Starte es bitte selbst. ") +
                                L"Melde dich dort an – HoverBar macht danach automatisch mit dem Einrichten weiter.");
                CloudWaitStart(sCloudName);
            }
        }
        if (g.open && !g.closing && g.view == View::Settings) Render();
        return 0;
    case WM_APP_UPDATE:  // HoverBar-Update: neuer Stand
        if (g.open && !g.closing && g.view == View::Settings) Render();
        return 0;
    case WM_APP_PLUGIN:  // Erweiterung: Fortschritt / fertig
        if (g.open && !g.closing) Refresh();
        return 0;
    case WM_APP_FAVICON:  // Website-Symbol geladen: Bildzwischenspeicher für Symbole leeren und neu zeichnen
        for (auto it = g.coverCache.begin(); it != g.coverCache.end();)
            if (it->first.find(L"\\favicons\\") != std::wstring::npos) {
                if (it->second) DeleteObject(it->second);
                it = g.coverCache.erase(it);
            } else ++it;
        if (g.open && !g.closing) Render();
        return 0;
    case WM_APP_GAMES:  // Ordnergröße eines Spiels fertig berechnet
        if (g.open && !g.closing && g.view == View::Folder) {
            LoadGameStats();
            Render();
        }
        if (g.open && !g.closing && g.view == View::AllApps) {
            for (auto& e : g.appSizes)  // nur fehlende Größen nachtragen, Reihenfolge bleibt
                if (e.second < 0) e.second = Apps_Size({g.apps[e.first].id, g.apps[e.first].file, g.apps[e.first].name}, g.apps[e.first].target);
            g.appMax = 1;
            g.appTotal = 0;
            for (auto& e : g.appSizes)
                if (e.second > 0) { g.appMax = std::max(g.appMax, e.second); g.appTotal += e.second; }
            Render();
        }
        return 0;
    case WM_APP_DONE:
        if ((int)wp == g_gen) g.loading = false;
        if (!g.open) TrimMemory();
        return 0;

    case WM_SYSKEYDOWN:
    case WM_KEYDOWN:
        if (g.ctlCapture >= 0) {
            if (wp == VK_SHIFT || wp == VK_CONTROL || wp == VK_MENU || wp == VK_LWIN || wp == VK_RWIN) return 0;
            if (wp != VK_ESCAPE) {  // Esc = alte Taste behalten
                if ((UINT)wp == g.hotVk) { MessageBeep(MB_ICONHAND); return 0; }
                for (int i = 0; i < 5; i++)  // schon vergeben: die beiden tauschen
                    if (i != g.ctlCapture && g.ctlKey[i] == (UINT)wp) g.ctlKey[i] = g.ctlKey[g.ctlCapture];
                g.ctlKey[g.ctlCapture] = (UINT)wp;
                for (int i = 0; i < 5; i++)
                    IniSet(L"Einstellungen", L"Steuerung" + std::to_wstring(i), std::to_wstring(g.ctlKey[i]).c_str());
            }
            g.ctlCapture = -1;
            Render();
            return 0;
        }
        if (g.grabCapture) {
            if (wp == VK_SHIFT || wp == VK_CONTROL || wp == VK_MENU || wp == VK_LWIN || wp == VK_RWIN) return 0;
            if (wp != VK_ESCAPE) {  // Esc = alte Taste behalten
                UINT mods = ((GetKeyState(VK_CONTROL) & 0x8000) ? MOD_CONTROL : 0) | ((GetKeyState(VK_MENU) & 0x8000) ? MOD_ALT : 0) |
                            ((GetKeyState(VK_SHIFT) & 0x8000) ? MOD_SHIFT : 0) |
                            (((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) ? MOD_WIN : 0);
                // eine nackte Buchstabentaste würde das Tippen überall blockieren
                if (!(mods & (MOD_CONTROL | MOD_ALT | MOD_WIN)) && (wp < VK_F1 || wp > VK_F24)) { MessageBeep(MB_ICONHAND); return 0; }
                UINT oldVk = g.grabVk, oldMods = g.grabMods;
                g.grabVk = (UINT)wp;
                g.grabMods = mods;
                if (!RegisterHotKey(hwnd, 5, mods | MOD_NOREPEAT, (UINT)wp)) {
                    g.grabVk = oldVk;
                    g.grabMods = oldMods;
                    MessageBeep(MB_ICONHAND);  // belegt: Aufnahme läuft weiter
                    return 0;
                }
                UnregisterHotKey(hwnd, 5);
                IniSet(L"Einstellungen", L"LinkTasteVk", std::to_wstring(g.grabVk).c_str());
                IniSet(L"Einstellungen", L"LinkTasteMods", std::to_wstring(g.grabMods).c_str());
            }
            g.grabCapture = false;
            RegisterGrabKey();
            Render();
            return 0;
        }
        if (g.hotCapture) {
            // neue Taste übernehmen (reine Umschalt-/Strg-/Alt-/Win-Tasten abwarten)
            if (wp == VK_SHIFT || wp == VK_CONTROL || wp == VK_MENU || wp == VK_LWIN || wp == VK_RWIN) return 0;
            g.hotCapture = false;
            if (wp == VK_ESCAPE) {
                g.hotVk = 0;
                g.hotMods = 0;
            } else {
                g.hotVk = (UINT)wp;
                g.hotMods = ((GetKeyState(VK_CONTROL) & 0x8000) ? MOD_CONTROL : 0) | ((GetKeyState(VK_MENU) & 0x8000) ? MOD_ALT : 0) |
                            ((GetKeyState(VK_SHIFT) & 0x8000) ? MOD_SHIFT : 0) |
                            (((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) ? MOD_WIN : 0);
            }
            IniSet(L"Einstellungen", L"Taste", std::to_wstring(g.hotVk).c_str());
            IniSet(L"Einstellungen", L"TasteMods", std::to_wstring(g.hotMods).c_str());
            if (g.hotVk && !RegisterHotKey(hwnd, 1, g.hotMods | MOD_NOREPEAT, g.hotVk)) {
                g.menuOpen = true;
                MessageBoxW(hwnd, L"Diese Taste ist schon von einem anderen Programm belegt. Bitte eine andere wählen.", L"HoverBar",
                            MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
                g.menuOpen = false;
            }
            Render();
            return 0;
        }
        if (msg == WM_SYSKEYDOWN) break;
        if (TypingAllowed() || wp == VK_ESCAPE || wp == VK_UP || wp == VK_DOWN || wp == VK_LEFT || wp == VK_RIGHT || wp == VK_RETURN ||
            wp == VK_APPS || wp == VK_SPACE) OnKeyDown(wp);
        return 0;
    case WM_CHAR:
        if (TypingAllowed() && wp >= 32 && wp != 127 && !(GetKeyState(VK_CONTROL) & 0x8000 && !(GetKeyState(VK_MENU) & 0x8000))) {
            InsertText(std::wstring(1, (wchar_t)wp));
            OnQueryChanged();
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            HT t = g.hot >= 0 && g.hot < (int)g.hits.size() ? g.hits[g.hot].type : HT::None;
            bool passive = t == HT::None || t == HT::SetCard || t == HT::SetFolderHead || t == HT::SetDrop ||
                           t == HT::SetSection || t == HT::SetRowLabel || t == HT::Pane || t == HT::PaneTitle || t == HT::MusicBar;
            bool text = t == HT::SearchR || t == HT::EditField || (t == HT::NewFolder && g.creatingFolder) || (g.textDrag);
            bool move = g.dragging || t == HT::SetAppRow || t == HT::AllAppRow || t == HT::PlSongRow || t == HT::AllSongRow || (t == HT::FavSlot && g.hits[g.hot].idx >= 0);
            SetCursor(LoadCursorW(nullptr, move ? IDC_SIZEALL : text ? IDC_IBEAM : passive ? IDC_ARROW : IDC_HAND));
            return TRUE;
        }
        break;

    case WM_MOUSEMOVE: {
        if (!g.tracking) {
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            g.tracking = true;
        }
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        g.mousePt = pt;
        if (g.sliderDrag >= 0) { ApplySlider(pt.x); Render(); return 0; }
        if (g.textDrag) {
            int c = IndexAtX(pt.x);
            if (c != g.caret) { g.caret = c; Render(); }
            return 0;
        }
        if (g.dragCand) {
            if (!g.dragging && (std::abs(pt.x - g.downPt.x) > S(5) || std::abs(pt.y - g.downPt.y) > S(5))) {
                g.dragging = true;
                g.tipShown = false;
                SetTimer(hwnd, T_DRAG, 30, nullptr);
            }
            if (g.dragging) {
                UpdateDrop();
                Render();
                return 0;
            }
        }
        int h = HitTest(pt);
        if (g.wheelDrag >= 0) {
            if (std::abs((int)pt.y - g.wheelY) > S(4)) g.wheelMoved = true;
            if (g.wheelMoved) {
                g.wheel[g.wheelDrag] = g.wheelStart - (pt.y - g.wheelY) / (double)S(44);
                Render();
            }
            return 0;
        }
        static POINT lastPt = {-1, -1};  // nur echte Mausbewegung beendet die Tastatur-Markierung
        if (pt.x == lastPt.x && pt.y == lastPt.y) return 0;
        lastPt = pt;
        g.kbdNav = false;
        if (h != g.hot) {
            g.hot = h;
            UpdateVolHover();
            g.tipShown = false;
            KillTimer(hwnd, T_TIP);
            if (h >= 0 && (IsAppHit(g.hits[h].type) || g.hits[h].type == HT::Bluetooth || g.hits[h].type == HT::MediaPhone ||
                           g.hits[h].type == HT::Wifi || g.hits[h].type == HT::Battery))
                SetTimer(hwnd, T_TIP, 350, nullptr);
            Render();
        } else if (h >= 0 && g.hits[h].type == HT::NpLyrics && g.lyrSynced) {  // Songtext: Zeile unter der Maus nachführen
            static int lastLine = -2;
            int line = -1;
            for (int k = 0; k < (int)g.lyrRects.size(); k++)
                if (PtInRect(&g.lyrRects[k].first, pt)) line = k;
            if (line != lastLine) { lastLine = line; Render(); }
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if ((HWND)lp != hwnd && g.wheelDrag >= 0) { SnapWheel(g.wheelDrag); g.wheelDrag = -1; Render(); }
        if ((HWND)lp != hwnd) g.textDrag = false;
        if (g.dragCand && (HWND)lp != hwnd) { EndDrag(); Render(); }
        if (g.sliderDrag >= 0 && (HWND)lp != hwnd) { g.sliderDrag = -1; SaveSliders(); }
        return 0;
    case WM_MOUSELEAVE:
        g.tracking = false;
        g.tipShown = false;
        KillTimer(hwnd, T_TIP);
        if (g.hot != -1) {
            g.hot = -1;
            if (g.open && !g.closing) Render();
        }
        UpdateVolHover();
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        g.pressed = HitTest(pt);
        if (g.pressed >= 0 && g.hits[g.pressed].type == HT::TimerWheel) {
            g.wheelDrag = g.hits[g.pressed].idx;
            g.wheelY = pt.y;
            g.wheelStart = g.wheel[g.wheelDrag];
            g.wheelMoved = false;
            g.pressed = -1;
            SetCapture(hwnd);
            return 0;
        }
        if (g.pressed >= 0 && g.hits[g.pressed].type == HT::EditField && g.editField != g.hits[g.pressed].idx) {
            g.editField = g.hits[g.pressed].idx;  // anderes Feld: erst aktivieren, dann Schreibmarke setzen
            Render();
        }
        if (TypingAllowed() && PtInRect(&g.activeField, pt)) {
            SyncCaret();
            g.caret = IndexAtX(pt.x);
            if (!(GetKeyState(VK_SHIFT) & 0x8000)) g.anchor = g.caret;
            g.textDrag = true;
            g.caretOn = true;
            g.pressed = -1;
            SetCapture(hwnd);
            Render();
            return 0;
        }
        if (g.pressed >= 0 && (g.hits[g.pressed].type == HT::SetSlider || g.hits[g.pressed].type == HT::BarVol)) {
            g.sliderDrag = g.hits[g.pressed].idx;
            SetCapture(hwnd);
            ApplySlider(pt.x);
            Render();
            return 0;
        }
        if (g.pressed >= 0 && g.hits[g.pressed].type == HT::FavSlot && g.hits[g.pressed].idx >= 0) {
            g.dragCand = true;
            g.dragging = false;
            g.dragApp = g.hits[g.pressed].idx;
            g.dragFromF = -1;
            g.dragFromFav = g.hits[g.pressed].pos;
            g.downPt = g.mousePt = pt;
            SetCapture(hwnd);
            return 0;
        }
        if (g.pressed >= 0 && (g.hits[g.pressed].type == HT::PlSongRow || g.hits[g.pressed].type == HT::AllSongRow)) {
            g.dragCand = true;
            g.dragging = false;
            g.dragMusic = true;
            g.dragApp = g.hits[g.pressed].idx;
            g.dragFromF = g.hits[g.pressed].f;
            g.dragFromPos = g.hits[g.pressed].pos;
            g.downPt = g.mousePt = pt;
            SetCapture(hwnd);
            return 0;
        }
        if (g.pressed >= 0 && (g.hits[g.pressed].type == HT::SetAppRow || g.hits[g.pressed].type == HT::AllAppRow)) {
            g.dragCand = true;
            g.dragging = false;
            g.dragApp = g.hits[g.pressed].idx;
            g.dragFromF = g.hits[g.pressed].f;
            g.downPt = g.mousePt = pt;
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (TypingAllowed() && PtInRect(&g.activeField, pt)) {
            SyncCaret();
            const std::wstring& t = EditTarget();
            int i = IndexAtX(pt.x);
            int a = i, b = i;
            while (a > 0 && IsWordChar(t[a - 1])) a--;
            while (b < (int)t.size() && IsWordChar(t[b])) b++;
            g.anchor = a;
            g.caret = b;
            Render();
            return 0;
        }
        return SendMessageW(hwnd, WM_LBUTTONDOWN, wp, lp);  // sonst wie ein normaler Klick
    }
    case WM_LBUTTONUP: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (g.wheelDrag >= 0) {
            int i = g.wheelDrag;
            g.wheelDrag = -1;
            ReleaseCapture();
            if (!g.wheelMoved)  // Klick auf eine Zahl darüber/darunter holt sie in die Mitte
                for (auto& h : g.hits)
                    if (h.type == HT::TimerWheel && h.idx == i) g.wheel[i] += std::lround((pt.y - (h.rc.top + h.rc.bottom) / 2) / (double)S(44));
            SnapWheel(i);
            Render();
            return 0;
        }
        if (g.textDrag) {
            g.textDrag = false;
            ReleaseCapture();
            return 0;
        }
        if (g.sliderDrag >= 0) {
            g.sliderDrag = -1;
            ReleaseCapture();
            SaveSliders();
            g.pressed = -1;
            g.hot = HitTest(pt);
            UpdateVolHover();
            Render();
            return 0;
        }
        if (g.dragCand) {
            bool wasDragging = g.dragging;
            int app = g.dragApp, toF = g.dropF, toPos = g.dropPos, fromFav = g.dragFromFav;
            bool music = g.dragMusic;
            int fromPl = g.dragFromF;
            EndDrag();
            if (music) {
                if (wasDragging && toF >= 0) PlaylistPlace(app, fromPl, -1, toF, toPos);
                else if (wasDragging && toF == -2 && fromPl >= 0) PlaylistRemove(fromPl, app);
                g.pressed = -1;
                Layout();
                ClampScroll();
                Layout();
                g.hot = HitTest(pt);
                Render();
                return 0;
            }
            if (wasDragging && toF == -3) {
                if (fromFav >= 0) {  // zwei Felder tauschen
                    int other = g.favs[toPos];
                    SetFav(toPos, app);
                    SetFav(fromFav, other);
                } else {
                    SetFav(toPos, app);
                }
            } else if (wasDragging && toF == -2) {
                if (fromFav >= 0) SetFav(fromFav, -1);
                else RemoveFromFolder(app);
            } else if (wasDragging && toF >= 0 && fromFav < 0) {
                MoveApp(app, toF, toPos);
            }
            g.pressed = -1;
            Layout();
            ClampScroll();
            Layout();
            g.hot = HitTest(pt);
            Render();
            return 0;
        }
        int h = HitTest(pt);
        if (h >= 0 && h == g.pressed) Activate(h, false);
        g.pressed = -1;
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        Activate(HitTest(pt), true);
        return 0;
    }
    case WM_APP_SCROLLGO: {
        // Weiches Scrollen: jedes Bild genau im Takt des Bildschirms (60/120/144 Hz) statt mit einem groben 15-ms-Timer.
        // Eingaben (weitere Mausrad-Schritte, Klicks) werden zwischen den Bildern abgearbeitet.
        static bool inLoop = false;
        if (inLoop) return 0;
        inLoop = true;
        while (g.scrollAnim && g.open) {
            MSG m;
            bool quit = false;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                if (m.message == WM_QUIT) { PostQuitMessage((int)m.wParam); quit = true; break; }
                if (m.message == WM_APP_SCROLLGO) continue;
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            if (quit || !g.scrollAnim) break;
            LARGE_INTEGER t0, t1, fq;
            QueryPerformanceCounter(&t0);
            SendMessageW(hwnd, WM_TIMER, T_SCROLL, 0);  // ein Bild berechnen und zeigen
            if (!g.scrollAnim) break;
            QueryPerformanceCounter(&t1);
            QueryPerformanceFrequency(&fq);
            // Bildwiederholrate des Monitors; nur auf das nächste Bild warten, wenn genug Zeit übrig ist,
            // sonst sofort weiter (sonst fiele man auf jedes zweite/dritte Monitorbild zurück)
            DWM_TIMING_INFO ti = {sizeof(ti)};
            double period = SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.qpcRefreshPeriod
                                ? (double)ti.qpcRefreshPeriod / fq.QuadPart
                                : 1.0 / 60;
            double took = (double)(t1.QuadPart - t0.QuadPart) / fq.QuadPart;
            if (took < period * 0.75) {
                if (FAILED(DwmFlush())) Sleep(1);
            }
        }
        inLoop = false;
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (pt.x < RailW()) {  // auf der Navigationsleiste: eine Seite pro Raste (kurze Sperre gegen Touchpad-Flut)
            // genau eine Raste (120) = eine Seite, ohne Sperre: schnelles Drehen geht schnell durch, nichts wird übersprungen
            ULONGLONG now = GetTickCount64();
            if (now - g.navWheelAt > 400 || (g.navAcc > 0) != (delta > 0)) g.navAcc = 0;  // neue Bewegung / Richtungswechsel
            g.navWheelAt = now;
            g.navAcc += delta;
            int steps = g.navAcc / WHEEL_DELTA;  // feine Touchpad-Schritte sammeln sich bis zu einer vollen Raste
            if (!steps) return 0;
            g.navAcc -= steps * WHEEL_DELTA;
            std::vector<int> pages = NavPages();
            int pos = (int)(std::find(pages.begin(), pages.end(), NavPage()) - pages.begin());
            int np = std::max(0, std::min((int)pages.size() - 1, pos - steps));  // runter = nächste Seite
            if (pages[np] != NavPage()) GoToPage(pages[np]);
            return 0;
        }
        if (g.view == View::Timer) {  // Ziffernrad unter der Maus: eine Raste = eine Zahl
            int hi = HitTest(pt);
            if (hi >= 0 && g.hits[hi].type == HT::TimerWheel) {
                int i = g.hits[hi].idx;
                g.wheel[i] += delta > 0 ? -1 : 1;
                SnapWheel(i);
                Render();
            }
            return 0;
        }
        if (g.view == View::NowPlaying) {
            if (g.npDropping) return 0;
            int hi = HitTest(pt);
            bool onLyrics = hi >= 0 && g.hits[hi].type == HT::NpLyrics && !g.lyr.empty();
            if (onLyrics) {  // durch den Songtext blättern; 2 s ohne Drehen -> zurück zur aktuellen Zeile
                if (!g.lyrManual) g.lyrUser = g.lyrScroll;
                g.lyrManual = true;
                g.lyrUser -= delta * S(64) / (double)WHEEL_DELTA;
                g.lyrUser = std::max(-g.lyrViewH * 0.36, std::min((double)g.lyrTotal - g.lyrViewH * 0.36, g.lyrUser));
                SetTimer(hwnd, T_LYRBACK, 2000, nullptr);
                Render();
            } else if (delta < 0) {  // auf Cover / Steuerung nach unten: "Läuft gerade" einklappen
                g.npDropping = true;
                g.npDrop = 0;
                SetTimer(hwnd, T_NPDROP, 15, nullptr);
            }
            return 0;
        }
        if (g.view == View::Folders && !g.showRecent && delta > 0 && g.scroll <= 0 && (!g.scrollAnim || g.scrollTo <= 0)) {
            ULONGLONG now = GetTickCount64();
            if (now - g.pullAt > 700) g.pullAcc = 0;  // muss zügig hintereinander kommen
            g.pullAt = now;
            g.pullAcc += delta;
            if (g.pullAcc >= 3 * WHEEL_DELTA) {  // stark hochgescrollt: einblenden und sanft hineinrutschen lassen
                g.pullAcc = 0;
                BuildRecent();
                if (!g.recent.empty()) {
                    g.showRecent = true;
                    int before = g.contentH;
                    Layout();
                    int added = g.contentH - before;
                    StopScrollAnim();
                    g.scroll = added;  // erst so tun, als stünde die neue Karte noch oben außerhalb …
                    g.scrollF = added;
                    g.scrollTo = 0;     // … und dann weich herunterrollen
                    g.scrollV = 0;
                    g.scrollAnim = true;
                    g.scrollRF = g.scrollR;
                    g.scrollRTo = g.scrollR;
                    QueryPerformanceCounter(&g.scrollT);
                    sScrollStart = g.scrollT;
                    Layout();
                    Render();
                    PostMessageW(hwnd, WM_APP_SCROLLGO, 0, 0);
                }
            }
            return 0;
        }
        int step = MulDiv(delta, S(92), WHEEL_DELTA);  // Weg pro Mausrad-Raste
        if (!g.scrollAnim) {
            g.scrollAnim = true;
            g.scrollF = g.scroll;
            g.scrollRF = g.scrollR;
            g.scrollV = g.scrollRV = 0;
            g.renderMsSum = 0;
            g.renderFrames = 0;
            for (double& s : g.stageMs) s = 0;
            QueryPerformanceCounter(&g.scrollT);
            sScrollStart = g.scrollT;
            PostMessageW(hwnd, WM_APP_SCROLLGO, 0, 0);  // Bild-für-Bild-Schleife im Takt des Monitors
        }
        if (OrdnerPage() && pt.x >= g.paneMid) g.scrollRTo -= step;
        else g.scrollTo -= step;
        g.tipShown = false;
        KillTimer(hwnd, T_TIP);
        ClampScroll();
        return 0;
    }
    case WM_ACTIVATE:
        // Klicks daneben schließen nicht; nur Alt+Tab (sonst bliebe die Leiste über dem neuen Fenster)
        if (LOWORD(wp) == WA_INACTIVE && g.open && !g.closing && !g.menuOpen && (GetAsyncKeyState(VK_MENU) & 0x8000))
            CloseBar(L"Alt+Tab");
        else if (g.open) Render();
        return 0;

    case WM_DPICHANGED:
        SetDpi(HIWORD(wp));
        if (g.open) Refresh();
        return 0;

    case WM_DESTROY:
        Media_Shutdown();
        g_gen++;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- Neustart nach Absturz
// Kein zweiter Prozess: Eine Aufgabe in der Windows-Aufgabenplanung startet HoverBar, sobald Windows einen
// HoverBar-Absturz protokolliert (Anwendungsfehler, Ereignis 1000). Wird nur neu angelegt, wenn sich der Pfad ändert.
static void EnsureCrashRestartTask() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (IniGet(L"Einstellungen", L"NeustartAufgabe") == exe) return;
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"<RegistrationInfo><Description>Startet HoverBar nach einem Absturz automatisch neu</Description></RegistrationInfo>\r\n"
        L"<Triggers><EventTrigger><Enabled>true</Enabled><Delay>PT2S</Delay><Subscription>"
        L"&lt;QueryList&gt;&lt;Query Id=\"0\" Path=\"Application\"&gt;&lt;Select Path=\"Application\"&gt;"
        L"*[System[Provider[@Name='Application Error'] and (EventID=1000)]] and *[EventData[Data[1]='HoverBar.exe']]"
        L"&lt;/Select&gt;&lt;/Query&gt;&lt;/QueryList&gt;</Subscription></EventTrigger></Triggers>\r\n"
        L"<Principals><Principal id=\"Author\"><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></Principals>\r\n"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>7</Priority></Settings>\r\n"
        L"<Actions Context=\"Author\"><Exec><Command>\"" + std::wstring(exe) + L"\"</Command></Exec></Actions>\r\n"
        L"</Task>\r\n";
    std::wstring file = g.ini.substr(0, g.ini.find_last_of(L'\\') + 1) + L"neustart_aufgabe.xml";
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr;
    WORD bom = 0xFEFF;
    WriteFile(h, &bom, 2, &wr, nullptr);
    WriteFile(h, xml.c_str(), (DWORD)(xml.size() * 2), &wr, nullptr);
    CloseHandle(h);
    std::wstring cl = L"schtasks.exe /create /f /tn \"HoverBar Neustart\" /xml \"" + file + L"\"";
    std::vector<wchar_t> b(cl.begin(), cl.end());
    b.push_back(0);
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, b.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 15000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (code == 0) IniSet(L"Einstellungen", L"NeustartAufgabe", exe);
        Log(code == 0 ? L"Neustart-Aufgabe angelegt" : L"Neustart-Aufgabe: schtasks Fehler " + std::to_wstring(code));
    }
    DeleteFileW(file.c_str());
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int) {
    if (cmd && wcsstr(cmd, L"--guard")) return 0;  // früherer Wächter-Prozess: gibt es nicht mehr
    int showMode = 0;  // 0 = nicht, 1 = normal, 2 = Design, 3 = Ordner
    if (cmd && wcsstr(cmd, L"--show")) showMode = 1;
    if (cmd && wcsstr(cmd, L"--settings")) showMode = 2;
    if (cmd && wcsstr(cmd, L"--folders")) showMode = 3;
    if (cmd && wcsstr(cmd, L"--musicsettings")) showMode = 4;
    WPARAM wp = showMode >= 2 ? showMode - 1 : 0;
    if (cmd && wcsstr(cmd, L"--musicview")) { showMode = 1; wp = 10; }
    if (cmd && wcsstr(cmd, L"--songedit")) { showMode = 1; wp = 11; }
    if (cmd && wcsstr(cmd, L"--games")) { showMode = 1; wp = 12; }
    if (cmd && wcsstr(cmd, L"--allapps")) { showMode = 1; wp = 13; }
    if (cmd && wcsstr(cmd, L"--calendar")) { showMode = 1; wp = 14; }
    if (cmd && wcsstr(cmd, L"--suggested")) { showMode = 1; wp = 4; }  // Einstellungen > Erweiterungen
    if (cmd && wcsstr(cmd, L"--storage")) { showMode = 1; wp = 15; }
    if (cmd && wcsstr(cmd, L"--chat")) { showMode = 1; wp = 16; }
    if (cmd && wcsstr(cmd, L"--nowplaying")) { showMode = 1; wp = 17; }
    if (cmd && wcsstr(cmd, L"--qrtest ")) {  // Test: QR-Beispiele als Text schreiben (werden in HoverMusic mit ZXing gegengeprüft)
        std::wstring outFile = wcsstr(cmd, L"--qrtest ") + 9;
        outFile.erase(std::remove(outFile.begin(), outFile.end(), L'"'), outFile.end());
        FILE* fp = _wfopen(outFile.c_str(), L"wb");
        if (fp) {
            std::vector<std::string> samples = {"A", "HoverBar", "https://github.com/KarosuMinoyari/HoverBar/releases#hm=eyJ2IjoxfQ",
                                                "Größe & Maß – Müsik für Handy ♪ äöüß", ""};
            // echte Einrichtungs-Links (so, wie der QR-Code sie enthält), damit HoverMusic sie gegenprüfen kann
            std::vector<std::string> setupUrls;
            struct SetupCase { const wchar_t *cloud, *pkg, *parent; };
            for (auto sc : {SetupCase{L"MEGA", L"mega.privacy.android.app", L""}, SetupCase{L"Google Drive", L"com.google.android.apps.docs", L"Meine Ablage"},
                            SetupCase{L"Cloud", L"", L"Müsik Ü \"Test\""}}) {
                g.syncCloud = sc.cloud;
                g.syncPkg = sc.pkg;
                g.syncParent = sc.parent;
                g.syncDir = L"C:\\Users\\jemand\\Ordner\\HoverBar-Musik";
                setupUrls.push_back(PhoneSetupUrl());
                samples.push_back(setupUrls.back());
            }
            for (int len : {120, 250, 330}) {
                std::string s = "Lorem ";
                for (int i = 0; (int)s.size() < len; i++) s += "ipsum" + std::to_string(i * 7919 % 1000) + " ";
                s.resize(len);
                samples.push_back(s);
            }
            for (auto& s : samples) {
                if (s.empty()) continue;
                for (int ecc = 0; ecc < 2; ecc++) {
                    QrCode q;
                    if (!Qr_Encode(s, ecc == 1, q)) continue;
                    fprintf(fp, "T:%s\nE:%c\nV:%d\n", s.c_str(), ecc ? 'M' : 'L', q.version);
                    for (int y = 0; y < q.size; y++) {
                        for (int x = 0; x < q.size; x++) fputc(q.dark(x, y) ? '#' : '.', fp);
                        fputc('\n', fp);
                    }
                    fputc('\n', fp);
                }
            }
            for (auto& u : setupUrls) fprintf(fp, "U:%s\n", u.c_str());
            fclose(fp);
        }
        return 0;
    }
    if (cmd && wcsstr(cmd, L"--cloudtest ")) {  // Test: erkannte Cloud-Dienste und ihre Startmenü-Verknüpfung auflisten (startet nichts)
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::wstring outFile = wcsstr(cmd, L"--cloudtest ") + 12;
        outFile.erase(std::remove(outFile.begin(), outFile.end(), L'"'), outFile.end());
        FILE* fp = _wfopen(outFile.c_str(), L"wb");
        if (fp) {
            for (auto& c : ListClouds()) {
                std::wstring lnk;
                LaunchCloudApp(CloudLaunchKeyword(c.name), false, &lnk);
                fprintf(fp, "%s | winget=%s | android=%s | Ordner=%s | Startmenue=%s\n", Utf8Of(c.name).c_str(), Utf8Of(c.wingetId).c_str(), Utf8Of(c.androidPkg).c_str(),
                        c.base.empty() ? "-" : Utf8Of(c.base).c_str(), lnk.empty() ? "-" : Utf8Of(lnk).c_str());
            }
            fclose(fp);
        }
        return 0;
    }
    if (cmd && wcsstr(cmd, L"--signtest ")) {  // Test: Signaturprüfung für Installer an einer Datei ausprobieren (schreibt <Datei>.txt, startet nichts)
        std::wstring file = wcsstr(cmd, L"--signtest ") + 11;
        file.erase(std::remove(file.begin(), file.end(), L'"'), file.end());
        while (!file.empty() && file.back() == L' ') file.pop_back();
        FILE* fp = _wfopen((file + L".txt").c_str(), L"wb");
        if (fp) {
            for (const wchar_t* who : {L"koofr", L"google", L"microsoft"}) {
                std::wstring found;
                bool ok = SignedBy(file, who, found);
                fprintf(fp, "erwartet=%s -> %s (Unterzeichner: %s)\n", Utf8Of(who).c_str(), ok ? "AKZEPTIERT" : "ABGELEHNT", found.empty() ? "-" : Utf8Of(found).c_str());
            }
            fclose(fp);
        }
        return 0;
    }
    if (cmd && wcsstr(cmd, L"--qrfenster")) {  // Test: QR-Fenster mit Beispieldaten zeigen (ändert nichts an der Einrichtung)
        g.inst = inst;
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        SetDpi(GetDpiForSystem());
        g.syncCloud = L"MEGA";
        g.syncPkg = L"mega.privacy.android.app";
        g.syncDir = L"C:\\Beispiel\\MEGA\\HoverBar-Musik";
        ShowPhoneQr();
        MSG m;
        while (sQrWnd && GetMessageW(&m, nullptr, 0, 0)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        return 0;
    }
    if (cmd && wcsstr(cmd, L"--warte-auf ")) {  // nach einem Update: erst warten, bis die alte Leiste beendet ist
        DWORD pid = (DWORD)_wtoi(wcsstr(cmd, L"--warte-auf ") + 12);
        HANDLE ph = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
        if (ph) { WaitForSingleObject(ph, 15000); CloseHandle(ph); }
    }
    Update_Cleanup();  // Reste der alten Version löschen
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\HoverBar.SingleInstance");
    if (cmd && wcsstr(cmd, L"--songordner ")) {  // Song-Ordner umstellen und vorhandene Songs dorthin verschieben
        std::wstring p = wcsstr(cmd, L"--songordner ") + 13;
        p.erase(std::remove(p.begin(), p.end(), L'"'), p.end());
        while (!p.empty() && (p.back() == L' ' || p.back() == L'\\')) p.pop_back();
        HWND other = FindWindowW(kClass, nullptr);
        if (other) {
            COPYDATASTRUCT cd = {1, (DWORD)((p.size() + 1) * sizeof(wchar_t)), (PVOID)p.c_str()};
            SendMessageW(other, WM_COPYDATA, 0, (LPARAM)&cd);
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    if (cmd && (wcsstr(cmd, L"--musiksync ") || wcsstr(cmd, L"--musiksync-aus"))) {  // Test: Musik-Sync der laufenden Instanz ein-/ausschalten
        bool off = wcsstr(cmd, L"--musiksync-aus") != nullptr;
        std::wstring p = off ? L"" : std::wstring(wcsstr(cmd, L"--musiksync ") + 12);
        p.erase(std::remove(p.begin(), p.end(), L'"'), p.end());
        while (!p.empty() && (p.back() == L' ' || p.back() == L'\\')) p.pop_back();
        HWND other = FindWindowW(kClass, nullptr);
        if (other) {
            COPYDATASTRUCT cd = {off ? 3u : 2u, (DWORD)((p.size() + 1) * sizeof(wchar_t)), (PVOID)p.c_str()};
            SendMessageW(other, WM_COPYDATA, 0, (LPARAM)&cd);
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // läuft schon: --show öffnet bzw. schließt die Leiste der laufenden Instanz
        HWND other = FindWindowW(kClass, nullptr);
        if (showMode && other) {
            AllowSetForegroundWindow(ASFW_ANY);
            PostMessageW(other, WM_APP_SHOW, wp, 0);
        }
        return 0;
    }

    g.inst = inst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    InitIni();
    EnsureCrashRestartTask();

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.style = CS_DBLCLKS;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));  // App-Symbol "[<]"
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, kClass, L"HoverBar", WS_POPUP,
                                -32000, -32000, 10, 10, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    EfficiencyMode(true);
    StartLoad();  // Apps und Symbole sofort im Hintergrund vorladen
    if (showMode) PostMessageW(hwnd, WM_APP_SHOW, wp, 0);
    if (cmd && wcsstr(cmd, L"--crashtest"))  // zum Testen des Neustarts: nach 2 s absichtlich abstürzen
        CreateThread(nullptr, 0, [](LPVOID) -> DWORD { Sleep(2000); volatile int* p = nullptr; *p = 1; return 0; }, nullptr, 0, nullptr);
    if (cmd && wcsstr(cmd, L"--addurl ")) {  // zum Testen: Link direkt umwandeln
        Music_Init(hwnd);
        Music_AddUrl(wcsstr(cmd, L"--addurl ") + 9);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return 0;
}
