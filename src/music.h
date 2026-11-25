// Musik-Bibliothek und Player für HoverBar:
// YouTube-Link -> MP3 (yt-dlp lädt die Tonspur, Windows' eingebauter MP3-Encoder wandelt um),
// Cover automatisch über Deezer/iTunes, Playlists, Wiedergabe über den Windows-MediaPlayer.
// Bibliothek und Player werden nur vom UI-Thread benutzt; Downloads laufen im Hintergrund.
#pragma once
#include <windows.h>
#include <string>
#include <vector>

enum { WM_APP_MUSIC = WM_APP + 40 };
// wParam: MUSIC_SONG (lParam = Song*, Empfänger löscht), MUSIC_JOBS (Fortschritt), MUSIC_COVERS (lParam = CoverResults*)
enum { MUSIC_SONG = 1, MUSIC_JOBS = 2, MUSIC_COVERS = 3, MUSIC_WAVE = 4, MUSIC_TOOL = 5, MUSIC_LYRICS = 6 };

// Songtexte: zuerst "<Song>.lrc" neben der MP3, sonst online bei lrclib.net (kostenlos, ohne Konto) – Treffer wird als .lrc gespeichert
struct LyricLine {
    double t = -1;  // Sekunden (-1 = ohne Zeitstempel)
    std::wstring text;
};
struct LyricsResult {  // MUSIC_LYRICS (lParam = LyricsResult*, Empfänger löscht)
    std::wstring songId;
    std::vector<LyricLine> lines;
    bool synced = false;  // Zeilen haben Zeitstempel
};
void Music_LoadLyrics(const std::wstring& id, bool online);

// yt-dlp herunterladen/aktualisieren (Status für die Einstellungen)
enum { TOOL_IDLE = 0, TOOL_CHECKING, TOOL_DOWNLOADING, TOOL_VERIFYING, TOOL_DONE, TOOL_ERROR };
struct ToolStatus {
    int state = TOOL_IDLE;
    int percent = -1;
    std::wstring local, latest, msg;  // installierte / neueste Version, Meldung
};
ToolStatus Music_ToolStatus();
void Music_ToolUpdate(HWND notify);      // neueste Version suchen, herunterladen, prüfen, ersetzen
void Music_ToolCheckLocal(HWND notify);  // nur die installierte Version ermitteln

struct WaveResult {  // MUSIC_WAVE: Lautstärke-Spitzen (0..1) gleichmäßig über den Song
    std::wstring songId;
    std::vector<float> peaks;
};
void Music_LoadWave(const std::wstring& id, int buckets);  // im Hintergrund, Ergebnis als MUSIC_WAVE

struct Song {
    std::wstring id, file, title, artist, cover, url;
    int duration = 0;  // Sekunden
};

struct Playlist {
    std::wstring name;
    std::vector<std::wstring> ids;
};

struct DownloadJob {
    std::wstring url, title;
    int percent = 0;
    int stage = 0;  // 0 wartet, 1 lädt, 2 wandelt um, 3 Cover, 4 fertig, -1 Fehler
    std::wstring error;
};

struct CoverResults {
    std::wstring songId;
    std::vector<std::wstring> files;  // heruntergeladene Vorschaubilder (lokale Dateien)
};

// ---- Bibliothek (nur UI-Thread)
void Music_Init(HWND notify);
std::vector<Song>& Music_Songs();
std::vector<Playlist>& Music_Playlists();
void Music_Save();
std::wstring Music_Folder();
void Music_SetFolder(const std::wstring& folder);  // leer = Standard (Musik\HoverBar)
int Music_MoveSongsToFolder();                       // vorhandene Songs dorthin verschieben
int Music_Find(const std::wstring& id);  // Index in Music_Songs() oder -1

// ---- YouTube -> MP3
void Music_AddUrl(const std::wstring& url);
std::vector<DownloadJob> Music_Jobs();
void Music_ClearFinishedJobs();
bool Music_ToolAvailable();

// ---- Bearbeiten
void Music_UpdateMeta(const std::wstring& id, const std::wstring& title, const std::wstring& artist);
bool Music_SetCoverFromFile(const std::wstring& id, const std::wstring& imagePath);  // zuschneiden, speichern, in MP3 eintragen
void Music_SearchCovers(const std::wstring& id, const std::wstring& artist, const std::wstring& title);  // Ergebnis kommt als MUSIC_COVERS
void Music_Delete(const std::wstring& id);
double Music_Length(const std::wstring& id);                               // echte Länge der MP3 in Sekunden
bool Music_Trim(const std::wstring& id, double from, double to, bool keepInside);  // verlustfrei schneiden
bool Music_Undo(const std::wstring& id);                                  // letzten Schnitt rückgängig
int Music_UndoCount(const std::wstring& id);
void Player_Preview(const std::wstring& id, double from, double to);      // Ausschnitt probehören

// ---- Wiedergabe
struct PlayerState {
    bool active = false, playing = false;
    std::wstring songId;
    double pos = 0, dur = 0;  // Sekunden
};
void Player_Play(const std::vector<std::wstring>& ids, int start);
void Player_Toggle();
void Player_Next();
void Player_Prev();
void Player_Seek(double fraction);
void Player_Stop();
void Player_SetVolume(double v);  // 0..1
void Player_SetModes(bool shuffle, int repeat);  // Zufall; Wiederholen: 0 aus, 1 Song, 2 Playlist
void Player_SetDuck(double f);    // zusätzlicher Faktor, solange andere Medien laufen
bool Music_OtherMediaPlaying();   // Browser/Video-Player gibt gerade Ton aus
void Net_Status(int& kind, int& bars, bool& internet, std::wstring& name);  // für die Kopfzeile
PlayerState Player_State();
