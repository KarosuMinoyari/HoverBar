// setupHoverBar.exe: richtet HoverBar ein (ohne Administratorrechte, nur für den aktuellen Benutzer).
//   - kopiert HoverBar.exe (als Ressource eingebettet) nach %LOCALAPPDATA%\Programs\HoverBar
//   - Startmenü-Verknüpfung, Eintrag unter "Apps & Features", optional Autostart
//   - startet HoverBar
// Aufruf:  setupHoverBar.exe                  mit Oberfläche
//          setupHoverBar.exe --still          ohne Fragen
//          setupHoverBar.exe --deinstallieren entfernt alles wieder
//          --ziel <Ordner>  anderer Ordner (Testmodus: keine Verknüpfung, kein Autostart, keine Registry, kein Start)
//          --kein-autostart  --nicht-starten
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include "update.h"  // HB_VERSION

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kUninstKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\HoverBar";

struct Opts {
    bool silent = false, noAutostart = false, noLaunch = false, uninstall = false, test = false;
    std::wstring dir;
};

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}
static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

static int Box(const std::wstring& title, const std::wstring& text, TASKDIALOG_COMMON_BUTTON_FLAGS buttons, PCWSTR icon) {
    TASKDIALOGCONFIG c = {sizeof(c)};
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    c.dwCommonButtons = buttons;
    c.pszWindowTitle = L"HoverBar";
    c.pszMainInstruction = title.c_str();
    c.pszContent = text.c_str();
    c.pszMainIcon = icon;
    int btn = 0;
    TaskDialogIndirect(&c, &btn, nullptr, nullptr);
    return btn;
}

// laufende HoverBar.exe genau aus diesem Ordner beenden (andere Kopien bleiben unberührt)
static void KillRunning(const std::wstring& exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = {sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (lstrcmpiW(pe.szExeFile, L"HoverBar.exe") != 0) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t path[MAX_PATH * 2];
        DWORD n = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(h, 0, path, &n) && lstrcmpiW(path, exe.c_str()) == 0) {
            TerminateProcess(h, 0);
            WaitForSingleObject(h, 3000);
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
}

static bool ExtractExe(const std::wstring& target) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(2), RT_RCDATA);
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    DWORD size = SizeofResource(nullptr, r);
    const void* data = g ? LockResource(g) : nullptr;
    if (!data || !size) return false;
    std::wstring tmp = target + L".neu";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    bool ok = WriteFile(f, data, size, &wr, nullptr) && wr == size;
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

static std::wstring StartMenuLink() { return KnownFolder(FOLDERID_Programs) + L"\\HoverBar.lnk"; }

static void MakeShortcut(const std::wstring& exe, const std::wstring& dir) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return;
    sl->SetPath(exe.c_str());
    sl->SetWorkingDirectory(dir.c_str());
    sl->SetDescription(L"HoverBar – Leiste am Bildschirmrand");
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
        pf->Save(StartMenuLink().c_str(), TRUE);
        pf->Release();
    }
    sl->Release();
}

static void SetStr(HKEY k, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}
static void SetDword(HKEY k, const wchar_t* name, DWORD v) { RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v)); }

static void RegisterUninstall(const std::wstring& exe, const std::wstring& dir, const std::wstring& uninst) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &fa);
    SetStr(k, L"DisplayName", L"HoverBar");
    SetStr(k, L"DisplayVersion", HB_VERSION);
    SetStr(k, L"Publisher", L"KarosuMinoyari");
    SetStr(k, L"InstallLocation", dir);
    SetStr(k, L"DisplayIcon", exe);
    SetStr(k, L"UninstallString", L"\"" + uninst + L"\" --deinstallieren");
    SetStr(k, L"URLInfoAbout", L"https://github.com/KarosuMinoyari/HoverBar");
    SetDword(k, L"NoModify", 1);
    SetDword(k, L"NoRepair", 1);
    SetDword(k, L"EstimatedSize", (fa.nFileSizeLow / 1024) * 2);
    RegCloseKey(k);
}

static std::wstring ArgAfter(const std::vector<std::wstring>& a, const wchar_t* flag) {
    for (size_t i = 0; i + 1 < a.size(); i++)
        if (a[i] == flag) return a[i + 1];
    return L"";
}

static int Install(Opts o) {
    std::wstring exe = o.dir + L"\\HoverBar.exe", uninst = o.dir + L"\\Deinstallieren.exe";
    bool update = Exists(exe);
    if (!o.silent) {
        TASKDIALOG_BUTTON btns[] = {{100, update ? L"Aktualisieren" : L"Installieren"}, {IDCANCEL, L"Abbrechen"}};
        std::wstring text = std::wstring(update ? L"HoverBar wird in diesem Ordner aktualisiert:\n" : L"HoverBar wird in diesen Ordner installiert:\n") + o.dir +
                            L"\n\nEs sind keine Administratorrechte nötig. Deine Einstellungen bleiben erhalten.";
        TASKDIALOGCONFIG c = {sizeof(c)};
        c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
        c.pszWindowTitle = L"HoverBar einrichten";
        c.pszMainInstruction = update ? L"HoverBar aktualisieren" : L"HoverBar installieren";
        c.pszContent = text.c_str();
        c.pszMainIcon = TD_INFORMATION_ICON;
        c.cButtons = 2;
        c.pButtons = btns;
        c.nDefaultButton = 100;
        c.pszVerificationText = L"Mit Windows starten";
        c.dwFlags |= TDF_VERIFICATION_FLAG_CHECKED;
        int btn = 0;
        BOOL verif = TRUE;
        if (FAILED(TaskDialogIndirect(&c, &btn, nullptr, &verif)) || btn != 100) return 1;
        o.noAutostart = !verif;
    }
    KillRunning(exe);
    if (SHCreateDirectoryExW(nullptr, o.dir.c_str(), nullptr) != ERROR_SUCCESS && !Exists(o.dir)) {
        if (!o.silent) Box(L"Ordner konnte nicht angelegt werden", o.dir, TDCBF_OK_BUTTON, TD_ERROR_ICON);
        return 2;
    }
    if (!ExtractExe(exe)) {
        if (!o.silent) Box(L"HoverBar.exe konnte nicht geschrieben werden", L"Läuft HoverBar noch oder ist der Ordner schreibgeschützt?\n" + o.dir, TDCBF_OK_BUTTON, TD_ERROR_ICON);
        return 3;
    }
    if (!o.test) {
        wchar_t self[MAX_PATH * 2];
        GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
        CopyFileW(self, uninst.c_str(), FALSE);  // Deinstallieren.exe = dieses Programm
        MakeShortcut(exe, o.dir);
        RegisterUninstall(exe, o.dir, uninst);
        if (!o.noAutostart) {
            std::wstring v = L"\"" + exe + L"\"";
            RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, L"HoverBar", REG_SZ, v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
        }
    }
    if (!o.noLaunch && !o.test) ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, o.dir.c_str(), SW_SHOWNORMAL);
    if (!o.silent)
        Box(update ? L"HoverBar ist aktualisiert" : L"HoverBar ist installiert",
            L"HoverBar läuft jetzt. Halte die Maus an den rechten Bildschirmrand oder drück die Ende-Taste.\n\nDu findest es auch im Startmenü. "
            L"Neue Versionen holst du in HoverBar unter Zahnrad → Funktionen → Nach Update suchen.",
            TDCBF_OK_BUTTON, TD_INFORMATION_ICON);
    return 0;
}

static int Uninstall(Opts o) {
    std::wstring exe = o.dir + L"\\HoverBar.exe";
    bool wipe = false;
    if (!o.silent) {
        TASKDIALOGCONFIG c = {sizeof(c)};
        c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
        c.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
        c.pszWindowTitle = L"HoverBar entfernen";
        c.pszMainInstruction = L"HoverBar wirklich entfernen?";
        c.pszContent = L"Das Programm, die Startmenü-Verknüpfung und der Autostart werden gelöscht.";
        c.pszMainIcon = TD_WARNING_ICON;
        c.nDefaultButton = IDNO;
        c.pszVerificationText = L"Auch Einstellungen, Songtexte-Cache und Claude-Anmeldung löschen";
        int btn = 0;
        BOOL verif = FALSE;
        if (FAILED(TaskDialogIndirect(&c, &btn, nullptr, &verif)) || btn != IDYES) return 1;
        wipe = verif != FALSE;
    }
    KillRunning(exe);
    if (!o.test) {
        DeleteFileW(StartMenuLink().c_str());
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, L"HoverBar");
        RegDeleteKeyW(HKEY_CURRENT_USER, kUninstKey);
        STARTUPINFOW si = {sizeof(si)};
        PROCESS_INFORMATION pi = {};
        std::wstring cmd = L"schtasks.exe /delete /f /tn \"HoverBar Neustart\"";
        std::vector<wchar_t> b(cmd.begin(), cmd.end());
        b.push_back(0);
        if (CreateProcessW(nullptr, b.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        if (wipe) {
            std::wstring a = KnownFolder(FOLDERID_RoamingAppData) + L"\\HoverBar", l = KnownFolder(FOLDERID_LocalAppData) + L"\\HoverBar";
            for (auto& d : {a, l}) {
                std::wstring dd = d + L'\0';
                SHFILEOPSTRUCTW op = {};
                op.wFunc = FO_DELETE;
                op.pFrom = dd.c_str();
                op.fFlags = FOF_NO_UI | FOF_NOCONFIRMATION;
                if (Exists(d)) SHFileOperationW(&op);
            }
        }
    }
    // Ordner samt dieser Datei kurz nach dem Beenden löschen
    std::wstring cmd = L"cmd.exe /d /s /c \"ping -n 3 127.0.0.1 >nul & rmdir /s /q \"" + o.dir + L"\"\"";
    std::vector<wchar_t> b(cmd.begin(), cmd.end());
    b.push_back(0);
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, b.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    if (!o.silent) Box(L"HoverBar wurde entfernt", wipe ? L"Programm und Einstellungen sind gelöscht." : L"Deine Einstellungen blieben erhalten, falls du HoverBar später wieder installierst.", TDCBF_OK_BUTTON, TD_INFORMATION_ICON);
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argc = 0;
    LPWSTR* av = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> a;
    for (int i = 1; i < argc; i++) a.push_back(av[i]);
    LocalFree(av);
    Opts o;
    for (auto& s : a) {
        if (s == L"--still") o.silent = true;
        else if (s == L"--kein-autostart") o.noAutostart = true;
        else if (s == L"--nicht-starten") o.noLaunch = true;
        else if (s == L"--deinstallieren") o.uninstall = true;
    }
    o.dir = ArgAfter(a, L"--ziel");
    o.test = !o.dir.empty();
    if (o.dir.empty()) o.dir = KnownFolder(FOLDERID_LocalAppData) + L"\\Programs\\HoverBar";
    while (o.dir.size() > 3 && o.dir.back() == L'\\') o.dir.pop_back();
    int r = o.uninstall ? Uninstall(o) : Install(o);
    CoUninitialize();
    return r;
}
