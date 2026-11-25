// QR-Code-Erzeugung (Byte-Modus, Fehlerkorrektur L oder M, Version 1–20), eigene kleine Umsetzung nach ISO/IEC 18004.
// Gedacht für kurze Texte (bis ca. 600 Byte), z. B. den Link fürs Einrichten der Handy-App.
#pragma once
#include <string>
#include <vector>

struct QrCode {
    int size = 0;     // Module pro Seite (ohne Ruherand)
    int version = 0;  // 1–20
    std::vector<char> modules;  // size*size, 1 = dunkel
    bool dark(int x, int y) const { return x >= 0 && y >= 0 && x < size && y < size && modules[(size_t)y * size + x]; }
};

// utf8: beliebiger Text als Bytes. eccMedium: false = L (mehr Platz), true = M (robuster). Liefert false, wenn der Text zu lang ist.
bool Qr_Encode(const std::string& utf8, bool eccMedium, QrCode& out);
