// Kleine Parser für Daten von außen (Spotify-CSV, Deezer-JSON). Test: tests\parse_test.cpp
#pragma once
#include <algorithm>
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

// ---- iCal (Google Kalender): Termine im Zeitraum [from, to] (jjjjmmtt), Serien aufgelöst
struct IcsEvent { int date, minutes; std::string title; };  // minutes seit 0 Uhr, -1 = ganztägig

inline long IcsDays(int date) {  // Tage seit 1970
    int y = date / 10000, m = date / 100 % 100, d = date % 100;
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400), doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}
inline int IcsDate(long z) {
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    return (int)((yoe + era * 400 + (m <= 2)) * 10000 + m * 100 + d);
}

// toLocal rechnet UTC-Zeiten ("...Z") um. shortcut: Zeiten mit TZID gelten als Ortszeit dieses PCs; bei Terminen aus anderen
// Zeitzonen um deren Unterschied daneben.
inline std::vector<IcsEvent> ParseIcs(const std::string& text, int from, int to, void (*toLocal)(int& date, int& minutes) = nullptr) {
    struct Raw { std::string uid, title, rule; int date = 0, minutes = -1, end = 0; bool utc = false, override_ = false, cancelled = false; std::vector<int> except; };
    std::vector<std::string> lines;
    for (size_t a = 0; a < text.size();) {  // Folgezeilen (beginnen mit Leerzeichen) anhängen
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        std::string l = text.substr(a, b - a);
        a = b + 1;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (!l.empty() && (l[0] == ' ' || l[0] == '\t') && !lines.empty()) lines.back() += l.substr(1);
        else lines.push_back(l);
    }
    std::vector<Raw> raw;
    Raw cur;
    bool in = false;
    int nested = 0;
    for (auto& l : lines) {
        if (l == "BEGIN:VEVENT") { cur = Raw(); in = true; nested = 0; continue; }
        if (!in) continue;
        if (l == "END:VEVENT") { in = false; if (cur.date) raw.push_back(cur); continue; }
        if (l.rfind("BEGIN:", 0) == 0) { nested++; continue; }  // z. B. VALARM
        if (l.rfind("END:", 0) == 0) { nested--; continue; }
        size_t colon = l.find(':');
        if (nested || colon == std::string::npos) continue;
        std::string name = l.substr(0, l.find_first_of(";:")), value = l.substr(colon + 1);
        int date = atoi(value.substr(0, 8).c_str());
        if (name == "SUMMARY") {
            for (size_t i = 0; i < value.size(); i++) {
                if (value[i] == '\\' && i + 1 < value.size()) { i++; cur.title += value[i] == 'n' || value[i] == 'N' ? ' ' : value[i]; }
                else cur.title += value[i];
            }
        } else if (name == "DTSTART") {
            cur.date = date;
            if (value.size() >= 13 && value[8] == 'T') cur.minutes = atoi(value.substr(9, 2).c_str()) * 60 + atoi(value.substr(11, 2).c_str());
            cur.utc = value.back() == 'Z';
        } else if (name == "DTEND") cur.end = value.size() == 8 ? date : 0;
        else if (name == "UID") cur.uid = value;
        else if (name == "RRULE") cur.rule = value;
        else if (name == "STATUS") cur.cancelled = value == "CANCELLED";
        else if (name == "RECURRENCE-ID") { cur.override_ = true; cur.except.push_back(date); }
        else if (name == "EXDATE")
            for (size_t p = 0; p < value.size(); p = value.find(',', p) == std::string::npos ? value.size() : value.find(',', p) + 1)
                cur.except.push_back(atoi(value.substr(p, 8).c_str()));
    }
    for (auto& r : raw)  // geänderte Einzeltermine ersetzen den Serientermin
        if (r.override_ && !r.except.empty())
            for (auto& m : raw)
                if (!m.override_ && m.uid == r.uid) m.except.push_back(r.except[0]);
    std::vector<IcsEvent> out;
    auto emit = [&](const Raw& r, int date) {
        int minutes = r.minutes;
        if (r.utc && minutes >= 0 && toLocal) toLocal(date, minutes);
        if (date >= from && date <= to && out.size() < 5000) out.push_back({date, minutes, r.title.empty() ? "(ohne Titel)" : r.title});
    };
    for (auto& r : raw) {
        if (r.cancelled) continue;
        auto skipped = [&](int d) { return !r.override_ && std::find(r.except.begin(), r.except.end(), d) != r.except.end(); };
        auto field = [&](const char* key) {
            std::string k = std::string(key) + "=", s = ";" + r.rule;
            size_t p = s.find(";" + k);
            if (p == std::string::npos) return std::string();
            p += k.size() + 1;
            size_t e = s.find(';', p);
            return s.substr(p, e == std::string::npos ? std::string::npos : e - p);
        };
        if (r.rule.empty() || r.override_) {
            long a = IcsDays(r.date), b = r.minutes < 0 && r.end > r.date ? IcsDays(r.end) : a + 1;  // mehrtägig: Ende zählt nicht mit
            for (long d = a; d < b && d < a + 62; d++)
                if (!skipped(IcsDate(d))) emit(r, IcsDate(d));
            continue;
        }
        std::string freq = field("FREQ"), byday = field("BYDAY");
        int interval = std::max(1, atoi(field("INTERVAL").c_str())), count = atoi(field("COUNT").c_str());
        int until = atoi(field("UNTIL").substr(0, 8).c_str());
        static const char* wd[] = {"MO", "TU", "WE", "TH", "FR", "SA", "SU"};
        auto weekday = [](long d) { return (int)(((d + 3) % 7 + 7) % 7); };  // 0 = Montag
        long start = IcsDays(r.date), last = IcsDays(to);
        int made = 0;
        for (long d = start; d <= last && d < start + 366 * 60; d++) {
            int date = IcsDate(d);
            if (until && date > until) break;
            int dy = date / 10000 - r.date / 10000, dm = dy * 12 + date / 100 % 100 - r.date / 100 % 100;
            bool hit = false;
            if (freq == "DAILY") hit = (d - start) % interval == 0;
            else if (freq == "WEEKLY") {
                bool day = byday.empty() ? weekday(d) == weekday(start) : byday.find(wd[weekday(d)]) != std::string::npos;
                hit = day && ((d - weekday(d)) - (start - weekday(start))) / 7 % interval == 0;
            } else if (freq == "MONTHLY" && dm % interval == 0) {
                if (byday.size() >= 3) {  // z. B. 2TU = zweiter Dienstag, -1FR = letzter Freitag
                    int n = atoi(byday.c_str()), dom = date % 100;
                    bool day = byday.find(wd[weekday(d)]) != std::string::npos;
                    hit = day && (n > 0 ? (dom - 1) / 7 + 1 == n : IcsDate(d + 7) / 100 != date / 100);
                } else hit = date % 100 == r.date % 100;
            } else if (freq == "YEARLY") hit = dy % interval == 0 && date % 10000 == r.date % 10000;
            if (!hit) continue;
            if (count && ++made > count) break;
            if (!skipped(date)) emit(r, date);
        }
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
