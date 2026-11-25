// Optionale Erweiterungen ("Vorgeschlagen"): werden nur auf Wunsch heruntergeladen.
// WizTree: Speicher-Analyse mit Baumansicht (kostenlos für private Nutzung, von diskanalyzer.com).
#pragma once
#include <windows.h>
#include <string>

enum { WM_APP_PLUGIN = WM_APP + 80 };  // Stand einer Erweiterung hat sich geändert

enum { PLUG_IDLE = 0, PLUG_WORKING, PLUG_DOWNLOADING, PLUG_DONE, PLUG_ERROR };
struct PlugStatus {
    int state = PLUG_IDLE;
    int percent = -1;
    std::wstring msg;
};

std::wstring WizTree_Exe();                    // Pfad zu WizTree64.exe, leer wenn nicht installiert
std::wstring WizTree_Version();                // z. B. "4.34" (aus der Datei), leer wenn unbekannt
PlugStatus WizTree_Status();
void WizTree_Install(HWND notify);             // aktuelle portable Version suchen, laden, prüfen, entpacken
bool WizTree_Remove();
bool WizTree_Open(const std::wstring& folder);  // Baumansicht für einen Ordner oder ein Laufwerk
