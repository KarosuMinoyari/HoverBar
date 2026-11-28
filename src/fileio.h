// Datei ganz lesen bzw. sicher schreiben (gemeinsam für alle Teile von HoverBar)
#pragma once
#include <windows.h>
#include <string>
#include <vector>

inline bool ReadAll(const std::wstring& f, std::vector<BYTE>& d) {
    HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    d.resize((size_t)sz.QuadPart);
    DWORD rd = 0;
    BOOL ok = d.empty() || ReadFile(h, d.data(), (DWORD)d.size(), &rd, nullptr);
    CloseHandle(h);
    return ok && rd == d.size();
}

// Erst daneben schreiben, dann ersetzen: nie eine halbe Datei (z. B. bei voller Festplatte bleibt die alte stehen)
inline bool WriteAtomic(const std::wstring& path, const void* data, size_t size) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    bool ok = size <= MAXDWORD && (!size || WriteFile(h, data, (DWORD)size, &wr, nullptr)) && wr == size;
    ok = CloseHandle(h) && ok;
    if (ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    DeleteFileW(tmp.c_str());
    return false;
}
inline bool WriteAtomic(const std::wstring& path, const std::string& data) { return WriteAtomic(path, data.data(), data.size()); }
