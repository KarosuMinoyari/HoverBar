// Spiele-Statistiken für Ordner im Spielmodus: Größe, Spielzeit, zuletzt gespielt.
// Steam liefert alles selbst (localconfig.vdf, libraryfolders.vdf), Epic die Größe (Manifeste).
// Für Epic- und andere Spiele misst HoverBar die Spielzeit selbst (einmal pro Minute laufende Prozesse prüfen).
#pragma once
#include <windows.h>
#include <string>
#include <vector>

enum { WM_APP_GAMES = WM_APP + 60 };  // Hintergrund-Größenberechnung fertig

struct GameStats {
    long long size = -1;      // Bytes, -1 = unbekannt
    int minutes = -1;         // Spielzeit, -1 = unbekannt
    long long lastPlayed = 0; // Unix-Zeit, 0 = nie/unbekannt
    bool measured = false;    // Spielzeit von HoverBar selbst gemessen (erst seit Einführung)
};

void Games_Init(HWND notify, const std::wstring& ini);
// App-Kennung (id), ggf. Desktop-Verknüpfung (file)
struct GameRef {
    std::wstring id, file, name;
};
GameStats Games_Get(const GameRef& g);
// Liste der Spiele, deren Spielzeit HoverBar selbst messen soll (Ordner im Spielmodus)
void Games_SetWatched(const std::vector<GameRef>& games);
void Games_Tick();  // einmal pro Minute aufrufen

// Quellen an/aus: 0 = Steam, 1 = Epic Games, 2 = Riot Games, 3 = andere Spiele (eigene Messung)
bool Games_SourceOn(int src);
void Games_SetSourceOn(int src, bool on);
// Bisherige Spielzeit von Hand (Minuten), wird zur gemessenen/protokollierten Zeit addiert; -1 = keine
int Games_ManualMinutes(const std::wstring& id);
void Games_SetManualMinutes(const std::wstring& id, int minutes);
bool Games_IsSteam(const std::wstring& id);

// Größe einer beliebigen App in Bytes (-1 = unbekannt bzw. wird noch gemessen; fertig -> WM_APP_GAMES)
long long Apps_Size(const GameRef& g, const std::wstring& target);
void Apps_RefreshSizes();  // alles neu einlesen bzw. neu messen ("Aktualisieren")

// Laufende Apps: woran ein Prozess erkannt wird (exe-Pfad, Spiele-Ordner oder Store-Paket)
struct AppMatcher {
    std::wstring exe, dir, uwpName, uwpHash;
    bool system = false;  // Windows-Bestandteil: nie beenden
};
struct RunProc {
    DWORD pid;
    std::wstring path;  // klein geschrieben
    SIZE_T ram;
};
AppMatcher Apps_Matcher(const GameRef& g, const std::wstring& target);
bool Apps_MatchProcess(const AppMatcher& m, const std::wstring& lowPath);
std::vector<RunProc> Apps_Processes();
bool Apps_CloseGracefully(const std::vector<DWORD>& pids, int timeoutMs);  // wie das X im Fenster
bool Apps_Kill(const std::vector<DWORD>& pids);                            // erzwingen
