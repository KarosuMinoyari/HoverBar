// Musik-Bibliothek, YouTube -> MP3, Cover-Suche, Wiedergabe. Siehe music.h.
#define NOMINMAX
#include "music.h"
#include <unknwn.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <urlmon.h>
#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <deque>
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Networking.Connectivity.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "shlwapi.lib")

namespace wmp = winrt::Windows::Media::Playback;
namespace wmc = winrt::Windows::Media::Core;

static HWND g_notify = nullptr;
static std::vector<Song> g_songs;
static std::vector<Playlist> g_playlists;
static bool g_loaded = false;

// ---------------------------------------------------------------- Hilfen

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}
static std::wstring DataDir() { return KnownFolder(FOLDERID_RoamingAppData) + L"\\HoverBar"; }
static std::wstring CoverDir() { return DataDir() + L"\\covers"; }
static std::wstring LibraryFile() { return DataDir() + L"\\musik.txt"; }
static std::wstring ToolPath() { return KnownFolder(FOLDERID_LocalAppData) + L"\\HoverBar\\tools\\yt-dlp.exe"; }
// Song-Ordner: Standard "Musik\HoverBar", in den Einstellungen änderbar (wird auch vom Download-Thread gelesen)
static std::wstring g_songFolder;
static SRWLOCK g_folderLock = SRWLOCK_INIT;
std::wstring Music_Folder() {
    AcquireSRWLockShared(&g_folderLock);
    std::wstring f = g_songFolder;
    ReleaseSRWLockShared(&g_folderLock);
    return f.empty() ? KnownFolder(FOLDERID_Music) + L"\\HoverBar" : f;
}
void Music_SetFolder(const std::wstring& folder) {
    AcquireSRWLockExclusive(&g_folderLock);
    g_songFolder = folder;
    ReleaseSRWLockExclusive(&g_folderLock);
}
// vorhandene Songs in den Song-Ordner verschieben; liefert die Anzahl verschobener Dateien
int Music_MoveSongsToFolder() {
    std::wstring dir = Music_Folder();
    CreateDirectoryW(dir.c_str(), nullptr);
    int moved = 0;
    for (auto& s : g_songs) {
        std::wstring name = s.file.substr(s.file.find_last_of(L'\\') + 1);
        std::wstring dst = dir + L"\\" + name;
        if (_wcsicmp(dst.c_str(), s.file.c_str()) == 0 || GetFileAttributesW(s.file.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        for (int n = 2; GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES; n++)
            dst = dir + L"\\" + name.substr(0, name.size() - 4) + L" (" + std::to_wstring(n) + L").mp3";
        if (MoveFileExW(s.file.c_str(), dst.c_str(), MOVEFILE_COPY_ALLOWED)) {  // auch auf andere Laufwerke
            s.file = dst;
            moved++;
        }
    }
    if (moved) Music_Save();
    return moved;
}

static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::wstring Clean(std::wstring s) {  // keine Tabs/Zeilenumbrüche in der Bibliotheksdatei
    for (auto& c : s)
        if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
    return s;
}
static std::wstring Trim(std::wstring s) {
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && iswspace(s[i])) i++;
    return s.substr(i);
}
static std::wstring LowerStr(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}
static std::wstring SafeFileName(std::wstring s) {
    const std::wstring bad = L"<>:\"/\\|?*";
    for (auto& c : s)
        if (bad.find(c) != std::wstring::npos || c < 32) c = L'_';
    s = Trim(s);
    while (!s.empty() && (s.back() == L'.' || s.back() == L' ')) s.pop_back();
    if (s.size() > 120) s.resize(120);
    return s.empty() ? L"Song" : s;
}
static std::wstring UrlEncode(const std::wstring& w) {
    std::string s = ToUtf8(w), out;
    char buf[4];
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else if (c == ' ') out += '+';
        else { sprintf_s(buf, "%%%02X", c); out += buf; }
    }
    return FromUtf8(out);
}
static std::wstring NewId() {
    GUID gd;
    CoCreateGuid(&gd);
    wchar_t b[40];
    wsprintfW(b, L"x%08lx%04x", gd.Data1, gd.Data2);
    return b;
}
static std::wstring YoutubeId(const std::wstring& url) {
    auto take = [&](size_t p) {
        std::wstring id;
        while (p < url.size() && (iswalnum(url[p]) || url[p] == L'-' || url[p] == L'_') && id.size() < 11) id += url[p++];
        return id.size() == 11 ? id : L"";
    };
    for (const wchar_t* k : {L"v=", L"youtu.be/", L"/shorts/", L"/embed/", L"/live/"}) {
        size_t p = url.find(k);
        if (p != std::wstring::npos) {
            std::wstring id = take(p + wcslen(k));
            if (!id.empty()) return id;
        }
    }
    return L"";
}

// Text einer Webadresse holen (für die Cover-Suche)
static std::string HttpGet(const std::wstring& url) {
    std::string out;
    IStream* st = nullptr;
    if (FAILED(URLOpenBlockingStreamW(nullptr, url.c_str(), &st, 0, nullptr))) return out;
    char buf[8192];
    ULONG got = 0;
    while (SUCCEEDED(st->Read(buf, sizeof(buf), &got)) && got > 0) {
        out.append(buf, got);
        if (out.size() > 4 * 1024 * 1024) break;
    }
    st->Release();
    return out;
}

// alle Werte eines JSON-Feldes (einfacher Textscan, reicht für die Such-APIs)
static std::vector<std::wstring> JsonValues(const std::string& json, const std::string& key) {
    std::vector<std::wstring> vals;
    std::string pat = "\"" + key + "\":\"";
    size_t p = 0;
    while ((p = json.find(pat, p)) != std::string::npos) {
        p += pat.size();
        std::string v;
        while (p < json.size() && json[p] != '"') {
            if (json[p] == '\\' && p + 1 < json.size()) { p++; if (json[p] != '/') v += '\\'; }
            v += json[p++];
        }
        vals.push_back(FromUtf8(v));
    }
    return vals;
}

// Bild laden, quadratisch zuschneiden, als JPEG speichern
static bool SaveSquareJpg(const std::wstring& src, const std::wstring& dst, int px) {
    bool ok = false;
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICBitmapScaler* scl = nullptr;
    IWICBitmapClipper* clip = nullptr;
    IWICFormatConverter* cv = nullptr;
    IWICStream* out = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fe = nullptr;
    do {
        if (FAILED(f->CreateDecoderFromFilename(src.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) break;
        if (FAILED(dec->GetFrame(0, &fr))) break;
        UINT iw = 0, ih = 0;
        fr->GetSize(&iw, &ih);
        if (!iw || !ih) break;
        double sc = std::max((double)px / iw, (double)px / ih);
        UINT sw = std::max<UINT>(px, (UINT)std::ceil(iw * sc)), sh = std::max<UINT>(px, (UINT)std::ceil(ih * sc));
        if (FAILED(f->CreateBitmapScaler(&scl)) || FAILED(scl->Initialize(fr, sw, sh, WICBitmapInterpolationModeHighQualityCubic))) break;
        WICRect rc = {(INT)(sw - px) / 2, (INT)(sh - px) / 2, px, px};
        if (FAILED(f->CreateBitmapClipper(&clip)) || FAILED(clip->Initialize(scl, &rc))) break;
        if (FAILED(f->CreateFormatConverter(&cv)) ||
            FAILED(cv->Initialize(clip, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) break;
        if (FAILED(f->CreateStream(&out)) || FAILED(out->InitializeFromFilename(dst.c_str(), GENERIC_WRITE))) break;
        if (FAILED(f->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) || FAILED(enc->Initialize(out, WICBitmapEncoderNoCache))) break;
        IPropertyBag2* bag = nullptr;
        if (FAILED(enc->CreateNewFrame(&fe, &bag))) break;
        PROPBAG2 opt = {};
        opt.pstrName = (LPOLESTR)L"ImageQuality";
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.9f;
        bag->Write(1, &opt, &v);
        if (FAILED(fe->Initialize(bag))) { bag->Release(); break; }
        bag->Release();
        UINT w = px, h = px;
        fe->SetSize(w, h);
        WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
        fe->SetPixelFormat(&pf);
        if (FAILED(fe->WriteSource(cv, nullptr)) || FAILED(fe->Commit()) || FAILED(enc->Commit())) break;
        ok = true;
    } while (false);
    if (fe) fe->Release();
    if (enc) enc->Release();
    if (out) out->Release();
    if (cv) cv->Release();
    if (clip) clip->Release();
    if (scl) scl->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    f->Release();
    if (!ok) DeleteFileW(dst.c_str());
    return ok;
}

// ID3v2.3-Tag (Titel, Künstler, Cover) vorn in die MP3 schreiben, damit auch andere Player alles sehen
static void WriteId3(const Song& s) {
    HANDLE h = CreateFileW(s.file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    std::vector<BYTE> data((size_t)sz.QuadPart);
    DWORD rd = 0;
    BOOL okRead = data.empty() || ReadFile(h, data.data(), (DWORD)data.size(), &rd, nullptr);
    CloseHandle(h);
    if (!okRead || rd != data.size()) return;
    size_t audio = 0;
    if (data.size() > 10 && data[0] == 'I' && data[1] == 'D' && data[2] == '3') {
        size_t tagSize = ((data[6] & 0x7f) << 21) | ((data[7] & 0x7f) << 14) | ((data[8] & 0x7f) << 7) | (data[9] & 0x7f);
        audio = 10 + tagSize + ((data[5] & 0x10) ? 10 : 0);
        if (audio > data.size()) audio = 0;
    }
    std::vector<BYTE> frames;
    auto frame = [&](const char* id, const std::vector<BYTE>& body) {
        frames.insert(frames.end(), id, id + 4);
        uint32_t n = (uint32_t)body.size();
        BYTE be[4] = {(BYTE)(n >> 24), (BYTE)(n >> 16), (BYTE)(n >> 8), (BYTE)n};
        frames.insert(frames.end(), be, be + 4);
        frames.push_back(0);
        frames.push_back(0);
        frames.insert(frames.end(), body.begin(), body.end());
    };
    auto text = [&](const char* id, const std::wstring& t) {
        if (t.empty()) return;
        std::vector<BYTE> b = {1, 0xFF, 0xFE};  // UTF-16 mit BOM
        for (wchar_t c : t) { b.push_back((BYTE)(c & 0xff)); b.push_back((BYTE)(c >> 8)); }
        b.push_back(0);
        b.push_back(0);
        frame(id, b);
    };
    text("TIT2", s.title);
    text("TPE1", s.artist);
    if (!s.cover.empty()) {
        HANDLE ch = CreateFileW(s.cover.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (ch != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER cs;
            GetFileSizeEx(ch, &cs);
            if (cs.QuadPart > 0 && cs.QuadPart < 8 * 1024 * 1024) {
                std::vector<BYTE> img((size_t)cs.QuadPart);
                DWORD cr = 0;
                if (ReadFile(ch, img.data(), (DWORD)img.size(), &cr, nullptr) && cr == img.size()) {
                    std::vector<BYTE> b = {0};
                    const char* mime = "image/jpeg";
                    b.insert(b.end(), mime, mime + strlen(mime) + 1);
                    b.push_back(3);  // Vorderseite
                    b.push_back(0);  // leere Beschreibung
                    b.insert(b.end(), img.begin(), img.end());
                    frame("APIC", b);
                }
            }
            CloseHandle(ch);
        }
    }
    uint32_t n = (uint32_t)frames.size();
    BYTE hdr[10] = {'I', 'D', '3', 3, 0, 0, (BYTE)((n >> 21) & 0x7f), (BYTE)((n >> 14) & 0x7f), (BYTE)((n >> 7) & 0x7f), (BYTE)(n & 0x7f)};
    std::wstring tmp = s.file + L".tmp";
    HANDLE w = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (w == INVALID_HANDLE_VALUE) return;
    DWORD wr;
    bool ok = WriteFile(w, hdr, 10, &wr, nullptr) && WriteFile(w, frames.data(), (DWORD)frames.size(), &wr, nullptr) &&
              WriteFile(w, data.data() + audio, (DWORD)(data.size() - audio), &wr, nullptr);
    CloseHandle(w);
    if (!ok || !MoveFileExW(tmp.c_str(), s.file.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
}

// ---------------------------------------------------------------- Schneiden (verlustfrei, ganze MP3-Frames)

static bool ReadAll(const std::wstring& f, std::vector<BYTE>& d) {
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

// Frames (Position, Länge) einer MP3 finden; liefert Sekunden pro Frame
static double ParseMp3(const std::vector<BYTE>& d, size_t& audio, std::vector<std::pair<size_t, size_t>>& fr) {
    static const int br1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const int br2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const int sr1[3] = {44100, 48000, 32000};
    audio = 0;
    if (d.size() > 10 && d[0] == 'I' && d[1] == 'D' && d[2] == '3') {
        size_t t = ((d[6] & 0x7f) << 21) | ((d[7] & 0x7f) << 14) | ((d[8] & 0x7f) << 7) | (d[9] & 0x7f);
        audio = 10 + t + ((d[5] & 0x10) ? 10 : 0);
        if (audio > d.size()) audio = 0;
    }
    size_t end = d.size();
    if (end >= 128 && d[end - 128] == 'T' && d[end - 127] == 'A' && d[end - 126] == 'G') end -= 128;  // ID3v1
    double spf = 0;
    size_t p = audio;
    while (p + 4 <= end) {
        BYTE b1 = d[p + 1], b2 = d[p + 2];
        int ver = (b1 >> 3) & 3, layer = (b1 >> 1) & 3, bi = b2 >> 4, si = (b2 >> 2) & 3;
        if (d[p] != 0xFF || (b1 & 0xE0) != 0xE0 || ver == 1 || layer != 1 || bi == 0 || bi == 15 || si == 3) { p++; continue; }
        bool m1 = ver == 3;
        int sr = sr1[si] / (m1 ? 1 : ver == 2 ? 2 : 4);
        int br = (m1 ? br1 : br2)[bi] * 1000;
        size_t len = (size_t)((m1 ? 144 : 72) * br / sr + ((b2 >> 1) & 1));
        if (len < 24 || p + len > end) break;
        // Xing/Info-Kopf (stimmt nach dem Schneiden nicht mehr) weglassen
        bool info = false;
        if (fr.empty())
            for (size_t k = p + 4; k + 4 < p + std::min<size_t>(len, 48); k++)
                if (!memcmp(&d[k], "Xing", 4) || !memcmp(&d[k], "Info", 4)) info = true;
        if (!info) fr.push_back({p, len});
        spf = (m1 ? 1152.0 : 576.0) / sr;
        p += len;
    }
    return spf;
}

double Music_Length(const std::wstring& id) {
    int i = Music_Find(id);
    std::vector<BYTE> d;
    if (i < 0 || !ReadAll(g_songs[i].file, d)) return 0;
    size_t audio;
    std::vector<std::pair<size_t, size_t>> fr;
    double spf = ParseMp3(d, audio, fr);  // erst parsen, dann zählen
    return spf * fr.size();
}

// Wellenform: MP3 dekodieren, je Abschnitt den lautesten Wert merken
struct WaveJob {
    std::wstring id, file;
    int buckets;
};
static DWORD WINAPI WaveThread(LPVOID p) {
    auto* job = (WaveJob*)p;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    auto* res = new WaveResult;
    res->songId = job->id;
    res->peaks.assign(job->buckets, 0.f);
    IMFSourceReader* rd = nullptr;
    IMFMediaType* pcm = nullptr;
    if (SUCCEEDED(MFCreateSourceReaderFromURL(job->file.c_str(), nullptr, &rd))) {
        rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        rd->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
        MFCreateMediaType(&pcm);
        pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        PROPVARIANT dur;
        PropVariantInit(&dur);
        LONGLONG total = 0;
        if (SUCCEEDED(rd->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &dur))) total = (LONGLONG)dur.uhVal.QuadPart;
        PropVariantClear(&dur);
        IMFMediaType* act = nullptr;
        if (total > 0 && SUCCEEDED(rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, pcm)) &&
            SUCCEEDED(rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &act))) {
            UINT32 rate = MFGetAttributeUINT32(act, MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
            UINT32 ch = std::max(1u, MFGetAttributeUINT32(act, MF_MT_AUDIO_NUM_CHANNELS, 2));
            act->Release();
            int nb = job->buckets;
            std::vector<double> sum(nb, 0.0);
            std::vector<int> cnt(nb, 0);
            for (;;) {
                DWORD fl = 0;
                LONGLONG ts = 0;
                IMFSample* s = nullptr;
                if (FAILED(rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &fl, &ts, &s))) break;
                if (s) {
                    IMFMediaBuffer* buf = nullptr;
                    if (SUCCEEDED(s->ConvertToContiguousBuffer(&buf))) {
                        BYTE* data = nullptr;
                        DWORD len = 0;
                        if (SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
                            const short* sm = (const short*)data;
                            size_t frames = len / 2 / ch;
                            for (size_t k = 0; k < frames; k += 4) {  // jeder 4. Wert reicht für die Anzeige
                                LONGLONG t = ts + (LONGLONG)(k * 10000000.0 / rate);
                                int b = (int)(t * nb / total);
                                if (b < 0 || b >= nb) continue;
                                // mittlere Lautstärke (RMS): laut gemasterte Musik hat fast überall Spitzen bei 100 %
                                double v = sm[k * ch] / 32768.0;
                                sum[b] += v * v;
                                cnt[b]++;
                            }
                            buf->Unlock();
                        }
                        buf->Release();
                    }
                    s->Release();
                }
                if (fl & MF_SOURCE_READERF_ENDOFSTREAM) break;
            }
            for (int b = 0; b < nb; b++) res->peaks[b] = cnt[b] ? (float)std::sqrt(sum[b] / cnt[b]) : 0.f;
        }
        if (pcm) pcm->Release();
        rd->Release();
    }
    MFShutdown();
    CoUninitialize();
    if (!PostMessageW(g_notify, WM_APP_MUSIC, MUSIC_WAVE, (LPARAM)res)) delete res;
    delete job;
    return 0;
}

void Music_LoadWave(const std::wstring& id, int buckets) {
    int i = Music_Find(id);
    if (i < 0 || buckets <= 0) return;
    auto* job = new WaveJob{id, g_songs[i].file, buckets};
    HANDLE t = CreateThread(nullptr, 0, WaveThread, job, 0, nullptr);
    if (t) CloseHandle(t);
    else delete job;
}

// Rückgängig: vor jedem Schnitt eine Kopie in %APPDATA%\HoverBar\undo\<id>_<n>.mp3
static std::wstring UndoFile(const std::wstring& id, int n) { return DataDir() + L"\\undo\\" + id + L"_" + std::to_wstring(n) + L".mp3"; }
int Music_UndoCount(const std::wstring& id) {
    int n = 0;
    while (GetFileAttributesW(UndoFile(id, n + 1).c_str()) != INVALID_FILE_ATTRIBUTES) n++;
    return n;
}
static void DropUndo(const std::wstring& id) {
    for (int n = Music_UndoCount(id); n > 0; n--) DeleteFileW(UndoFile(id, n).c_str());
}

bool Music_Undo(const std::wstring& id) {
    int i = Music_Find(id), n = Music_UndoCount(id);
    if (i < 0 || n == 0) return false;
    if (!MoveFileExW(UndoFile(id, n).c_str(), g_songs[i].file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) return false;
    WriteId3(g_songs[i]);  // aktuellen Titel/Cover behalten
    g_songs[i].duration = (int)(Music_Length(id) + 0.5);
    Music_Save();
    return true;
}

// keepInside: nur from..to behalten; sonst from..to herausschneiden
bool Music_Trim(const std::wstring& id, double from, double to, bool keepInside) {
    int i = Music_Find(id);
    std::vector<BYTE> d;
    if (i < 0 || !ReadAll(g_songs[i].file, d)) return false;
    size_t audio;
    std::vector<std::pair<size_t, size_t>> fr;
    double spf = ParseMp3(d, audio, fr);
    if (spf <= 0 || fr.empty()) return false;
    size_t a = std::min(fr.size(), (size_t)std::max(0.0, from / spf + 0.5)), b = std::min(fr.size(), (size_t)std::max(0.0, to / spf + 0.5));
    if (b <= a) return false;
    size_t keep = keepInside ? b - a : fr.size() - (b - a);
    if (keep < 10) return false;  // fast nichts übrig
    CreateDirectoryW((DataDir() + L"\\undo").c_str(), nullptr);
    if (!CopyFileW(g_songs[i].file.c_str(), UndoFile(id, Music_UndoCount(id) + 1).c_str(), FALSE)) return false;
    std::wstring tmp = g_songs[i].file + L".tmp";
    HANDLE w = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (w == INVALID_HANDLE_VALUE) return false;
    DWORD wr;
    bool ok = !audio || WriteFile(w, d.data(), (DWORD)audio, &wr, nullptr);  // ID3-Tag (Titel, Cover) bleibt
    for (size_t k = 0; ok && k < fr.size(); k++) {
        bool inside = k >= a && k < b;
        if (inside == keepInside) ok = WriteFile(w, d.data() + fr[k].first, (DWORD)fr[k].second, &wr, nullptr) != FALSE;
    }
    CloseHandle(w);
    if (!ok || !MoveFileExW(tmp.c_str(), g_songs[i].file.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        DeleteFileW(UndoFile(id, Music_UndoCount(id)).c_str());
        return false;
    }
    g_songs[i].duration = (int)(keep * spf + 0.5);
    Music_Save();
    return true;
}

// ---------------------------------------------------------------- Bibliothek

void Music_Init(HWND notify) {
    g_notify = notify;
    if (g_loaded) return;
    g_loaded = true;
    CreateDirectoryW(DataDir().c_str(), nullptr);
    CreateDirectoryW(CoverDir().c_str(), nullptr);
    HANDLE h = CreateFileW(LibraryFile().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    std::string data((size_t)sz.QuadPart, '\0');
    DWORD rd = 0;
    if (!data.empty()) ReadFile(h, &data[0], (DWORD)data.size(), &rd, nullptr);
    CloseHandle(h);
    std::wstring all = FromUtf8(data);
    size_t start = 0;
    while (start < all.size()) {
        size_t end = all.find(L'\n', start);
        if (end == std::wstring::npos) end = all.size();
        std::wstring line = all.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::vector<std::wstring> f;
        size_t a = 0;
        while (true) {
            size_t b = line.find(L'\t', a);
            f.push_back(line.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a));
            if (b == std::wstring::npos) break;
            a = b + 1;
        }
        if (f[0] == L"S" && f.size() >= 8) {
            Song s{f[1], f[2], f[3], f[4], f[5], f[6], _wtoi(f[7].c_str())};
            if (GetFileAttributesW(s.file.c_str()) != INVALID_FILE_ATTRIBUTES) g_songs.push_back(s);
        } else if (f[0] == L"P" && f.size() >= 2) {
            Playlist p{f[1], {}};
            for (size_t i = 2; i < f.size(); i++)
                if (!f[i].empty()) p.ids.push_back(f[i]);
            g_playlists.push_back(p);
        }
    }
}

std::vector<Song>& Music_Songs() { return g_songs; }
std::vector<Playlist>& Music_Playlists() { return g_playlists; }

int Music_Find(const std::wstring& id) {
    for (int i = 0; i < (int)g_songs.size(); i++)
        if (g_songs[i].id == id) return i;
    return -1;
}

void Music_Save() {
    std::wstring out;
    for (auto& s : g_songs)
        out += L"S\t" + Clean(s.id) + L"\t" + Clean(s.file) + L"\t" + Clean(s.title) + L"\t" + Clean(s.artist) + L"\t" +
               Clean(s.cover) + L"\t" + Clean(s.url) + L"\t" + std::to_wstring(s.duration) + L"\n";
    for (auto& p : g_playlists) {
        out += L"P\t" + Clean(p.name);
        for (auto& id : p.ids) out += L"\t" + id;
        out += L"\n";
    }
    std::string u = ToUtf8(out);
    std::wstring tmp = LibraryFile() + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr;
    WriteFile(h, u.data(), (DWORD)u.size(), &wr, nullptr);
    CloseHandle(h);
    MoveFileExW(tmp.c_str(), LibraryFile().c_str(), MOVEFILE_REPLACE_EXISTING);
}

void Music_UpdateMeta(const std::wstring& id, const std::wstring& title, const std::wstring& artist) {
    int i = Music_Find(id);
    if (i < 0) return;
    g_songs[i].title = Trim(title).empty() ? g_songs[i].title : Trim(title);
    g_songs[i].artist = Trim(artist);
    Music_Save();
    WriteId3(g_songs[i]);
}

bool Music_SetCoverFromFile(const std::wstring& id, const std::wstring& imagePath) {
    int i = Music_Find(id);
    if (i < 0) return false;
    std::wstring dst = CoverDir() + L"\\" + id + L"_" + std::to_wstring(GetTickCount64()) + L".jpg";
    if (!SaveSquareJpg(imagePath, dst, 600)) return false;
    if (!g_songs[i].cover.empty() && g_songs[i].cover.find(CoverDir()) == 0) DeleteFileW(g_songs[i].cover.c_str());
    g_songs[i].cover = dst;
    Music_Save();
    WriteId3(g_songs[i]);
    return true;
}

void Music_Delete(const std::wstring& id) {
    int i = Music_Find(id);
    if (i < 0) return;
    PlayerState ps = Player_State();
    if (ps.active && ps.songId == id) Player_Stop();
    DeleteFileW(g_songs[i].file.c_str());
    DropUndo(id);
    if (g_songs[i].cover.find(CoverDir()) == 0) DeleteFileW(g_songs[i].cover.c_str());
    g_songs.erase(g_songs.begin() + i);
    for (auto& p : g_playlists) p.ids.erase(std::remove(p.ids.begin(), p.ids.end(), id), p.ids.end());
    Music_Save();
}

// ---------------------------------------------------------------- Cover-Suche (Deezer, iTunes)

static std::vector<std::wstring> FindCoverUrls(const std::wstring& query, size_t max) {
    std::vector<std::wstring> urls;
    auto add = [&](const std::wstring& u) {
        if (!u.empty() && std::find(urls.begin(), urls.end(), u) == urls.end() && urls.size() < max) urls.push_back(u);
    };
    std::string dz = HttpGet(L"https://api.deezer.com/search?limit=8&q=" + UrlEncode(query));
    for (auto& u : JsonValues(dz, "cover_xl")) add(u);
    if (urls.size() < max) {
        std::string it = HttpGet(L"https://itunes.apple.com/search?entity=song&limit=8&term=" + UrlEncode(query));
        for (auto u : JsonValues(it, "artworkUrl100")) {
            size_t p = u.find(L"100x100");
            if (p != std::wstring::npos) u.replace(p, 7, L"600x600");
            add(u);
        }
    }
    return urls;
}

static std::wstring CleanTitle(std::wstring s);  // weiter unten (Songtexte)

// YouTube-Titel sind selten sauber ("Künstler - Titel | Synthwave | NCS (Official Video)"): erst bereinigt suchen, dann gröber, zuletzt roh
static std::vector<std::wstring> FindCoverUrlsSmart(const std::wstring& artist, const std::wstring& title, size_t max) {
    std::vector<std::wstring> queries;
    auto addQ = [&](std::wstring q) {
        while (!q.empty() && q.front() == L' ') q.erase(0, 1);
        while (!q.empty() && q.back() == L' ') q.pop_back();
        if (q.size() >= 2 && std::find(queries.begin(), queries.end(), q) == queries.end()) queries.push_back(q);
    };
    std::wstring t = CleanTitle(title), a = artist;
    size_t dash = t.find(L" - ");
    if (dash != std::wstring::npos) {  // "Künstler - Titel": beide Teile nutzen
        addQ(t.substr(0, dash) + L" " + CleanTitle(t.substr(dash + 3)));
        addQ(CleanTitle(t.substr(dash + 3)) + L" " + t.substr(0, dash));
    }
    size_t sep = a.find_first_of(L",&");
    std::wstring firstArtist = sep == std::wstring::npos ? a : a.substr(0, sep);
    addQ(firstArtist + L" " + t);
    addQ(a + L" " + t);
    addQ(t);
    addQ(a + L" " + title);
    std::vector<std::wstring> out;
    int tries = 0;
    for (auto& q : queries) {
        if (++tries > 5) break;
        for (auto& u : FindCoverUrls(q, max))
            if (std::find(out.begin(), out.end(), u) == out.end() && out.size() < max) out.push_back(u);
        if (!out.empty()) break;
    }
    return out;
}

static std::wstring TempDir() {
    wchar_t t[MAX_PATH];
    GetTempPathW(MAX_PATH, t);
    std::wstring d = std::wstring(t) + L"HoverBarMusik";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

struct CoverSearchParams { std::wstring id, artist, title; };

static DWORD WINAPI CoverSearchThread(LPVOID p) {
    auto* prm = (CoverSearchParams*)p;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto* res = new CoverResults();
    res->songId = prm->id;
    auto urls = FindCoverUrlsSmart(prm->artist, prm->title, 8);
    int n = 0;
    for (auto& u : urls) {
        std::wstring file = TempDir() + L"\\suche_" + std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(n++) + L".jpg";
        if (SUCCEEDED(URLDownloadToFileW(nullptr, u.c_str(), file.c_str(), 0, nullptr))) res->files.push_back(file);
    }
    if (!PostMessageW(g_notify, WM_APP_MUSIC, MUSIC_COVERS, (LPARAM)res)) delete res;
    CoUninitialize();
    delete prm;
    return 0;
}

void Music_SearchCovers(const std::wstring& id, const std::wstring& artist, const std::wstring& title) {
    auto* p = new CoverSearchParams{id, artist, title};
    HANDLE t = CreateThread(nullptr, 0, CoverSearchThread, p, 0, nullptr);
    if (t) CloseHandle(t);
    else delete p;
}

// ---------------------------------------------------------------- YouTube -> MP3

static CRITICAL_SECTION g_jobCs;
static bool g_jobCsInit = false;
static std::vector<DownloadJob> g_jobs;
static HANDLE g_jobThread = nullptr;
static HANDLE g_jobWake = nullptr;

static void InitJobs() {
    if (g_jobCsInit) return;
    InitializeCriticalSection(&g_jobCs);
    g_jobWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_jobCsInit = true;
}

static void UpdateJob(size_t i, int stage, int percent, const std::wstring& title = L"", const std::wstring& err = L"") {
    EnterCriticalSection(&g_jobCs);
    if (i < g_jobs.size()) {
        g_jobs[i].stage = stage;
        if (percent >= 0) g_jobs[i].percent = percent;
        if (!title.empty()) g_jobs[i].title = title;
        if (!err.empty()) g_jobs[i].error = err;
    }
    LeaveCriticalSection(&g_jobCs);
    PostMessageW(g_notify, WM_APP_MUSIC, MUSIC_JOBS, 0);
}

// AAC/Opus-Tonspur mit Media Foundation in MP3 (192 kbit/s) umwandeln
static int g_convStep = 0; static HRESULT g_convHr = S_OK;
static bool ConvertToMp3(const std::wstring& in, const std::wstring& out, size_t job) {
    bool ok = false;
    IMFSourceReader* rd = nullptr;
    IMFSinkWriter* wr = nullptr;
    IMFMediaType *pcm = nullptr, *actual = nullptr, *mp3 = nullptr;
    IMFAttributes* attr = nullptr;
    do {
        if (FAILED(g_convHr = (MFCreateSourceReaderFromURL(in.c_str(), nullptr, &rd)))) { g_convStep = 1; break; }
        rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        rd->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
        MFCreateMediaType(&pcm);
        pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        if (FAILED(g_convHr = (rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, pcm)))) { g_convStep = 2; break; }
        if (FAILED(g_convHr = (rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual)))) { g_convStep = 3; break; }
        UINT32 rate = MFGetAttributeUINT32(actual, MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
        UINT32 ch = MFGetAttributeUINT32(actual, MF_MT_AUDIO_NUM_CHANNELS, 2);
        if (rate != 44100 && rate != 48000) {  // der MP3-Encoder kann nur 44,1/48 kHz
            pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
            pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch > 2 ? 2 : ch);
            pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, (ch > 2 ? 2 : ch) * 2);
            pcm->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * (ch > 2 ? 2 : ch) * 2);
            if (FAILED(g_convHr = (rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, pcm)))) { g_convStep = 4; break; }
            actual->Release();
            actual = nullptr;
            if (FAILED(g_convHr = (rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual)))) { g_convStep = 5; break; }
            rate = MFGetAttributeUINT32(actual, MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
            ch = MFGetAttributeUINT32(actual, MF_MT_AUDIO_NUM_CHANNELS, 2);
        }
        PROPVARIANT dur;
        PropVariantInit(&dur);
        LONGLONG total = 0;
        if (SUCCEEDED(rd->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &dur))) total = (LONGLONG)dur.uhVal.QuadPart;
        PropVariantClear(&dur);

        MFCreateAttributes(&attr, 1);
        attr->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MP3);
        if (FAILED(g_convHr = (MFCreateSinkWriterFromURL(out.c_str(), nullptr, attr, &wr)))) { g_convStep = 6; break; }
        MFCreateMediaType(&mp3);
        mp3->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        mp3->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_MP3);
        mp3->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        mp3->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
        mp3->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);  // 192 kbit/s
        DWORD si = 0;
        if (FAILED(g_convHr = (wr->AddStream(mp3, &si)))) { g_convStep = 7; break; }
        if (FAILED(g_convHr = (wr->SetInputMediaType(si, actual, nullptr)))) { g_convStep = 8; break; }
        if (FAILED(g_convHr = (wr->BeginWriting()))) { g_convStep = 9; break; }
        bool fail = false;
        int lastPct = -1;
        for (;;) {
            DWORD flags = 0;
            LONGLONG ts = 0;
            IMFSample* smp = nullptr;
            if (FAILED(g_convHr = rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &ts, &smp))) { g_convStep = 90; fail = true; break; }
            if (smp) {
                if (FAILED(g_convHr = wr->WriteSample(si, smp))) { g_convStep = 91; fail = true; }
                smp->Release();
                if (fail) break;
                if (total > 0) {
                    int pct = (int)(ts * 100 / total);
                    if (pct != lastPct && pct % 5 == 0) { lastPct = pct; UpdateJob(job, 2, pct); }
                }
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        }
        if (fail || FAILED(g_convHr = wr->Finalize())) { if (!fail) g_convStep = 92; break; }
        ok = true;
    } while (false);
    if (mp3) mp3->Release();
    if (attr) attr->Release();
    if (wr) wr->Release();
    if (actual) actual->Release();
    if (pcm) pcm->Release();
    if (rd) rd->Release();
    if (!ok) DeleteFileW(out.c_str());
    return ok;
}

// "Künstler - Titel (Official Video)" -> Künstler, Titel
static void GuessArtistTitle(const std::wstring& videoTitle, const std::wstring& uploader, const std::wstring& ytArtist,
                             const std::wstring& ytTrack, std::wstring& artist, std::wstring& title) {
    if (!ytTrack.empty() && ytTrack != L"NA") {
        title = ytTrack;
        artist = ytArtist != L"NA" ? ytArtist : L"";
    } else {
        std::wstring t = videoTitle;
        // Zusätze wie (Official Video), [Lyrics] entfernen
        for (int pass = 0; pass < 4; pass++) {
            for (wchar_t open : {L'(', L'['}) {
                wchar_t close = open == L'(' ? L')' : L']';
                size_t a = t.find(open);
                while (a != std::wstring::npos) {
                    size_t b = t.find(close, a);
                    if (b == std::wstring::npos) break;
                    std::wstring inner = LowerStr(t.substr(a, b - a + 1));
                    bool junk = false;
                    for (const wchar_t* k : {L"official", L"video", L"lyric", L"audio", L"visualizer", L"hd", L"4k", L"mv", L"clip"})
                        if (inner.find(k) != std::wstring::npos) junk = true;
                    if (junk) { t.erase(a, b - a + 1); a = t.find(open, a); }
                    else a = t.find(open, b);
                }
            }
        }
        t = Trim(t);
        size_t dash = std::wstring::npos;
        for (const wchar_t* sep : {L" - ", L" – ", L" — ", L" | "}) {
            dash = t.find(sep);
            if (dash != std::wstring::npos) {
                artist = Trim(t.substr(0, dash));
                title = Trim(t.substr(dash + wcslen(sep)));
                break;
            }
        }
        if (dash == std::wstring::npos) {
            title = t;
            artist = uploader;
            for (const wchar_t* suf : {L" - Topic", L"VEVO", L"Official"}) {
                size_t p = artist.find(suf);
                if (p != std::wstring::npos) artist.erase(p);
            }
            artist = Trim(artist);
        }
    }
    if (title.empty()) title = videoTitle;
}

static DWORD WINAPI JobThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    for (;;) {
        // nächsten wartenden Auftrag holen
        size_t idx = (size_t)-1;
        std::wstring url;
        EnterCriticalSection(&g_jobCs);
        for (size_t i = 0; i < g_jobs.size(); i++)
            if (g_jobs[i].stage == 0) { idx = i; url = g_jobs[i].url; break; }
        LeaveCriticalSection(&g_jobCs);
        if (idx == (size_t)-1) {
            if (WaitForSingleObject(g_jobWake, 60000) == WAIT_TIMEOUT) break;  // eine Minute nichts zu tun: Thread beenden
            continue;
        }
        UpdateJob(idx, 1, 0);
        // manche m4a-Dateien von YouTube kann Windows nicht lesen (0 Samples) -> zweiter Versuch mit WebM/Opus
        int attempt = 0;
    retry:
        // 1. Tonspur mit yt-dlp laden
        std::wstring tmp = TempDir();
        std::wstring fmt = attempt == 0 ? L"bestaudio[ext=m4a]/bestaudio[acodec^=mp4a]/bestaudio"
                                        : L"bestaudio[ext=webm]/bestaudio[acodec=opus]/bestaudio";
        std::wstring cmd = L"\"" + ToolPath() + L"\" --no-playlist --no-warnings --newline --no-part --no-mtime "
                           L"-f \"" + fmt + L"\" "
                           L"-o \"" + tmp + L"\\%(id)s.%(ext)s\" "
                           L"--print \"before_dl:META|%(id)s|%(title)s|%(uploader)s|%(artist)s|%(track)s|%(duration)s\" "
                           L"--print \"after_move:FILE|%(filepath)s\" "
                           L"--progress-template \"download:PROG|%(progress._percent_str)s\" --no-simulate \"" + url + L"\"";
        SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
        HANDLE rdPipe = nullptr, wrPipe = nullptr;
        CreatePipe(&rdPipe, &wrPipe, &sa, 0);
        SetHandleInformation(rdPipe, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si = {sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = wrPipe;
        si.hStdError = wrPipe;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi = {};
        SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
        SetEnvironmentVariableW(L"PYTHONUTF8", L"1");
        std::vector<wchar_t> cl(cmd.begin(), cmd.end());
        cl.push_back(0);
        bool started = CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                                      nullptr, nullptr, &si, &pi) != FALSE;
        CloseHandle(wrPipe);
        std::wstring id, vtitle, uploader, ytArtist, ytTrack, file, lastLines;
        int duration = 0;
        if (started) {
            std::string buf;
            char chunk[4096];
            DWORD got = 0;
            while (ReadFile(rdPipe, chunk, sizeof(chunk), &got, nullptr) && got) {
                buf.append(chunk, got);
                size_t nl;
                while ((nl = buf.find('\n')) != std::string::npos) {
                    std::wstring line = FromUtf8(buf.substr(0, nl));
                    buf.erase(0, nl + 1);
                    if (!line.empty() && line.back() == L'\r') line.pop_back();
                    if (line.rfind(L"META|", 0) == 0) {
                        std::vector<std::wstring> f;
                        size_t a = 5;
                        while (true) {
                            size_t b = line.find(L'|', a);
                            f.push_back(line.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a));
                            if (b == std::wstring::npos) break;
                            a = b + 1;
                        }
                        // Titel kann selbst "|" enthalten: von hinten zuordnen
                        if (f.size() >= 6) {
                            id = f[0];
                            duration = _wtoi(f.back().c_str());
                            ytTrack = f[f.size() - 2];
                            ytArtist = f[f.size() - 3];
                            uploader = f[f.size() - 4];
                            vtitle.clear();
                            for (size_t k = 1; k + 4 < f.size(); k++) vtitle += (k > 1 ? L"|" : L"") + f[k];
                            UpdateJob(idx, 1, -1, vtitle);
                        }
                    } else if (line.rfind(L"PROG|", 0) == 0) {
                        UpdateJob(idx, 1, (int)_wtof(Trim(line.substr(5)).c_str()));
                    } else if (line.rfind(L"FILE|", 0) == 0) {
                        file = line.substr(5);
                    } else if (!line.empty()) {
                        lastLines = line;
                    }
                }
            }
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        CloseHandle(rdPipe);
        if (!started || file.empty() || GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::wstring err = !started ? L"yt-dlp fehlt" : lastLines.find(L"ERROR") != std::wstring::npos ? lastLines : L"Download fehlgeschlagen";
            UpdateJob(idx, -1, -1, L"", err);
            // Grund festhalten: %APPDATA%\HoverBar\musik-fehler.log
            std::wstring lf = KnownFolder(FOLDERID_RoamingAppData) + L"\\HoverBar\\musik-fehler.log";
            if (FILE* fp = _wfopen(lf.c_str(), L"a, ccs=UTF-8")) {
                fwprintf(fp, L"%s\n  %s\n", url.c_str(), lastLines.c_str());
                fclose(fp);
            }
            continue;
        }

        // 2. in MP3 umwandeln
        Song s;
        s.id = id.empty() ? NewId() : id;
        s.url = url;
        s.duration = duration;
        GuessArtistTitle(vtitle, uploader, ytArtist, ytTrack, s.artist, s.title);
        CreateDirectoryW(Music_Folder().c_str(), nullptr);
        std::wstring base = Music_Folder() + L"\\" + SafeFileName(s.artist.empty() ? s.title : s.artist + L" - " + s.title);
        s.file = base + L".mp3";
        for (int n = 2; GetFileAttributesW(s.file.c_str()) != INVALID_FILE_ATTRIBUTES; n++) s.file = base + L" (" + std::to_wstring(n) + L").mp3";
        UpdateJob(idx, 2, 0, s.artist.empty() ? s.title : s.artist + L" – " + s.title);
        bool conv = ConvertToMp3(file, s.file, idx);
        DeleteFileW(file.c_str());
        if (!conv) {
            DeleteFileW(s.file.c_str());  // halbe MP3 weg
            if (attempt == 0) {
                attempt = 1;
                UpdateJob(idx, 1, 0);
                goto retry;
            }
            UpdateJob(idx, -1, -1, L"", L"Umwandeln in MP3 fehlgeschlagen");
            std::wstring lf = KnownFolder(FOLDERID_RoamingAppData) + L"\\HoverBar\\musik-fehler.log";
            if (FILE* fp = _wfopen(lf.c_str(), L"a, ccs=UTF-8")) {
                fwprintf(fp, L"%s\n  MP3-Umwandlung fehlgeschlagen, Schritt %d, 0x%08X\n", url.c_str(), g_convStep, (unsigned)g_convHr);
                fclose(fp);
            }
            continue;
        }

        // 3. Cover: erst Musik-Datenbanken, sonst Vorschaubild des Videos
        UpdateJob(idx, 3, 100);
        std::wstring coverTmp = TempDir() + L"\\cover_" + s.id + L".jpg";
        bool haveCover = false;
        for (auto& u : FindCoverUrlsSmart(s.artist, s.title, 1))
            if (SUCCEEDED(URLDownloadToFileW(nullptr, u.c_str(), coverTmp.c_str(), 0, nullptr))) haveCover = true;
        if (!haveCover && !id.empty()) {
            for (const wchar_t* q : {L"maxresdefault", L"hqdefault"}) {
                std::wstring u = L"https://i.ytimg.com/vi/" + id + L"/" + q + L".jpg";
                if (SUCCEEDED(URLDownloadToFileW(nullptr, u.c_str(), coverTmp.c_str(), 0, nullptr))) { haveCover = true; break; }
            }
        }
        if (haveCover) {
            std::wstring dst = CoverDir() + L"\\" + s.id + L"_" + std::to_wstring(GetTickCount64()) + L".jpg";
            if (SaveSquareJpg(coverTmp, dst, 600)) s.cover = dst;
            DeleteFileW(coverTmp.c_str());
        }
        WriteId3(s);
        UpdateJob(idx, 4, 100);
        auto* song = new Song(s);
        if (!PostMessageW(g_notify, WM_APP_MUSIC, MUSIC_SONG, (LPARAM)song)) delete song;
    }
    MFShutdown();
    CoUninitialize();
    return 0;
}

bool Music_ToolAvailable() { return GetFileAttributesW(ToolPath().c_str()) != INVALID_FILE_ATTRIBUTES; }

void Music_AddUrl(const std::wstring& url) {
    InitJobs();
    DownloadJob j;
    j.url = Trim(url);
    j.title = j.url;
    EnterCriticalSection(&g_jobCs);
    g_jobs.push_back(j);
    LeaveCriticalSection(&g_jobCs);
    if (g_jobThread && WaitForSingleObject(g_jobThread, 0) == WAIT_OBJECT_0) {
        CloseHandle(g_jobThread);
        g_jobThread = nullptr;
    }
    if (!g_jobThread) g_jobThread = CreateThread(nullptr, 0, JobThread, nullptr, 0, nullptr);
    SetEvent(g_jobWake);
    PostMessageW(g_notify, WM_APP_MUSIC, MUSIC_JOBS, 0);
}

std::vector<DownloadJob> Music_Jobs() {
    if (!g_jobCsInit) return {};
    EnterCriticalSection(&g_jobCs);
    auto copy = g_jobs;
    LeaveCriticalSection(&g_jobCs);
    return copy;
}

void Music_ClearFinishedJobs() {
    if (!g_jobCsInit) return;
    EnterCriticalSection(&g_jobCs);
    // nur ganz vorne abgeschlossene entfernen, damit laufende Indizes gültig bleiben
    while (!g_jobs.empty() && (g_jobs.front().stage == 4 || g_jobs.front().stage == -1)) {
        bool anyRunning = false;
        for (auto& j : g_jobs)
            if (j.stage >= 0 && j.stage < 4) anyRunning = true;
        if (anyRunning) break;
        g_jobs.erase(g_jobs.begin());
    }
    LeaveCriticalSection(&g_jobCs);
}

// ---------------------------------------------------------------- Wiedergabe (Windows MediaPlayer)

static wmp::MediaPlayer g_player{nullptr};
static wmp::MediaPlaybackList g_list{nullptr};
static std::vector<std::wstring> g_listIds;
static double g_volume = 0.8;

static void ApplyPlayModes();  // Zufall/Wiederholen auf Player und Liste anwenden
static double g_duck = 1.0;  // < 1, solange andere Medien laufen

void Player_SetVolume(double v) {
    g_volume = std::max(0.0, std::min(1.0, v));
    try { if (g_player) g_player.Volume(g_volume * g_duck); } catch (...) {}
}

void Player_SetDuck(double f) {
    f = std::max(0.0, std::min(1.0, f));
    if (std::fabs(f - g_duck) < 0.001) return;
    g_duck = f;
    try { if (g_player) g_player.Volume(g_volume * g_duck); } catch (...) {}
}

// Spielt gerade ein Browser oder Video-Player hörbar Ton? (Spiele, Discord usw. zählen nicht)
bool Music_OtherMediaPlaying() {
    static const wchar_t* media[] = {L"chrome.exe", L"msedge.exe", L"brave.exe", L"firefox.exe", L"opera.exe", L"opera_gx.exe",
                                     L"arc.exe", L"vivaldi.exe", L"vlc.exe", L"mpc-hc64.exe", L"mpc-hc.exe", L"mpc-be64.exe",
                                     L"wmplayer.exe", L"Microsoft.Media.Player.exe", L"Video.UI.exe", L"potplayermini64.exe",
                                     L"mpv.exe", L"Spotify.exe", L"Netflix.exe", L"PrimeVideo.exe", L"Disney+.exe"};
    bool found = false;
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioSessionManager2* mgr = nullptr;
    IAudioSessionEnumerator* se = nullptr;
    DWORD self = GetCurrentProcessId();
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
        SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev)) &&
        SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr)) &&
        SUCCEEDED(mgr->GetSessionEnumerator(&se))) {
        int n = 0;
        se->GetCount(&n);
        for (int k = 0; k < n && !found; k++) {
            IAudioSessionControl* sc = nullptr;
            if (FAILED(se->GetSession(k, &sc)) || !sc) continue;
            IAudioSessionControl2* sc2 = nullptr;
            IAudioMeterInformation* mi = nullptr;
            AudioSessionState st;
            DWORD pid = 0;
            float peak = 0;
            if (SUCCEEDED(sc->GetState(&st)) && st == AudioSessionStateActive &&
                SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc2))) && SUCCEEDED(sc2->GetProcessId(&pid)) && pid && pid != self &&
                SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&mi))) && SUCCEEDED(mi->GetPeakValue(&peak)) && peak > 0.005f) {
                if (HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                    wchar_t path[MAX_PATH];
                    DWORD len = MAX_PATH;
                    if (QueryFullProcessImageNameW(ph, 0, path, &len)) {
                        const wchar_t* name = PathFindFileNameW(path);
                        for (auto m : media)
                            if (!_wcsicmp(name, m)) found = true;
                    }
                    CloseHandle(ph);
                }
            }
            if (mi) mi->Release();
            if (sc2) sc2->Release();
            sc->Release();
        }
    }
    if (se) se->Release();
    if (mgr) mgr->Release();
    if (dev) dev->Release();
    if (en) en->Release();
    return found;
}

static std::wstring FileUri(const std::wstring& path) {
    wchar_t buf[2048];
    DWORD n = 2048;
    if (SUCCEEDED(UrlCreateFromPathW(path.c_str(), buf, &n, 0))) return buf;
    return L"file:///" + path;
}

void Player_Play(const std::vector<std::wstring>& ids, int start) {
    try {
        if (!g_player) {
            g_player = wmp::MediaPlayer();
            g_player.AudioCategory(wmp::MediaPlayerAudioCategory::Media);
            g_player.Volume(g_volume * g_duck);
        }
        g_list = wmp::MediaPlaybackList();
        g_listIds.clear();
        for (auto& id : ids) {
            int i = Music_Find(id);
            if (i < 0) continue;
            const Song& s = g_songs[i];
            auto src = wmc::MediaSource::CreateFromUri(winrt::Windows::Foundation::Uri(FileUri(s.file)));
            wmp::MediaPlaybackItem item(src);
            try {
                auto props = item.GetDisplayProperties();
                props.Type(winrt::Windows::Media::MediaPlaybackType::Music);
                props.MusicProperties().Title(s.title);
                props.MusicProperties().Artist(s.artist);
                if (!s.cover.empty())
                    props.Thumbnail(winrt::Windows::Storage::Streams::RandomAccessStreamReference::CreateFromUri(
                        winrt::Windows::Foundation::Uri(FileUri(s.cover))));
                item.ApplyDisplayProperties(props);
            } catch (...) {
            }
            g_list.Items().Append(item);
            g_listIds.push_back(id);
        }
        if (g_listIds.empty()) return;
        start = std::max(0, std::min(start, (int)g_listIds.size() - 1));
        ApplyPlayModes();
        g_player.Source(g_list);
        if (start > 0) g_list.MoveTo((uint32_t)start);
        g_player.Play();
    } catch (...) {
    }
}

// Netzwerk für die Kopfzeile: 0 nichts, 1 WLAN, 2 LAN, 3 Mobilfunk; bars 0..4
void Net_Status(int& kind, int& bars, bool& internet, std::wstring& name) {
    kind = 0;
    bars = 0;
    internet = false;
    name.clear();
    try {
        using namespace winrt::Windows::Networking::Connectivity;
        auto p = NetworkInformation::GetInternetConnectionProfile();
        if (!p) return;
        internet = p.GetNetworkConnectivityLevel() == NetworkConnectivityLevel::InternetAccess;
        try { name = p.ProfileName().c_str(); } catch (...) {}
        if (p.IsWlanConnectionProfile()) kind = 1;
        else if (p.IsWwanConnectionProfile()) kind = 3;
        else kind = 2;
        if (kind != 2) {
            auto sb = p.GetSignalBars();  // 0..5
            bars = sb ? std::min(4, (int)sb.Value() * 4 / 5 + (sb.Value() > 0 ? 1 : 0)) : 4;
        }
    } catch (...) {
    }
}

// Zufall und Wiederholen: 0 = aus, 1 = Song wiederholen, 2 = Playlist wiederholen
static bool g_shuffle = false;
static int g_repeat = 0;
static void ApplyPlayModes() {
    try {
        if (g_list) {
            g_list.ShuffleEnabled(g_shuffle);
            g_list.AutoRepeatEnabled(g_repeat == 2);
        }
        if (g_player) g_player.IsLoopingEnabled(g_repeat == 1);  // aktuellen Song endlos
    } catch (...) {
    }
}
void Player_SetModes(bool shuffle, int repeat) {
    g_shuffle = shuffle;
    g_repeat = std::max(0, std::min(2, repeat));
    ApplyPlayModes();
}

// Probehören eines Ausschnitts (from..to Sekunden)
void Player_Preview(const std::wstring& id, double from, double to) {
    try {
        int i = Music_Find(id);
        if (i < 0) return;
        if (!g_player) {
            g_player = wmp::MediaPlayer();
            g_player.AudioCategory(wmp::MediaPlayerAudioCategory::Media);
            g_player.Volume(g_volume * g_duck);
        }
        auto src = wmc::MediaSource::CreateFromUri(winrt::Windows::Foundation::Uri(FileUri(g_songs[i].file)));
        winrt::Windows::Foundation::TimeSpan st((long long)(std::max(0.0, from) * 1e7));
        winrt::Windows::Foundation::TimeSpan len((long long)(std::max(0.5, to - from) * 1e7));
        wmp::MediaPlaybackItem item(src, st, len);
        g_list = wmp::MediaPlaybackList();
        g_list.Items().Append(item);
        g_listIds.assign(1, id);
        g_player.IsLoopingEnabled(false);  // Probehören nie wiederholen
        g_player.Source(g_list);
        g_player.Play();
    } catch (...) {
    }
}

void Player_Toggle() {
    if (!g_player) return;
    try {
        if (g_player.PlaybackSession().PlaybackState() == wmp::MediaPlaybackState::Playing) g_player.Pause();
        else g_player.Play();
    } catch (...) {
    }
}

void Player_Next() {
    try { if (g_list) g_list.MoveNext(); } catch (...) {}
}

void Player_Prev() {
    try {
        if (!g_player || !g_list) return;
        // wie bei jedem Player: nach 3 Sekunden springt "Zurück" an den Anfang des Songs
        if (g_player.PlaybackSession().Position().count() > 30000000) g_player.PlaybackSession().Position({});
        else g_list.MovePrevious();
    } catch (...) {
    }
}

void Player_Seek(double fraction) {
    try {
        if (!g_player) return;
        auto s = g_player.PlaybackSession();
        auto d = s.NaturalDuration();
        s.Position(winrt::Windows::Foundation::TimeSpan((long long)(d.count() * std::max(0.0, std::min(1.0, fraction)))));
    } catch (...) {
    }
}

void Player_Stop() {
    try {
        if (g_player) g_player.Close();
    } catch (...) {
    }
    g_player = nullptr;
    g_list = nullptr;
    g_listIds.clear();
}

PlayerState Player_State() {
    PlayerState st;
    if (!g_player) return st;
    try {
        auto s = g_player.PlaybackSession();
        st.active = true;
        st.playing = s.PlaybackState() == wmp::MediaPlaybackState::Playing || s.PlaybackState() == wmp::MediaPlaybackState::Buffering ||
                     s.PlaybackState() == wmp::MediaPlaybackState::Opening;
        st.pos = s.Position().count() / 1e7;
        st.dur = s.NaturalDuration().count() / 1e7;
        if (g_list) {
            uint32_t i = g_list.CurrentItemIndex();
            if (i < g_listIds.size()) st.songId = g_listIds[i];
        }
    } catch (...) {
    }
    return st;
}

// ---------------------------------------------------------------- yt-dlp herunterladen / aktualisieren
// Fragt GitHub nach der neuesten Version, lädt yt-dlp.exe herunter, prüft die SHA-256-Prüfsumme aus der
// offiziellen Liste (SHA2-256SUMS) und ersetzt erst dann die alte Datei.

#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

static CRITICAL_SECTION g_toolCs;
static bool g_toolCsInit = false;
static ToolStatus g_tool;
static bool g_toolBusy = false;

static void ToolSet(HWND notify, int state, const std::wstring& msg, int percent = -1) {
    EnterCriticalSection(&g_toolCs);
    g_tool.state = state;
    if (!msg.empty() || state == TOOL_IDLE) g_tool.msg = msg;
    g_tool.percent = percent;
    LeaveCriticalSection(&g_toolCs);
    PostMessageW(notify, WM_APP_MUSIC, MUSIC_TOOL, 0);
}

ToolStatus Music_ToolStatus() {
    if (!g_toolCsInit) return ToolStatus();
    EnterCriticalSection(&g_toolCs);
    ToolStatus s = g_tool;
    LeaveCriticalSection(&g_toolCs);
    return s;
}

// Ausgabe eines Programms (ohne Fenster) einlesen
static std::string RunCapture(const std::wstring& cmd) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return "";
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> cl(cmd.begin(), cmd.end());
    cl.push_back(0);
    std::string out;
    if (CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(wr);
        wr = nullptr;
        char buf[512];
        DWORD got;
        while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    if (wr) CloseHandle(wr);
    CloseHandle(rd);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return out;
}

static std::wstring LocalToolVersion() {
    if (GetFileAttributesW(ToolPath().c_str()) == INVALID_FILE_ATTRIBUTES) return L"";
    std::string v = RunCapture(L"\"" + ToolPath() + L"\" --version");
    return v.size() < 40 ? FromUtf8(v) : L"";
}

// Fortschritt beim Herunterladen
struct DlProgress : public IBindStatusCallback {
    HWND notify;
    ULONG ref = 1;
    explicit DlProgress(HWND n) : notify(n) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** p) override {
        if (r == IID_IUnknown || r == IID_IBindStatusCallback) { *p = this; AddRef(); return S_OK; }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
    ULONG STDMETHODCALLTYPE Release() override { return --ref; }
    HRESULT STDMETHODCALLTYPE OnStartBinding(DWORD, IBinding*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPriority(LONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnLowResource(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnProgress(ULONG done, ULONG total, ULONG, LPCWSTR) override {
        if (total > 0) ToolSet(notify, TOOL_DOWNLOADING, L"", (int)((unsigned long long)done * 100 / total));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnStopBinding(HRESULT, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBindInfo(DWORD* f, BINDINFO*) override { *f = BINDF_GETNEWESTVERSION | BINDF_NOWRITECACHE; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDataAvailable(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnObjectAvailable(REFIID, IUnknown*) override { return S_OK; }
};

static std::string FileSha256(const std::wstring& file) {
    std::vector<BYTE> d;
    if (!ReadAll(file, d)) return "";
    BYTE h[32];
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, d.data(), (ULONG)d.size(), h, 32) != 0) return "";
    static const char* hx = "0123456789abcdef";
    std::string s;
    for (BYTE b : h) { s += hx[b >> 4]; s += hx[b & 15]; }
    return s;
}

static std::string ReadText(const std::wstring& file) {
    std::vector<BYTE> d;
    if (!ReadAll(file, d)) return "";
    return std::string(d.begin(), d.end());
}

static DWORD WINAPI ToolThread(LPVOID p) {
    HWND notify = (HWND)p;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wstring dir = KnownFolder(FOLDERID_LocalAppData) + L"\\HoverBar\\tools";
    CreateDirectoryW((KnownFolder(FOLDERID_LocalAppData) + L"\\HoverBar").c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring stamp = L"?t=" + std::to_wstring(GetTickCount64());  // nie eine alte Kopie aus dem Zwischenspeicher
    // 1. neueste Version bei GitHub erfragen
    ToolSet(notify, TOOL_CHECKING, L"Suche nach der neuesten Version …");
    std::wstring local = LocalToolVersion();
    std::wstring api = dir + L"\\latest.json";
    std::wstring latest;
    if (SUCCEEDED(URLDownloadToFileW(nullptr, (L"https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest" + stamp).c_str(), api.c_str(), 0, nullptr))) {
        std::string j = ReadText(api);
        size_t k = j.find("\"tag_name\"");
        if (k != std::string::npos) {
            size_t a = j.find('"', j.find(':', k) + 1), b = a == std::string::npos ? a : j.find('"', a + 1);
            if (b != std::string::npos) latest = FromUtf8(j.substr(a + 1, b - a - 1));
        }
    }
    DeleteFileW(api.c_str());
    EnterCriticalSection(&g_toolCs);
    g_tool.local = local;
    g_tool.latest = latest;
    LeaveCriticalSection(&g_toolCs);
    if (latest.empty()) {
        ToolSet(notify, TOOL_ERROR, L"GitHub nicht erreichbar – Internetverbindung prüfen.");
    } else if (!local.empty() && local == latest) {
        ToolSet(notify, TOOL_DONE, L"Ist schon die neueste Version.");
    } else {
        // 2. Prüfsummen-Liste und Programm herunterladen
        std::wstring base = L"https://github.com/yt-dlp/yt-dlp/releases/download/" + latest + L"/";
        std::wstring sums = dir + L"\\SHA2-256SUMS.txt", tmp = dir + L"\\yt-dlp.download";
        ToolSet(notify, TOOL_DOWNLOADING, L"", 0);
        DlProgress cb(notify);
        bool ok = SUCCEEDED(URLDownloadToFileW(nullptr, (base + L"SHA2-256SUMS" + stamp).c_str(), sums.c_str(), 0, nullptr)) &&
                  SUCCEEDED(URLDownloadToFileW(nullptr, (base + L"yt-dlp.exe" + stamp).c_str(), tmp.c_str(), 0, &cb));
        if (!ok) {
            ToolSet(notify, TOOL_ERROR, L"Herunterladen fehlgeschlagen.");
        } else {
            // 3. Prüfsumme vergleichen (Zeile "<hash>  yt-dlp.exe")
            ToolSet(notify, TOOL_VERIFYING, L"Prüfe die Datei …");
            std::string list = ReadText(sums), want;
            for (size_t a = 0; a < list.size();) {
                size_t b = list.find('\n', a);
                std::string line = list.substr(a, b == std::string::npos ? std::string::npos : b - a);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.size() > 66 && line.compare(line.size() - 11, 11, " yt-dlp.exe") == 0) want = line.substr(0, 64);
                if (b == std::string::npos) break;
                a = b + 1;
            }
            for (auto& ch : want) ch = (char)tolower(ch);
            std::string have = FileSha256(tmp);
            if (want.empty() || have != want) {
                ToolSet(notify, TOOL_ERROR, L"Prüfsumme passt nicht – Datei verworfen.");
            } else if (!MoveFileExW(tmp.c_str(), ToolPath().c_str(), MOVEFILE_REPLACE_EXISTING)) {
                ToolSet(notify, TOOL_ERROR, L"Konnte die alte Version nicht ersetzen (läuft gerade ein Download?).");
            } else {
                EnterCriticalSection(&g_toolCs);
                g_tool.local = latest;
                LeaveCriticalSection(&g_toolCs);
                ToolSet(notify, TOOL_DONE, local.empty() ? L"Installiert und geprüft." : L"Aktualisiert und geprüft.");
            }
        }
        DeleteFileW(tmp.c_str());
        DeleteFileW(sums.c_str());
    }
    EnterCriticalSection(&g_toolCs);
    g_toolBusy = false;
    LeaveCriticalSection(&g_toolCs);
    CoUninitialize();
    return 0;
}

void Music_ToolUpdate(HWND notify) {
    if (!g_toolCsInit) { InitializeCriticalSection(&g_toolCs); g_toolCsInit = true; }
    EnterCriticalSection(&g_toolCs);
    bool start = !g_toolBusy;
    g_toolBusy = true;
    LeaveCriticalSection(&g_toolCs);
    if (!start) return;
    HANDLE t = CreateThread(nullptr, 0, ToolThread, notify, 0, nullptr);
    if (t) CloseHandle(t);
    else { EnterCriticalSection(&g_toolCs); g_toolBusy = false; LeaveCriticalSection(&g_toolCs); }
}

// installierte Version im Hintergrund ermitteln (ohne Internet), z. B. beim Öffnen der Einstellungen
void Music_ToolCheckLocal(HWND notify) {
    if (!g_toolCsInit) { InitializeCriticalSection(&g_toolCs); g_toolCsInit = true; }
    EnterCriticalSection(&g_toolCs);
    bool start = !g_toolBusy && g_tool.local.empty() && g_tool.state == TOOL_IDLE;
    if (start) g_toolBusy = true;
    LeaveCriticalSection(&g_toolCs);
    if (!start) return;
    HANDLE t = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        std::wstring v = LocalToolVersion();
        EnterCriticalSection(&g_toolCs);
        g_tool.local = v;
        g_toolBusy = false;
        LeaveCriticalSection(&g_toolCs);
        PostMessageW((HWND)p, WM_APP_MUSIC, MUSIC_TOOL, 0);
        return 0;
    }, notify, 0, nullptr);
    if (t) CloseHandle(t);
    else { EnterCriticalSection(&g_toolCs); g_toolBusy = false; LeaveCriticalSection(&g_toolCs); }
}

// ---------------------------------------------------------------- Songtexte (Lyrics)
// JSON-Text ab dem öffnenden Anführungszeichen lesen (mit \n, \", \uXXXX)
static std::wstring JsonString(const std::string& j, size_t p) {
    std::wstring out;
    std::string raw;
    if (p >= j.size() || j[p] != '"') return out;
    p++;
    auto flush = [&] { if (!raw.empty()) { out += FromUtf8(raw); raw.clear(); } };
    while (p < j.size() && j[p] != '"') {
        if (j[p] == '\\' && p + 1 < j.size()) {
            flush();
            char e = j[++p];
            if (e == 'n') out += L'\n';
            else if (e == 'r') {}
            else if (e == 't') out += L'\t';
            else if (e == 'u' && p + 4 < j.size()) { out += (wchar_t)strtol(j.substr(p + 1, 4).c_str(), nullptr, 16); p += 4; }
            else out += (wchar_t)e;
            p++;
        } else {
            raw += j[p++];
        }
    }
    flush();
    return out;
}

// Wert eines Feldes innerhalb eines JSON-Abschnitts; leer bei null/fehlend
static std::wstring JsonField(const std::string& seg, const char* key) {
    std::string pat = std::string("\"") + key + "\":";
    size_t p = seg.find(pat);
    if (p == std::string::npos) return L"";
    p += pat.size();
    while (p < seg.size() && seg[p] == ' ') p++;
    return JsonString(seg, p);
}
static double JsonNumber(const std::string& seg, const char* key) {
    std::string pat = std::string("\"") + key + "\":";
    size_t p = seg.find(pat);
    return p == std::string::npos ? -1 : atof(seg.c_str() + p + pat.size());
}

// LRC-Text in Zeilen zerlegen; Zeilen ohne Zeitstempel bleiben als einfacher Text
static void ParseLrc(const std::wstring& txt, LyricsResult& r) {
    size_t a = 0;
    std::vector<LyricLine> timed, plain;
    while (a <= txt.size()) {
        size_t b = txt.find(L'\n', a);
        if (b == std::wstring::npos) b = txt.size();
        std::wstring line = txt.substr(a, b - a);
        a = b + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        std::vector<double> ts;
        size_t p = 0;
        bool tag = false;
        while (p < line.size() && line[p] == L'[') {
            size_t q = line.find(L']', p);
            if (q == std::wstring::npos) break;
            std::wstring in = line.substr(p + 1, q - p - 1);
            int mm = 0;
            double ss = 0;
            if (swscanf(in.c_str(), L"%d:%lf", &mm, &ss) == 2) ts.push_back(mm * 60 + ss);
            else tag = true;  // [ar:...], [ti:...] usw.
            p = q + 1;
        }
        std::wstring text = line.substr(p);
        while (!text.empty() && text.front() == L' ') text.erase(0, 1);
        if (!ts.empty()) for (double t : ts) timed.push_back({t, text});
        else if (!tag) plain.push_back({-1, text});
    }
    if (!timed.empty()) {
        std::stable_sort(timed.begin(), timed.end(), [](const LyricLine& x, const LyricLine& y) { return x.t < y.t; });
        r.lines = timed;
        r.synced = true;
    } else {
        while (!plain.empty() && plain.back().text.empty()) plain.pop_back();
        while (!plain.empty() && plain.front().text.empty()) plain.erase(plain.begin());
        r.lines = plain;
    }
}

// Titel von YouTube-Zusätzen befreien: "(Official Video)", "[Lyrics]" usw.
static std::wstring CleanTitle(std::wstring s) {
    for (wchar_t o : {L'(', L'['}) {
        wchar_t c = o == L'(' ? L')' : L']';
        size_t p;
        while ((p = s.find(o)) != std::wstring::npos) {
            size_t q = s.find(c, p);
            s.erase(p, q == std::wstring::npos ? std::wstring::npos : q - p + 1);
        }
    }
    for (const wchar_t* cut : {L" | ", L" ft. ", L" feat. ", L" ft ", L" feat "}) {
        size_t p = s.find(cut);
        if (p != std::wstring::npos) s.erase(p);
    }
    while (!s.empty() && (s.back() == L' ' || s.back() == L'-')) s.pop_back();
    while (!s.empty() && s.front() == L' ') s.erase(0, 1);
    return s;
}

struct LyricsParams { std::wstring id, file, title, artist; int duration; bool online; };

static DWORD WINAPI LyricsThread(LPVOID pp) {
    auto* p = (LyricsParams*)pp;
    auto* res = new LyricsResult();
    res->songId = p->id;
    std::wstring lrc = p->file;
    size_t dot = lrc.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > lrc.find_last_of(L'\\')) lrc.erase(dot);
    lrc += L".lrc";
    // 1) lokale .lrc-Datei
    HANDLE h = CreateFileW(lrc.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        std::string data;
        DWORD sz = GetFileSize(h, nullptr), got = 0;
        if (sz && sz < 2 * 1024 * 1024) {
            data.resize(sz);
            if (ReadFile(h, &data[0], sz, &got, nullptr)) data.resize(got);
        }
        CloseHandle(h);
        if (data.size() >= 3 && (unsigned char)data[0] == 0xEF) data.erase(0, 3);  // BOM
        ParseLrc(FromUtf8(data), *res);
    } else if (p->online) {
        // 2) lrclib.net: erst genaue Abfrage (Titel + Künstler + Länge), dann freie Suche
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::wstring title = CleanTitle(p->title), artist = p->artist;
        size_t dash = title.find(L" - ");
        if (dash != std::wstring::npos) {  // YouTube: "Künstler - Titel"
            artist = title.substr(0, dash);
            title = CleanTitle(title.substr(dash + 3));
        }
        // mehrere Künstler ("A, B", "A & B", "A x B", "A feat. B"): für die Suche nur den ersten
        for (const wchar_t* sep : {L", ", L" & ", L" x ", L" X ", L" feat", L" ft.", L" und ", L" and "}) {
            size_t q = artist.find(sep);
            if (q != std::wstring::npos && q > 0) artist.erase(q);
        }
        std::wstring synced, plain;
        bool instrumental = false;
        auto take = [&](const std::string& seg) {
            std::wstring s = JsonField(seg, "syncedLyrics"), t = JsonField(seg, "plainLyrics");
            if (!s.empty() && synced.empty()) synced = s;
            if (!t.empty() && plain.empty()) plain = t;
            if (seg.find("\"instrumental\":true") != std::string::npos) instrumental = true;
        };
        if (!title.empty() && !artist.empty()) {
            std::wstring u = L"https://lrclib.net/api/get?track_name=" + UrlEncode(title) + L"&artist_name=" + UrlEncode(artist);
            if (p->duration > 0) u += L"&duration=" + std::to_wstring(p->duration);
            std::string j = HttpGet(u);
            if (j.find("\"id\":") != std::string::npos) take(j);
        }
        // freie Suche: erst Titel + Künstler, dann nur Titel (dann muss die Länge passen)
        for (int attempt = 0; attempt < 2 && synced.empty() && plain.empty(); attempt++) {
            if (attempt == 1 && (artist.empty() || p->duration <= 0)) break;
            std::wstring q = attempt == 0 && !artist.empty() ? title + L" " + artist : title;
            std::string j = HttpGet(L"https://lrclib.net/api/search?q=" + UrlEncode(q));
            // Treffer einzeln betrachten, passende Länge (±4 s) zuerst
            std::vector<std::string> segs;
            size_t a = j.find("{\"id\":");
            while (a != std::string::npos) {
                size_t b = j.find("{\"id\":", a + 1);
                segs.push_back(j.substr(a, b == std::string::npos ? std::string::npos : b - a));
                a = b;
            }
            for (int pass = 0; pass < (attempt == 0 ? 2 : 1) && synced.empty(); pass++)
                for (auto& s : segs) {
                    double d = JsonNumber(s, "duration");
                    if (pass == 0 && p->duration > 0 && (d < 0 || std::fabs(d - p->duration) > 4)) continue;
                    take(s);
                    if (!synced.empty()) break;
                }
        }
        std::wstring txt = !synced.empty() ? synced : plain;
        if (txt.empty() && instrumental) txt = L"♪ Instrumental";
        if (!txt.empty()) {
            ParseLrc(txt, *res);
            std::string u8 = ToUtf8(txt);  // für das nächste Mal (und offline) speichern
            HANDLE w = CreateFileW(lrc.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (w != INVALID_HANDLE_VALUE) {
                DWORD wr = 0;
                WriteFile(w, u8.data(), (DWORD)u8.size(), &wr, nullptr);
                CloseHandle(w);
            }
        }
        CoUninitialize();
    }
    if (!PostMessageW(g_notify, WM_APP_MUSIC, MUSIC_LYRICS, (LPARAM)res)) delete res;
    delete p;
    return 0;
}

void Music_LoadLyrics(const std::wstring& id, bool online) {
    int i = Music_Find(id);
    if (i < 0) return;
    const Song& s = Music_Songs()[i];
    auto* p = new LyricsParams{id, s.file, s.title, s.artist, s.duration, online};
    HANDLE t = CreateThread(nullptr, 0, LyricsThread, p, 0, nullptr);
    if (t) CloseHandle(t);
    else delete p;
}