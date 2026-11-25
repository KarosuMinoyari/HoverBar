// Musik-Player für HoverBar: steuert die Wiedergabe von Windows (Spotify, Browser, Handy …)
// und kann Musik vom Handy per Bluetooth auf den PC holen.
// Läuft in einem eigenen Thread und nur, solange die Leiste offen ist (oder ein Handy verbunden ist).
#pragma once
#include <windows.h>
#include <string>
#include <vector>

enum { WM_APP_MEDIA = WM_APP + 20, WM_APP_PHONES, WM_APP_PHONESTATE };

// WM_APP_MEDIA, lParam = MediaInfo* (Empfänger löscht)
struct MediaInfo {
    bool has = false, playing = false, canPrev = false, canNext = false;
    bool audioOnly = false;  // keine Medien-Sitzung, aber ein Programm/Handy gibt gerade Ton aus (Titel unbekannt)
    std::wstring source;     // Name der Quelle, z. B. "Spotify" oder "Handy (Bluetooth)"
    std::wstring title, artist, app;
    bool artChanged = false;
    HBITMAP art = nullptr;  // Albumbild in Anzeigegröße, vormultipliziert, runde Ecken (oder nullptr)
};

struct PhoneDevice { std::wstring name, id; };

// WM_APP_PHONES, lParam = PhoneList* (Empfänger löscht)
struct PhoneList {
    std::vector<PhoneDevice> devices;
    std::wstring connectedId;
};

// WM_APP_PHONESTATE, lParam = PhoneState* (Empfänger löscht)
struct PhoneState {
    bool connected = false;
    std::wstring name, error;
};

enum MediaCmd { MC_NONE = 0, MC_PREV = 1, MC_PLAYPAUSE = 2, MC_NEXT = 3, MC_LISTPHONES = 4, MC_CONNECT = 5, MC_DISCONNECT = 6 };

void Media_Open(HWND notify, int artPx);  // Leiste geöffnet
void Media_Close();                       // Leiste geschlossen
void Media_Command(int cmd);
void Media_ConnectPhone(const std::wstring& id, const std::wstring& name);
void Media_Shutdown();
void Media_SetSpotifyOnly(bool on);  // Musikquelle Spotify: nur dessen Sitzung anzeigen und steuern
