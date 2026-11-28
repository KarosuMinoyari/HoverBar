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
enum { MUSIC_SONG = 1, MUSIC_JOBS = 2, MUSIC_COVERS = 3, MUSIC_WAVE = 4, MUSIC_TOOL = 5, MUSIC_LYRICS = 6, MUSIC_SYNC = 7, MUSIC_PLAYED = 8, MUSIC_SUGGEST = 9, MUSIC_FOUND = 10, MUSIC_PREVIEW = 11 };  // FOUND: Suchtreffer (std::vector<Suggestion>*)  // PLAYED: lParam = std::wstring* (Song-ID), Empfänger löscht

// ---- Musik-Sync (Erweiterung): Songs in einem Cloud-Ordner, dazu HoverBar-Bibliothek.json mit Playlists, Favoriten und Covern (Cover\ daneben).
// Die Datei ist das Austauschformat für andere Geräte (z. B. eine Handy-App): Dateinamen sind relativ zum Ordner.
struct SyncStatus {
    int state = 0;  // 0 nichts, 1 verschiebt Songs, 2 fertig, 3 Fehler
    int done = 0, total = 0;
    std::wstring msg;
};
struct SyncResult {  // MUSIC_SYNC (lParam = SyncResult*, Empfänger löscht): neue Dateipfade der verschobenen Songs
    std::vector<std::pair<std::wstring, std::wstring>> moved;  // Song-ID, neuer Pfad
    int failed = 0;
};
void Music_SetSyncExport(bool on);                         // Bibliotheksdatei bei jeder Änderung schreiben
bool Music_SyncExportOn();
void Music_SetFavorites(const std::vector<std::wstring>& ids);
bool Music_ExportLibrary();                                // schreibt HoverBar-Bibliothek.json in den Song-Ordner und kopiert fehlende Cover in dessen Unterordner Cover
unsigned long long Music_FolderState(bool& busy);          // Prüfwert der Dateien im Song-Ordner; busy = das Cloud-Programm lädt noch
int Music_ImportFolder();                                  // neue .mp3/.m4a im Song-Ordner aufnehmen (z. B. vom Handy); Anzahl
bool Music_ImportEdits();                                  // Änderungen vom Handy (HoverMusic-Aenderungen.txt) übernehmen; true, wenn etwas geändert wurde
std::wstring Music_SyncInfo();                             // "27 Songs · Bibliothek 12:30"
void Music_SyncSetup(HWND notify, const std::wstring& dir);  // Songs im Hintergrund in den Ordner verschieben (Ergebnis als MUSIC_SYNC)
SyncStatus Music_SyncStatus();

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
std::wstring Music_ReadLyrics(const std::wstring& id);                          // Inhalt der .lrc, leer = keine
bool Music_SaveLyrics(const std::wstring& id, const std::wstring& text);        // leerer Text löscht die .lrc

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
    int plays = 0;     // wie oft abgespielt
    std::wstring playlist;  // nur beim Import: in diese Playlist legen (wird nicht gespeichert)
    bool repair = false;    // nur beim Neu-Laden eines unlesbaren Songs: die Datei ersetzen
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
    std::wstring artist, name, playlist;  // beim Import / bei Vorschlägen: bekannte Angaben
    std::wstring repairId, repairFile;    // Neu-Laden eines unlesbaren Handy-Songs: dessen ID und bisherige Datei
    int seconds = 0;
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
// Titel suchen (YouTube) und laden; seconds = erwartete Länge, es zählt nur ein Video mit ±5 s (0 = egal), playlist = danach dort einsortieren
void Music_AddSearch(const std::wstring& artist, const std::wstring& name, int seconds, const std::wstring& playlist);
// Spotify-Export (CSV, z. B. von Exportify): fehlende Titel laden, vorhandene nur in die Playlist legen; Anzahl Titel, -1 = Datei nicht lesbar
int Music_ImportCsv(const std::wstring& file, int& downloads);
// ---- Vorschläge (Deezer): ähnliche Künstler zu den meistgehörten; Ergebnis als MUSIC_SUGGEST (lParam = std::vector<Suggestion>*, Empfänger löscht)
struct Suggestion { std::wstring artist, title, preview; int seconds = 0; bool added = false; std::wstring url; std::wstring cover; };  // cover: Albumbild der Hörprobe (lokale Datei)  // url: YouTube-Treffer
void Music_Suggest();
bool Music_PreviewActive();                               // läuft gerade eine Hörprobe?
std::string Net_Get(const std::wstring& url);             // einfacher HTTPS-Abruf (blockiert)
void Music_Search(const std::wstring& query);             // YouTube durchsuchen (braucht yt-dlp)
// Adresse der Tonspur eines Suchtreffers holen; Ergebnis als MUSIC_PREVIEW (std::pair<Video, Tonspur>*, leer = ging nicht)
void Music_FindPreview(const std::wstring& video);
void Music_AddToPlaylist(const std::wstring& name, const std::wstring& id);  // Playlist anlegen, falls es sie nicht gibt
void Music_PlayPreview(const std::wstring& url);           // 30-s-Hörprobe; leer = stoppen

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
// Equalizer (Erweiterung): Voreinstellung 0 = Flat; wirkt sofort auf den laufenden Song
void Player_SetEq(bool on, int preset);
int Eq_Count();
const wchar_t* Eq_Name(int preset);
bool Music_OtherMediaPlaying();   // Browser/Video-Player gibt gerade Ton aus
void Net_Status(int& kind, int& bars, bool& internet, std::wstring& name);  // für die Kopfzeile
PlayerState Player_State();
