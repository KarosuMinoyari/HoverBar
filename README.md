# HoverBar

Eine Leiste am rechten Bildschirmrand für Windows: Maus an den Rand halten (oder die Ende-Taste drücken), und es fährt eine Leiste mit deinen Apps, Musik, Kalender, Claude-Chat und Speicherübersicht heraus. Im Ruhezustand braucht sie fast keinen Arbeitsspeicher und läuft im Windows-Effizienzmodus. Eine einzelne `HoverBar.exe`, ohne Installation und ohne Systemeingriffe.

## Herunterladen

Die neueste `HoverBar.exe` gibt es unter [Releases](https://github.com/KarosuMinoyari/HoverBar/releases/latest). In einen beliebigen Ordner legen und starten. Danach aktualisiert sich HoverBar auf Knopfdruck selbst: Zahnrad → Funktionen → *Nach Update suchen*. Es wird nie automatisch nach Updates gesucht.

Voraussetzungen: Windows 10/11. Für die Claude-Seite braucht es die WebView2-Laufzeit (bei Windows 11 und mit Edge schon vorhanden).

## Seiten (Leiste links)

Die Symbole sind unten angeheftet. Mit dem Mausrad über der Leiste blätterst du Seite für Seite (eine Raste = eine Seite).

| Seite | Inhalt |
| --- | --- |
| Apps | Ordner mit Apps, Favoriten, Favoriten-Links, „Zuletzt genutzt“ (stark nach oben überscrollen). Klick auf „Apps“ zeigt alle Apps mit Größe, laufende mit Arbeitsspeicher und Beenden-Knopf. |
| Bibliothek | Alle Songs, darüber fest: Favoriten, Playlists, Zuletzt gespielt. |
| Kalender | Monatsansicht mit Terminen. |
| Claude | claude.ai eingebettet (WebView2). Anmeldung bleibt gespeichert, der Speicher wird beim Schließen freigegeben. |
| Speicher | Nur mit dem optionalen Plugin WizTree (Zahnrad → Vorgeschlagen). Laufwerke und größte Apps. |

## Apps

- **Öffnen:** Maus kurz an den rechten Rand halten. Nicht über Vollbild-Spielen oder -Videos. Die Ende-Taste öffnet und schließt ebenfalls.
- **Suchen:** einfach lostippen, Enter startet den ersten Treffer.
- **Ordner:** Großes Symbol = App starten, kleine Symbole oder Ordnername = Ordner öffnen. Sortieren, verschieben und ausblenden unter Zahnrad → Ordner oder per Rechtsklick.
- **Startseite (4 Apps)** und **Favoriten-Links** (kleine Reihe mit Website-Symbol und Name).
- **Schließen:** Maus an den linken Rand (linke 15 %), Esc, Alt+Tab oder eine App starten.

## Musik

Eigener Player für die Songs im Musikordner (Standard `Musik\HoverBar`, änderbar unter Funktionen). Songs kommen aus YouTube-Links: Link ins Feld der Bibliothek einfügen und Enter, oder Alt+Ende (markierter Link oder die Adresse der offenen YouTube-Seite). Dafür lädt HoverBar auf Wunsch `yt-dlp` herunter (Funktionen → *yt-dlp*, mit Prüfsumme).

- **Musikleiste unten:** Cover (Klick öffnet „Läuft gerade“), Titel, Fortschritt, Steuerung, Lautstärke (klappt beim Darüberfahren aus), Herz für Favoriten, Zufall/Wiederholen (jeder Klick wechselt den Modus: aus, Zufall, Playlist wiederholen, Song wiederholen).
- **Läuft gerade:** Großes Cover, Steuerung, Songtext (aus einer `.lrc`-Datei neben dem Song oder von lrclib.net, dabei werden Titel und Künstler übertragen). Mit dem Mausrad im Text suchen, Klick springt dorthin; nach 2 s geht es zurück zur aktuellen Zeile. Nach unten scrollen klappt die Ansicht in die Bibliothek ein. Rechtsklick aufs Cover: Song bearbeiten.
- **Song schneiden:** Rechtsklick → *Bearbeiten / Schneiden*. Linien für Anfang und Ende (auf die Millisekunde), Wellenform, Rückgängig, „Auswahl entfernen“. Verlustfrei.
- **Tasten:** Ende halten + Pos1 = Play/Pause, Bild ab = weiter, Bild auf = zurück, „+“/„−“ = Lautstärke. Steuern nur den eigenen Player.
- **Leiser bei Videos:** Spielt ein Browser oder Video-Player Ton, wird die Musik auf 10 % abgesenkt.
- Alles lässt sich unter Funktionen abschalten (Hauptschalter „Musik-Player“), auch zu Spotify wechseln.

## Spiele

Ordner im Spielmodus (Standard: „Spiele“) zeigen Größe, Spielzeit und „zuletzt gespielt“ als Liste. Quellen: Steam, Epic Games, Riot Games, andere Spiele (selbst gemessen); je einzeln abschaltbar. Bisherige Spielzeit lässt sich per Rechtsklick nachtragen.

## Kopfzeile

Logo, Titel, Zahnrad; rechts Bluetooth, WLAN, Akku und das Menü `···` (Neu laden, Mit Windows starten, Nur auf dem Desktop öffnen, Beenden).

## Daten und Datenschutz

- Einstellungen: `%APPDATA%\HoverBar\HoverBar.ini`; Termine: `termine.txt` im selben Ordner.
- Netzwerk nur auf Anforderung: YouTube-Downloads, Cover-Suche (Deezer/iTunes), Songtexte (lrclib.net), Website-Symbole der Favoriten-Links, `yt-dlp`/WizTree/HoverBar-Updates (GitHub bzw. diskanalyzer.com), die Claude-Seite. Es gibt keine Konten und keine Telemetrie.
- Stürzt HoverBar ab, startet die geplante Aufgabe „HoverBar Neustart“ es neu (wird beim ersten Start automatisch angelegt).

## Bauen

Voraussetzung: Visual Studio 2022 Build Tools mit C++.

```
build.bat
```

Ergebnis: `build\HoverBar.exe`. Das WebView2-SDK liegt unter `third_party\WebView2` (eigene Lizenz, siehe dort).

## Neue Version veröffentlichen

1. `HB_VERSION` in `src\update.h` erhöhen.
2. `tools\release.ps1` ausführen: baut, berechnet die SHA-256-Prüfsumme und erstellt das GitHub-Release `vX.Y.Z` mit `HoverBar.exe` und `HoverBar.exe.sha256`. Der Update-Knopf in HoverBar prüft diese Summe vor dem Einsetzen.

## Aufbau

```
src/           Quelltext (main.cpp = Oberfläche, music/media/games/plugins/chat/update)
res/           Icon, Ressourcen, Skript zum Erzeugen des Icons
third_party/   WebView2-SDK
tools/         Release-Skript
build.bat      Build
```

Mit Hilfe von Claude entstanden.
