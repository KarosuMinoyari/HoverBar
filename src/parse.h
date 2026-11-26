// Kleine Parser für Daten von außen (Spotify-CSV, Deezer-JSON). Test: tests\parse_test.cpp
#pragma once
#include <cstdlib>
#include <string>
#include <vector>

// CSV mit Anführungszeichen ("" = ein Anführungszeichen, Komma und Zeilenumbruch darin erlaubt); BOM wird übersprungen
inline std::vector<std::vector<std::string>> ParseCsv(const std::string& s) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string cell;
    bool quoted = false;
    for (size_t i = s.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0; i < s.size(); i++) {
        char c = s[i];
        if (quoted) {
            if (c != '"') cell += c;
            else if (i + 1 < s.size() && s[i + 1] == '"') cell += s[++i];
            else quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == ',') { row.push_back(cell); cell.clear(); }
        else if (c == '\n') {
            row.push_back(cell);
            cell.clear();
            if (row.size() > 1 || !row[0].empty()) rows.push_back(row);
            row.clear();
        } else if (c != '\r') cell += c;
    }
    if (!cell.empty() || !row.empty()) { row.push_back(cell); rows.push_back(row); }
    return rows;
}

// Zeilen "<Video-ID> <Sekunden>" (yt-dlp --flat-playlist): erstes Video, dessen Länge höchstens tolerance abweicht; leer = keins
inline std::string PickByDuration(const std::string& lines, int seconds, int tolerance = 5) {
    for (size_t a = 0; a < lines.size();) {
        size_t b = lines.find('\n', a);
        if (b == std::string::npos) b = lines.size();
        std::string line = lines.substr(a, b - a);
        a = b + 1;
        size_t sp = line.find(' ');
        if (sp != 11) continue;  // keine Video-ID (z. B. eine Warnung)
        double d = atof(line.c_str() + sp + 1);
        if (d > 0 && d >= seconds - tolerance && d <= seconds + tolerance) return line.substr(0, sp);
    }
    return "";
}

// Suchtreffer "<Video-ID>|<Sekunden>|<Kanal>|<Titel>" (der Titel darf selbst "|" enthalten); andere Zeilen (Warnungen) zählen nicht
struct Hit11 { std::string id, channel, title; int seconds; };
inline std::vector<Hit11> ParseSearch(const std::string& lines) {
    std::vector<Hit11> out;
    for (size_t a = 0; a < lines.size();) {
        size_t b = lines.find('\n', a);
        if (b == std::string::npos) b = lines.size();
        std::string l = lines.substr(a, b - a);
        a = b + 1;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        size_t p1 = l.find('|'), p2 = p1 == std::string::npos ? p1 : l.find('|', p1 + 1), p3 = p2 == std::string::npos ? p2 : l.find('|', p2 + 1);
        if (p1 != 11 || p3 == std::string::npos) continue;
        out.push_back({l.substr(0, p1), l.substr(p2 + 1, p3 - p2 - 1), l.substr(p3 + 1), (int)atof(l.c_str() + p1 + 1)});
    }
    return out;
}

// Die Objekte des Feldes "data":[{...},{...}] einer Deezer-Antwort, jedes als eigener Text
inline std::vector<std::string> JsonObjects(const std::string& json) {
    std::vector<std::string> out;
    size_t a = json.find("\"data\":[");
    if (a == std::string::npos) return out;
    int depth = 0;
    bool str = false;
    size_t start = 0;
    for (size_t i = a + 8; i < json.size(); i++) {
        char c = json[i];
        if (str) {
            if (c == '\\') i++;
            else if (c == '"') str = false;
        } else if (c == '"') str = true;
        else if (c == '{') { if (!depth++) start = i; }
        else if (c == '}') { if (!--depth) out.push_back(json.substr(start, i - start + 1)); }
        else if (c == ']' && !depth) break;
    }
    return out;
}
