# HoverBar

Eine Leiste am rechten Bildschirmrand für Windows: Maus an den Rand halten (oder die Ende-Taste drücken), und es fährt eine Leiste mit deinen Apps, Musik, Kalender, Claude-Chat und Speicherübersicht heraus. Im Ruhezustand braucht sie fast keinen Arbeitsspeicher und läuft im Windows-Effizienzmodus. Eine einzelne `HoverBar.exe`, ohne Installation und ohne Systemeingriffe.

## Herunterladen

Unter [Releases](https://github.com/KarosuMinoyari/HoverBar/releases/latest) gibt es zwei Dateien:

- **`setupHoverBar.exe`** (empfohlen): richtet alles ein. Kopiert HoverBar nach `%LOCALAPPDATA%\Programs\HoverBar`, legt eine Startmenü-Verknüpfung und einen Eintrag unter „Apps & Features“ an, richtet auf Wunsch den Autostart ein und startet HoverBar. Keine Administratorrechte nötig. Nochmal ausführen aktualisiert, `Deinstallieren.exe` im Programmordner (oder „Apps & Features“) entfernt alles wieder. Windows warnt eventuell („Unbekannter Herausgeber“), weil die Datei nicht signiert ist: *Weitere Informationen → Trotzdem ausführen*.
- **`HoverBar.exe`**: nur das Programm, in einen beliebigen Ordner legen und starten.

Danach aktualisiert sich HoverBar auf Knopfdruck selbst: Zahnrad → Funktionen → *Nach Update suchen*. Es wird nie automatisch nach Updates gesucht.

Für den Installer ohne Oberfläche: `setupHoverBar.exe --still` (weitere Schalter: `--kein-autostart`, `--nicht-starten`, `--deinstallieren`).

Voraussetzungen: Windows 10/11. Für die Claude-Seite braucht es die WebView2-Laufzeit (bei Windows 11 und mit Edge schon vorhanden).

## Seiten (Leiste links)

Die Symbole sind unten angeheftet. Mit dem Mausrad über der Leiste blätterst du Seite für Seite (eine Raste = eine Seite).

| Seite | Inhalt |
| --- | --- |
| Apps | Ordner mit Apps, Favoriten, Favoriten-Links, „Zuletzt genutzt“ (stark nach oben überscrollen). Klick auf „Apps“ zeigt alle Apps mit Größe, laufende mit Arbeitsspeicher und Beenden-Knopf. |
| Bibliothek | Alle Songs, darüber fest: Favoriten, Playlists, Zuletzt gespielt. |
| Kalender | Monatsansicht mit Terminen. |
| Claude | Optionale Erweiterung (Zahnrad → Vorgeschlagen → *Hinzufügen*). claude.ai eingebettet (WebView2), Anmeldung bleibt gespeichert, der Speicher wird beim Schließen freigegeben. *Entfernen* löscht auch die gespeicherte Anmeldung. |
| Speicher | Nur mit der optionalen Erweiterung WizTree (Zahnrad → Vorgeschlagen). Laufwerke und größte Apps. |

## Apps

- **Öffnen:** Maus kurz an den rechten Rand halten. Nicht über Vollbild-Spielen oder -Videos. Die Ende-Taste öffnet und schließt ebenfalls.
- **Suchen:** einfach lostippen, Enter startet den ersten Treffer.
- **Ordner:** Großes Symbol = App starten, kleine Symbole oder Ordnername = Ordner öffnen. Sortieren, verschieben und ausblenden unter Zahnrad → Ordner oder per Rechtsklick.
- **Startseite (4 Apps)** und **Favoriten-Links** (kleine Reihe mit Website-Symbol und Name).
- **Schließen:** Maus an den linken Rand (linke 15 %), Esc, Alt+Tab oder eine App starten.

## Musik

Eigener Player für die Songs im Musikordner (Standard `Musik\HoverBar`, änderbar unter Funktionen). Songs kommen aus YouTube-Links: Link ins Feld der Bibliothek einfügen und Enter, oder Alt+Ende (Taste unter Funktionen → *Link laden* änderbar; markierter Link oder die Adresse der offenen YouTube-Seite). Dafür lädt HoverBar auf Wunsch `yt-dlp` herunter (Funktionen → *yt-dlp*, mit Prüfsumme).

- **Musikleiste unten:** Cover (Klick öffnet „Läuft gerade“), Titel, Fortschritt, Steuerung, Lautstärke (klappt beim Darüberfahren aus), Herz für Favoriten, Zufall/Wiederholen (jeder Klick wechselt den Modus: aus, Zufall, Playlist wiederholen, Song wiederholen).
- **Läuft gerade:** Großes Cover, Steuerung, Songtext (aus einer `.lrc`-Datei neben dem Song oder von lrclib.net, dabei werden Titel und Künstler übertragen). Mit dem Mausrad im Text suchen, Klick springt dorthin; nach 2 s geht es zurück zur aktuellen Zeile. Nach unten scrollen klappt die Ansicht in die Bibliothek ein. Rechtsklick aufs Cover: Song bearbeiten.
- **Song schneiden:** Rechtsklick → *Bearbeiten / Schneiden*. Linien für Anfang und Ende (auf die Millisekunde), Wellenform, Rückgängig, „Auswahl entfernen“. Verlustfrei.
- **Tasten:** Ende halten + Pos1 = Play/Pause, Bild ab = weiter, Bild auf = zurück, „+“/„−“ = Lautstärke. Steuern nur den eigenen Player.
- **Meistgespielt:** Der Knopf neben „Alle abspielen“ sortiert „Alle Songs“ nach Titel oder nach Anzahl der Wiedergaben.
- **Leiser bei Videos:** Spielt ein Browser oder Video-Player Ton, wird die Musik auf 10 % abgesenkt.
- **Tastatur:** Pfeiltasten wandern durch die Leiste, Enter aktiviert, die Menü-Taste ist der Rechtsklick. Während du tippst, gehören links/rechts dem Textfeld.
- **Grafik:** Cover in den drei Musik-Kacheln und feine Trennlinien, beides unter Design abschaltbar.
- Alles lässt sich unter Funktionen abschalten (Hauptschalter „Musik-Player“), auch zu Spotify wechseln.

## Musik-Sync (Erweiterung: Musik aufs Handy)

Optional unter Zahnrad → Vorgeschlagen → *Musik-Sync*. Legt deine Songs, Cover und Playlists in einen Ordner deines Cloud-Programms, damit andere Geräte (z. B. eine Handy-App) sie spielen können.

- **Beliebiger Anbieter:** HoverBar erkennt installierte Cloud-Programme (Google Drive, OneDrive, Koofr, MEGA, pCloud, Dropbox) und zeigt bei Google Drive und pCloud den freien Platz. Fehlt ein Dienst, installiert ihn ein Klick (Eintrag „… installieren“) über den Windows-Paketmanager winget (Hersteller-Paket mit Prüfsumme, Windows fragt eventuell nach Administratorrechten; danach im Cloud-Programm anmelden und wieder auf „Einrichten“ klicken). Gibt es kein winget (oder kein Paket, z. B. bei Koofr), lädt HoverBar den Installer direkt vom Hersteller (HTTPS) und startet ihn erst, wenn die digitale Signatur gültig ist und zum Hersteller passt (Koofr, MEGA, Google Drive, OneDrive, Dropbox); eine veränderte Datei wird abgelehnt. Nur wenn es gar keinen Weg gibt (pCloud ohne winget), öffnet der Eintrag die Download-Seite. Nach der Installation startet HoverBar das Programm, wartet auf die Anmeldung und richtet den Musik-Sync dann automatisch ein. Ein Dienst gilt nur als installiert, wenn das Programm wirklich da ist (übrig gebliebene Startmenü-Verknüpfungen zählen nicht). Alternativ ein eigener Ordner. Reicht der Platz nicht, bricht HoverBar vor dem Verschieben ab (bei Anbietern, die ihren Speicher nicht als Laufwerk zeigen, steht ein Hinweis).
- **Einrichten:** Die Songs werden im Hintergrund in `<Cloud>\HoverBar-Musik` verschoben, danach spielt HoverBar von dort. Neue Downloads landen ebenfalls dort. Das Cloud-Programm lädt hoch, HoverBar selbst überträgt nichts ins Internet. Nur für die eigene Musik gedacht, nicht zum Teilen.
- **Bibliotheksdatei:** `HoverBar-Bibliothek.json` im Ordner wird bei jeder Änderung (Songs, Playlists, Favoriten) aktualisiert, nur wenn sich der Inhalt geändert hat. Cover liegen in `Cover/`. Aufbau:

```json
{
  "format": "HoverBar-Bibliothek", "version": 1, "updated": "2026-11-25T12:30:00",
  "songs": [ {"id":"…","file":"Name.mp3","title":"…","artist":"…","duration":156,"plays":3,"cover":"Cover/….jpg","lyrics":"Songtexte/Name.lrc","url":"https://…"} ],
  "playlists": [ {"name":"…","songs":["id", …]} ],
  "favorites": ["id", …]
}
```

`file` und `cover` sind relativ zum Ordner. Aufgeführt werden nur Songs, die wirklich im Ordner liegen.
- **Handy verbinden (QR-Code):** Nach dem Einrichten (und jederzeit über den Knopf „Handy verbinden“) zeigt HoverBar einen QR-Code. Er enthält einen Link auf die HoverMusic-Downloadseite (die normale Handy-Kamera führt also zur App) und dahinter, nach `#hm=`, die Einrichtung als Base64-JSON: `{"v":1,"cloud":"MEGA","folder":"HoverBar-Musik","pkg":"<Android-Paket der Cloud-App>","parent":"<Ordner davor>"}`. Keine Zugangsdaten und keine Pfade vom PC. HoverMusic liest den Code und führt durch die Cloud-App und die Ordnerwahl.
- **Ordner:** Songtexte liegen in `Songtexte/`, Cover in `Cover/`. Änderungen vom Handy (`HoverMusic-Aenderungen.txt`) übernimmt HoverBar beim nächsten Öffnen.
- **Ausschalten:** Die Songs bleiben im Cloud-Ordner, nur die Bibliotheksdatei wird nicht mehr gepflegt.
- **Tests:** `HoverBar.exe --musiksync <Ordner>` richtet den Sync ohne Rückfragen in einem beliebigen Ordner ein, `--musiksync-aus` schaltet ihn ab (bei laufender HoverBar). `--qrtest <Datei>` schreibt QR-Beispiele (HoverMusic dekodiert sie in den Unit-Tests mit ZXing), `--qrfenster` zeigt das QR-Fenster mit Beispieldaten.

## HoverMusic (Android-App)

Eigene App im Ordner `HoverMusic/` (Kotlin, Jetpack Compose, Media3), mit eigenen Releases: Tags `hovermusic-v…`, Datei `HoverMusic-<Version>.apk` (siehe [Releases](https://github.com/KarosuMinoyari/HoverBar/releases)). Sie spielt die Musik aus dem Musik-Sync von HoverBar auf dem Handy:

- **Einrichten:** In HoverBar am PC *Musik-Sync* einrichten, am Ende erscheint der QR-Code. In HoverMusic „QR-Code scannen“ (nutzt den Code-Scanner der Google Play-Dienste, ohne Kamera-Berechtigung; alternativ „Link aus der Zwischenablage“): Die App führt durch die Installation der Cloud-App auf dem Handy und die Wahl des Ordners `HoverBar-Musik` (Android-Ordnerauswahl: Google Drive, OneDrive, MEGA, Koofr … erscheinen dort). Es werden keine Zugangsdaten und keine Cloud-Schnittstellen gebraucht, die App liest nur diesen Ordner.
- **Funktionen:** Alle Songs, Favoriten, Playlists, Zuletzt gespielt; „Läuft gerade“ mit großem Cover, Fortschritt, Zurück/Play/Weiter, Zufall/Wiederholen (jeder Klick wechselt: aus → Zufall → Playlist wiederholen → Song wiederholen), Herz; Songtext mit Mitscrollen (Tippen auf eine Zeile springt dorthin, nach 2 s geht es zur aktuellen Zeile zurück); Wiedergabe im Hintergrund mit Steuerung in der Benachrichtigung und auf dem Sperrbildschirm.
- **Von YouTube laden:** In YouTube auf *Teilen* → HoverMusic. yt-dlp wird beim ersten Start in der neuesten Version geholt (Aktualisieren über das Download-Symbol). Diese Songs bleiben auf dem Handy, nicht im Cloud-Ordner.
- **Meistgespielt:** „Alle Songs“ lässt sich nach Wiedergaben sortieren (Zahl vom PC plus die auf dem Handy).
- **Bearbeiten:** Song lange drücken oder Stift in „Läuft gerade“ (Titel, Künstler, Cover vom Handy oder aus dem Web, Schneiden). Ein Schnitt wirkt auf dem Handy sofort, die Datei schneidet HoverBar. Braucht Schreibrecht: bei älterer Einrichtung den Ordner neu wählen.
- **Einstellungen:** Farbe, Hintergrundbild, Glas-Effekt, Schrift, Ausfahren und die Grafik-Extras.
- **Einhändig laden:** Direktziel „Song laden“ im Teilen-Menü oder die gleichnamige Kachel in den Schnelleinstellungen (kopierter Link).
- **Favoriten und „Zuletzt“** liegen lokal auf dem Handy (die Favoriten vom PC werden einmal übernommen). Die Bibliothek wird beim Start und über den Aktualisieren-Knopf neu gelesen und für den Start ohne Netz zwischengespeichert. Die Songs selbst streamt oder lädt der Cloud-Anbieter beim Abspielen; wer sie offline braucht, markiert den Ordner im Cloud-App als „offline verfügbar“.
- **Update:** Einstellungen → *App* → *Nach Updates suchen* lädt die neueste Version (ohne Vorabversionen), prüft die SHA-256 und öffnet den Android-Installer.
- **Installieren:** APK aus dem Release auf dem Handy öffnen und „Installation aus dieser Quelle“ erlauben (die App ist nicht im Play Store). Voraussetzung: Android 8 oder neuer.
- **Bauen:** JDK 17 und Android SDK (Plattform 34), dann in `HoverMusic/`: `gradlew assembleDebug` bzw. `gradlew test`. Für signierte Releases liegt eine `keystore.properties` (nicht im Repo) neben dem Projekt; `tools\release-hovermusic.ps1` baut und veröffentlicht.

## Spiele

Ordner im Spielmodus (Standard: „Spiele“) zeigen Größe, Spielzeit und „zuletzt gespielt“ als Liste. Quellen: Steam, Epic Games, Riot Games, andere Spiele (selbst gemessen); je einzeln abschaltbar. Bisherige Spielzeit lässt sich per Rechtsklick nachtragen.

## Kopfzeile

Logo, Titel, Zahnrad; rechts Bluetooth, WLAN, Akku und das Menü `···` (Neu laden, Mit Windows starten, Nur auf dem Desktop öffnen, Beenden).

## Daten und Datenschutz

- Einstellungen: `%APPDATA%\HoverBar\HoverBar.ini`; Termine: `termine.txt` im selben Ordner.
- Netzwerk nur auf Anforderung: YouTube-Downloads, Cover-Suche (Deezer/iTunes), Songtexte (lrclib.net), Website-Symbole der Favoriten-Links (und das Claude-Symbol von claude.ai, sobald du den Reiter „Vorgeschlagen“ öffnest oder Claude hinzugefügt hast), `yt-dlp`/WizTree/HoverBar-Updates (GitHub bzw. diskanalyzer.com), die Claude-Seite. Es gibt keine Konten und keine Telemetrie.
- Stürzt HoverBar ab, startet die geplante Aufgabe „HoverBar Neustart“ es neu (wird beim ersten Start automatisch angelegt).

## Bauen

Voraussetzung: Visual Studio 2022 Build Tools mit C++.

```
build.bat
```

Ergebnis: `build\HoverBar.exe`. Das WebView2-SDK liegt unter `third_party\WebView2` (eigene Lizenz, siehe dort).

## Neue Version veröffentlichen

1. `HB_VERSION` in `src\update.h` erhöhen.
2. `tools\release.ps1` ausführen: baut HoverBar und den Installer (`tools\build_setup.bat`), berechnet die SHA-256-Prüfsumme und erstellt das GitHub-Release `vX.Y.Z` mit `setupHoverBar.exe`, `HoverBar.exe` und `HoverBar.exe.sha256`. Der Update-Knopf in HoverBar prüft diese Summe vor dem Einsetzen.

## Aufbau

```
src/           Quelltext (main.cpp = Oberfläche, music/media/games/plugins/chat/update, qrcode = QR-Encoder)
res/           Icon, Ressourcen, Skript zum Erzeugen des Icons
setup/         Installer (setupHoverBar.exe)
HoverMusic/    Android-App (eigenes Gradle-Projekt)
third_party/   WebView2-SDK
tools/         Release-Skript, Installer-Build
build.bat      Build
```

Mit Hilfe von Claude entstanden.
