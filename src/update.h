// Selbst-Update: holt die neueste HoverBar.exe aus den GitHub-Releases (nur auf Knopfdruck, nie automatisch).
// Ablauf: neueste Version erfragen -> HoverBar.exe laden -> SHA-256 prüfen -> alte Datei zur Seite, neue an ihre Stelle -> Neustart.
#pragma once
#include <windows.h>
#include <string>

#define HB_VERSION L"1.3.1"
#define HB_REPO "KarosuMinoyari/HoverBar"  // GitHub: Besitzer/Repository

enum { WM_APP_UPDATE = WM_APP + 90 };
enum { UPD_IDLE = 0, UPD_CHECKING, UPD_AVAILABLE, UPD_UPTODATE, UPD_DOWNLOADING, UPD_VERIFYING, UPD_READY, UPD_ERROR };
struct UpdateStatus {
    int state = UPD_IDLE;
    int percent = -1;
    std::wstring latest, msg;
};
UpdateStatus Update_Status();
void Update_Check(HWND notify);     // nur nachsehen
void Update_Install(HWND notify);   // herunterladen, prüfen, ersetzen (bei UPD_AVAILABLE)
void Update_Restart();              // neue Version starten (der Aufrufer beendet danach die Leiste)
void Update_Cleanup();              // Reste der alten Version löschen
