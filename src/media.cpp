// Musik-Player: Windows-Medien-Steuerung (GlobalSystemMediaTransportControls) und
// Handy-Musik per Bluetooth (AudioPlaybackConnection: das Handy spielt über die PC-Lautsprecher).
#define NOMINMAX
#include "media.h"
#include <unknwn.h>
#include <wincodec.h>
#include <shcore.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#pragma comment(lib, "version.lib")
#include <atomic>
#include <cwctype>
#include <cmath>
#include <algorithm>
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Media.Audio.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Storage.Streams.h>

using namespace winrt;
using namespace winrt::Windows::Media::Control;
using namespace winrt::Windows::Media::Audio;
using namespace winrt::Windows::Devices::Enumeration;

static HANDLE g_thread = nullptr;
static HANDLE g_wake = nullptr;
static HWND g_notify = nullptr;
static std::atomic<bool> g_open{false};
static std::atomic<int> g_cmd{MC_NONE};
static std::atomic<int> g_artPx{48};
static std::atomic<bool> g_resetArt{false};
static std::atomic<bool> g_spotifyOnly{false};  // nur die Spotify-Sitzung zeigen/steuern
static CRITICAL_SECTION g_cs;
static bool g_csInit = false;
static std::wstring g_phoneId, g_phoneName;

// Albumbild aus einem Datenstrom: füllend zuschneiden, auf Anzeigegröße skalieren, Ecken abrunden
static HBITMAP ArtFromStream(IStream* is, int px) {
    HBITMAP result = nullptr;
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return nullptr;
    IWICBitmapDecoder* dec = nullptr;
    if (SUCCEEDED(f->CreateDecoderFromStream(is, nullptr, WICDecodeMetadataCacheOnDemand, &dec))) {
        IWICBitmapFrameDecode* fr = nullptr;
        if (SUCCEEDED(dec->GetFrame(0, &fr))) {
            UINT iw = 0, ih = 0;
            fr->GetSize(&iw, &ih);
            if (iw && ih) {
                double sc = std::max((double)px / iw, (double)px / ih);
                UINT sw = std::max<UINT>(px, (UINT)std::ceil(iw * sc)), sh = std::max<UINT>(px, (UINT)std::ceil(ih * sc));
                IWICBitmapScaler* scl = nullptr;
                IWICFormatConverter* cv = nullptr;
                if (SUCCEEDED(f->CreateBitmapScaler(&scl)) && SUCCEEDED(scl->Initialize(fr, sw, sh, WICBitmapInterpolationModeHighQualityCubic)) &&
                    SUCCEEDED(f->CreateFormatConverter(&cv)) &&
                    SUCCEEDED(cv->Initialize(scl, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
                    BITMAPINFO bi = {};
                    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), px, -px, 1, 32, BI_RGB};
                    void* bits = nullptr;
                    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
                    if (bmp) {
                        WICRect rc = {(INT)(sw - px) / 2, (INT)(sh - px) / 2, px, px};
                        if (SUCCEEDED(cv->CopyPixels(&rc, px * 4, px * px * 4, (BYTE*)bits))) {
                            // runde Ecken (weich), Pixel sind schon vormultipliziert
                            float R = px * 0.18f;
                            BYTE* p = (BYTE*)bits;
                            for (int y = 0; y < px; y++)
                                for (int x = 0; x < px; x++, p += 4) {
                                    if ((x >= R && x < px - R) || (y >= R && y < px - R)) continue;
                                    float cx = x < R ? R : px - R, cy = y < R ? R : px - R;
                                    float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                                    float cov = R - std::sqrt(dx * dx + dy * dy) + 0.5f;
                                    cov = cov < 0 ? 0 : cov > 1 ? 1 : cov;
                                    for (int c = 0; c < 4; c++) p[c] = (BYTE)(p[c] * cov);
                                }
                            result = bmp;
                        } else {
                            DeleteObject(bmp);
                        }
                    }
                }
                if (cv) cv->Release();
                if (scl) scl->Release();
            }
            fr->Release();
        }
        dec->Release();
    }
    f->Release();
    return result;
}

// Anzeigename eines Programms: Dateibeschreibung (z. B. "Spotify") oder Dateiname
static std::wstring ProcessDisplayName(DWORD pid, bool& isService, std::wstring* exePath = nullptr) {
    isService = false;
    std::wstring name;
    HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!ph) { isService = true; return name; }
    wchar_t path[MAX_PATH];
    DWORD len = MAX_PATH;
    if (QueryFullProcessImageNameW(ph, 0, path, &len)) {
        std::wstring p = path;
        if (exePath) *exePath = p;
        std::wstring exe = p.substr(p.find_last_of(L'\\') + 1);
        std::wstring low = exe;
        for (auto& ch : low) ch = (wchar_t)towlower(ch);
        isService = low == L"svchost.exe" || low == L"audiodg.exe" || low == L"system";
        DWORD dummy = 0, sz = GetFileVersionInfoSizeW(path, &dummy);
        if (sz) {
            std::vector<BYTE> buf(sz);
            if (GetFileVersionInfoW(path, 0, sz, buf.data())) {
                struct LangCp { WORD lang, cp; }* tr = nullptr;
                UINT trLen = 0;
                if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &trLen) && trLen >= sizeof(LangCp)) {
                    wchar_t key[64];
                    wsprintfW(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr->lang, tr->cp);
                    wchar_t* desc = nullptr;
                    UINT dl = 0;
                    if (VerQueryValueW(buf.data(), key, (void**)&desc, &dl) && dl > 1) name = desc;
                }
            }
        }
        if (name.empty()) name = exe.substr(0, exe.find_last_of(L'.'));
    }
    CloseHandle(ph);
    return name;
}

// Ohne Medien-Sitzung: welches Programm gibt gerade hörbar Ton aus? (z. B. Handy über Bluetooth, Spiele, ältere Player)
static void DetectAudioPlayback(MediaInfo* info, bool phoneLinked, const std::wstring& phoneName) {
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&en)))) return;
    IMMDevice* dev = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev))) {
        IAudioSessionManager2* mgr = nullptr;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_INPROC_SERVER, nullptr, (void**)&mgr))) {
            IAudioSessionEnumerator* se = nullptr;
            if (SUCCEEDED(mgr->GetSessionEnumerator(&se))) {
                int count = 0;
                se->GetCount(&count);
                float best = 0;
                for (int i = 0; i < count; i++) {
                    IAudioSessionControl* ctl = nullptr;
                    if (FAILED(se->GetSession(i, &ctl))) continue;
                    IAudioSessionControl2* c2 = nullptr;
                    IAudioMeterInformation* meter = nullptr;
                    AudioSessionState st = AudioSessionStateInactive;
                    ctl->GetState(&st);
                    if (st == AudioSessionStateActive && SUCCEEDED(ctl->QueryInterface(IID_PPV_ARGS(&c2))) &&
                        c2->IsSystemSoundsSession() != S_OK && SUCCEEDED(ctl->QueryInterface(IID_PPV_ARGS(&meter)))) {
                        float peak = 0;
                        meter->GetPeakValue(&peak);
                        DWORD pid = 0;
                        c2->GetProcessId(&pid);
                        if (peak > 0.0005f && pid != GetCurrentProcessId()) {
                            bool service = false;
                            std::wstring exe;
                            std::wstring name = ProcessDisplayName(pid, service, &exe);
                            std::wstring low = name + L"|" + exe;
                            for (auto& ch : low) ch = (wchar_t)towlower(ch);
                            // keine Musik: animierte Hintergründe, Sprach-Chat/Anrufe, Spiele
                            static const wchar_t* notMusic[] = {L"wallpaper", L"lively", L"discord", L"teams", L"zoom", L"skype",
                                                                L"slack", L"whatsapp", L"telegram", L"signal", L"teamspeak", L"mumble",
                                                                L"guilded", L"steamapps", L"epic games", L"riot games", L"\\games\\",
                                                                L"ubisoft", L"battle.net", L"xboxgames", L"easyanticheat"};
                            bool ignore = false;
                            for (auto nm : notMusic)
                                if (low.find(nm) != std::wstring::npos) ignore = true;
                            // Bluetooth-Ton vom Handy hat Vorrang vor allen anderen Quellen
                            float score = ignore ? 0 : peak + (service ? 10.f : 0.f);
                            if (score <= best) { if (meter) meter->Release(); if (c2) c2->Release(); ctl->Release(); continue; }
                            peak = score;
                            PWSTR dn = nullptr;
                            if (SUCCEEDED(ctl->GetDisplayName(&dn)) && dn) {
                                if (dn[0] && dn[0] != L'@') name = dn;
                                CoTaskMemFree(dn);
                            }
                            // Bluetooth-Ton vom Handy läuft über einen Windows-Dienst
                            if (service || name.empty())
                                name = phoneLinked && !phoneName.empty() ? phoneName + L" (Bluetooth)" : L"Handy / Bluetooth-Audio";
                            best = peak;
                            info->source = name;
                        }
                    }
                    if (meter) meter->Release();
                    if (c2) c2->Release();
                    ctl->Release();
                }
                if (best > 0) {
                    info->audioOnly = true;
                    info->playing = true;
                }
                se->Release();
            }
            mgr->Release();
        }
        dev->Release();
    }
    en->Release();
}

static void PostPhoneState(bool connected, const std::wstring& name, const std::wstring& error) {
    auto* s = new PhoneState{connected, name, error};
    if (!PostMessageW(g_notify, WM_APP_PHONESTATE, 0, (LPARAM)s)) delete s;
}

static DWORD WINAPI MediaThread(LPVOID) {
    init_apartment(apartment_type::multi_threaded);
    {
        GlobalSystemMediaTransportControlsSessionManager mgr{nullptr};
        try { mgr = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get(); } catch (...) {}
        AudioPlaybackConnection conn{nullptr};
        std::wstring connName, lastKey, lastSent;

        for (;;) {
            bool open = g_open.load();
            // Handy-Verbindung beendet (z. B. am Handy getrennt)?
            if (conn) {
                try {
                    if (conn.State() == AudioPlaybackConnectionState::Closed) {
                        conn.Close();
                        conn = nullptr;
                        PostPhoneState(false, connName, L"");
                    }
                } catch (...) { conn = nullptr; PostPhoneState(false, connName, L""); }
            }
            if (!open && !conn && g_cmd.load() == MC_NONE) break;  // nichts zu tun: Thread beenden, Speicher wird frei
            if (g_resetArt.exchange(false)) { lastKey.clear(); lastSent.clear(); }

            int cmd = g_cmd.exchange(MC_NONE);
            try {
                GlobalSystemMediaTransportControlsSession session{nullptr};
                bool spotify = g_spotifyOnly.load();
                if (mgr && spotify) {
                    for (auto const& s : mgr.GetSessions()) {
                        std::wstring app = s.SourceAppUserModelId().c_str();
                        for (auto& ch : app) ch = (wchar_t)towlower(ch);
                        if (app.find(L"spotify") != std::wstring::npos) { session = s; break; }
                    }
                } else if (mgr) {
                    session = mgr.GetCurrentSession();
                }
                if (session) {
                    if (cmd == MC_PREV) session.TrySkipPreviousAsync().get();
                    else if (cmd == MC_PLAYPAUSE) session.TryTogglePlayPauseAsync().get();
                    else if (cmd == MC_NEXT) session.TrySkipNextAsync().get();
                } else if (cmd == MC_PLAYPAUSE && spotify) {
                    ShellExecuteW(nullptr, L"open", L"spotify:", nullptr, nullptr, SW_SHOWMINNOACTIVE);  // Spotify starten
                }
                if (cmd == MC_LISTPHONES) {
                    auto* list = new PhoneList();
                    try {
                        auto devs = DeviceInformation::FindAllAsync(AudioPlaybackConnection::GetDeviceSelector()).get();
                        for (auto const& d : devs) list->devices.push_back({d.Name().c_str(), d.Id().c_str()});
                    } catch (...) {}
                    if (conn) { EnterCriticalSection(&g_cs); list->connectedId = g_phoneId; LeaveCriticalSection(&g_cs); }
                    if (!PostMessageW(g_notify, WM_APP_PHONES, 0, (LPARAM)list)) delete list;
                } else if (cmd == MC_CONNECT) {
                    std::wstring id, name;
                    EnterCriticalSection(&g_cs);
                    id = g_phoneId;
                    name = g_phoneName;
                    LeaveCriticalSection(&g_cs);
                    if (conn) { conn.Close(); conn = nullptr; }
                    std::wstring err;
                    try {
                        conn = AudioPlaybackConnection::TryCreateFromId(id);
                        if (conn) {
                            conn.StateChanged([](auto&&, auto&&) { SetEvent(g_wake); });
                            conn.StartAsync().get();
                            auto r = conn.OpenAsync().get();
                            if (r.Status() != AudioPlaybackConnectionOpenResultStatus::Success) {
                                err = r.Status() == AudioPlaybackConnectionOpenResultStatus::RequestTimedOut
                                          ? L"Das Handy hat nicht geantwortet. Ist Bluetooth am Handy an?"
                                          : L"Verbindung abgelehnt. Am Handy das Koppeln erlauben.";
                                conn.Close();
                                conn = nullptr;
                            }
                        } else {
                            err = L"Dieses Gerät unterstützt kein Abspielen über den PC.";
                        }
                    } catch (...) {
                        err = L"Verbindung fehlgeschlagen.";
                        conn = nullptr;
                    }
                    connName = name;
                    PostPhoneState(conn != nullptr, name, err);
                } else if (cmd == MC_DISCONNECT) {
                    if (conn) { conn.Close(); conn = nullptr; }
                    PostPhoneState(false, connName, L"");
                }

                // aktuelle Wiedergabe melden (nur bei Änderungen)
                if (open) {
                    auto* info = new MediaInfo();
                    if (session) {
                        auto props = session.TryGetMediaPropertiesAsync().get();
                        auto pb = session.GetPlaybackInfo();
                        info->has = true;
                        info->title = props.Title().c_str();
                        info->artist = props.Artist().c_str();
                        if (info->artist.empty()) info->artist = props.AlbumArtist().c_str();
                        info->app = session.SourceAppUserModelId().c_str();
                        info->playing = pb.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                        auto ctl = pb.Controls();
                        info->canPrev = ctl.IsPreviousEnabled();
                        info->canNext = ctl.IsNextEnabled();
                        std::wstring key = info->title + L"\n" + info->artist;
                        if (key != lastKey) {
                            lastKey = key;
                            info->artChanged = true;
                            auto thumb = props.Thumbnail();
                            if (thumb) {
                                auto stream = thumb.OpenReadAsync().get();
                                IStream* is = nullptr;
                                if (SUCCEEDED(CreateStreamOverRandomAccessStream(winrt::get_unknown(stream), IID_PPV_ARGS(&is)))) {
                                    info->art = ArtFromStream(is, g_artPx.load());
                                    is->Release();
                                }
                            }
                        }
                    } else {
                        if (!lastKey.empty()) {
                            lastKey.clear();
                            info->artChanged = true;
                        }
                        if (!spotify) DetectAudioPlayback(info, conn != nullptr, connName);
                    }
                    std::wstring sent = std::to_wstring(info->has) + std::to_wstring(info->playing) + std::to_wstring(info->canPrev) +
                                        std::to_wstring(info->canNext) + std::to_wstring(info->audioOnly) + info->title + L"\n" +
                                        info->artist + L"\n" + info->source;
                    if (sent != lastSent || info->artChanged) {
                        lastSent = sent;
                        if (!PostMessageW(g_notify, WM_APP_MEDIA, 0, (LPARAM)info)) {
                            if (info->art) DeleteObject(info->art);
                            delete info;
                        }
                    } else {
                        delete info;
                    }
                }
            } catch (...) {
            }
            WaitForSingleObject(g_wake, open ? 1000 : conn ? INFINITE : 0);  // geschlossen ohne Handy: gleich beenden
        }
    }
    uninit_apartment();
    return 0;
}

static void EnsureThread() {
    if (!g_csInit) {
        InitializeCriticalSection(&g_cs);
        g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_csInit = true;
    }
    if (g_thread && WaitForSingleObject(g_thread, 0) == WAIT_OBJECT_0) {
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (!g_thread) g_thread = CreateThread(nullptr, 0, MediaThread, nullptr, 0, nullptr);
}

void Media_Open(HWND notify, int artPx) {
    g_notify = notify;
    g_artPx = artPx;
    g_resetArt = true;
    g_open = true;
    EnsureThread();
    SetEvent(g_wake);
}

void Media_Close() {
    g_open = false;
    if (g_wake) SetEvent(g_wake);
}

void Media_Command(int cmd) {
    g_cmd = cmd;  // vor dem Thread-Start setzen, sonst beendet er sich bei geschlossener Leiste sofort
    EnsureThread();
    SetEvent(g_wake);
}

void Media_SetSpotifyOnly(bool on) {
    g_spotifyOnly = on;
    g_resetArt = true;
    if (g_wake) SetEvent(g_wake);
}

void Media_ConnectPhone(const std::wstring& id, const std::wstring& name) {
    EnsureThread();
    EnterCriticalSection(&g_cs);
    g_phoneId = id;
    g_phoneName = name;
    LeaveCriticalSection(&g_cs);
    Media_Command(MC_CONNECT);
}

void Media_Shutdown() {
    g_open = false;
    Media_Command(MC_DISCONNECT);
    if (g_thread) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}
