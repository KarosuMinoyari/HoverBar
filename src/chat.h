// Claude-Chat in der HoverBar: claude.ai in einer eingebetteten Webansicht (WebView2).
// Das Fenster liegt genau über dem Inhaltsbereich der Leiste. Speicher wird beim Schließen der Leiste freigegeben;
// die Anmeldung bleibt im eigenen Profilordner (%LOCALAPPDATA%\HoverBar\webview) erhalten.
#pragma once
#include <windows.h>
#include <string>

void Chat_Show(HWND owner, const RECT& screenRect, int cornerPx);  // anzeigen / verschieben
void Chat_Hide();
void Chat_Free();                     // Webansicht beenden (gibt den Arbeitsspeicher frei)
bool Chat_IsOwnWindow(HWND h);        // gehört h zum Chat (für Fokus-/Schließen-Logik)?
void Chat_Navigate(const std::wstring& url);
std::wstring Chat_Status();           // "", "Lädt …" oder Fehlermeldung
