// Equalizer: 8 Bänder, Voreinstellungen wie in der Soundcore-App (Werte selbst nachempfunden).
// Der Windows-MediaPlayer hat keinen eigenen Equalizer; deshalb dekodiert HoverBar die Datei selbst, filtert den Ton
// und reicht ihn als MediaStreamSource an denselben Player weiter (Playlist, Spulen und Medientasten bleiben).
#pragma once
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.MediaProperties.h>
#include <winrt/Windows.Storage.Streams.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace eq {

constexpr int kBands = 8, kRate = 44100, kChannels = 2;
inline constexpr double kFreq[kBands] = {100, 200, 400, 800, 1600, 3200, 6400, 12800};
struct Preset { const wchar_t* name; double db[kBands]; };
inline constexpr Preset kPresets[] = {
    {L"Flat", {0, 0, 0, 0, 0, 0, 0, 0}},
    {L"Soundcore Signature", {2, 1, -1, -1, 0, 1, 3, 2}},
    {L"Acoustic", {3, 1, 1, 2, 2, 2, 3, 2}},
    {L"Bass Booster", {6, 4, 2, 0, 0, 0, 0, 0}},
    {L"Bass Reducer", {-6, -4, -2, 0, 0, 0, 0, 0}},
    {L"Classical", {3, 2, -1, -1, 0, 1, 3, 3}},
    {L"Podcast", {-3, 0, 2, 3, 3, 2, 0, -2}},
    {L"Dance", {5, 3, 0, -2, -1, 1, 3, 3}},
    {L"Deep", {5, 3, 1, 1, 0, -2, -3, -4}},
    {L"Electronic", {4, 3, 0, -2, 1, 1, 3, 4}},
    {L"Hip-Hop", {5, 4, 1, -1, -1, 1, 2, 3}},
    {L"Jazz", {3, 2, 1, -1, -1, 0, 2, 3}},
    {L"Latin", {3, 2, 0, -1, -1, 0, 3, 4}},
    {L"Lounge", {-2, 0, 2, 3, 1, 0, 1, 1}},
    {L"Piano", {1, 0, 1, 3, 2, 2, 3, 2}},
    {L"Pop", {-1, 1, 3, 3, 2, 0, -1, -1}},
    {L"R&B", {4, 5, 3, -1, -1, 1, 2, 3}},
    {L"Rock", {4, 3, 1, -1, -1, 1, 3, 4}},
    {L"Small Speakers", {5, 4, 2, 1, 0, -2, -3, -3}},
    {L"Spoken Word", {-4, -2, 1, 3, 3, 2, 0, -3}},
    {L"Treble Booster", {0, 0, 0, 0, 1, 3, 5, 6}},
    {L"Treble Reducer", {0, 0, 0, 0, -1, -3, -5, -6}},
};
constexpr int kPresetCount = (int)(sizeof(kPresets) / sizeof(kPresets[0]));
inline std::atomic<int> g_preset{0};  // gilt sofort für alle laufenden Quellen

// Glockenfilter (RBJ), eine Oktave breit
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1[kChannels] = {}, z2[kChannels] = {};
    void Set(double freq, double db) {
        double A = std::pow(10.0, db / 40.0), w = 2 * 3.14159265358979 * freq / kRate, alpha = std::sin(w) / (2 * 1.41), a0 = 1 + alpha / A;
        b0 = (1 + alpha * A) / a0;
        b1 = -2 * std::cos(w) / a0;
        b2 = (1 - alpha * A) / a0;
        a1 = b1;
        a2 = (1 - alpha / A) / a0;
    }
    double Run(double x, int c) {
        double y = b0 * x + z1[c];
        z1[c] = b1 * x - a1 * y + z2[c];
        z2[c] = b2 * x - a2 * y;
        return y;
    }
};

struct Filter {
    Biquad band[kBands];
    double gain = 1;
    int preset = -1;
    // in: -1..1 je Kanal abwechselnd; out: 16 Bit. Voreinstellung "Flat" lässt den Ton unverändert.
    void Process(const float* in, short* out, size_t samples) {
        int p = g_preset;
        if (p < 0 || p >= kPresetCount) p = 0;
        if (p != preset) {
            preset = p;
            double top = 0;
            for (int i = 0; i < kBands; i++) {
                band[i].Set(kFreq[i], kPresets[p].db[i]);
                top = std::max(top, kPresets[p].db[i]);
            }
            gain = std::pow(10.0, -0.6 * top / 20.0);  // etwas leiser, damit Anhebungen nicht übersteuern
        }
        for (size_t i = 0; i < samples; i++) {
            double x = in[i] * gain;
            if (p)
                for (auto& b : band) x = b.Run(x, (int)(i % kChannels));
            out[i] = (short)std::lround(std::max(-1.0, std::min(1.0, x)) * 32767.0);
        }
    }
};

struct Stream {
    std::wstring file;
    std::mutex mu;
    IMFSourceReader* rd = nullptr;
    Filter filter;
    ~Stream() { Close(); }
    void Close() {
        if (rd) rd->Release();
        rd = nullptr;
    }
    bool Open() {
        if (rd) return true;
        if (FAILED(MFCreateSourceReaderFromURL(file.c_str(), nullptr, &rd))) return false;
        rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        rd->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
        IMFMediaType* t = nullptr;
        MFCreateMediaType(&t);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
        t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
        t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
        t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4 * kChannels);
        t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 4 * kChannels * kRate);
        HRESULT hr = rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, t);
        t->Release();
        if (FAILED(hr)) Close();
        return rd != nullptr;
    }
};

inline winrt::Windows::Media::Core::MediaSource Source(const std::wstring& file, int seconds) {
    namespace wmc = winrt::Windows::Media::Core;
    namespace wss = winrt::Windows::Storage::Streams;
    static const HRESULT started = MFStartup(MF_VERSION, MFSTARTUP_LITE);  // einmal für die ganze Laufzeit
    (void)started;
    auto s = std::make_shared<Stream>();
    s->file = file;
    auto props = winrt::Windows::Media::MediaProperties::AudioEncodingProperties::CreatePcm(kRate, kChannels, 16);
    wmc::MediaStreamSource mss{wmc::AudioStreamDescriptor(props)};
    mss.Duration(winrt::Windows::Foundation::TimeSpan((long long)seconds * 10000000));
    mss.CanSeek(true);
    mss.BufferTime(winrt::Windows::Foundation::TimeSpan(3000000));  // 0,3 s: eine neue Voreinstellung ist schnell hörbar
    mss.Starting([s](wmc::MediaStreamSource const&, wmc::MediaStreamSourceStartingEventArgs const& e) {
        auto from = e.Request().StartPosition();
        std::lock_guard<std::mutex> l(s->mu);
        if (!from || !s->Open()) return;
        long long t = from.Value().count();
        PROPVARIANT v;
        InitPropVariantFromInt64(t, &v);
        s->rd->SetCurrentPosition(GUID_NULL, v);
        PropVariantClear(&v);
        s->filter = Filter();
        e.Request().SetActualStartPosition(winrt::Windows::Foundation::TimeSpan(t));
    });
    mss.SampleRequested([s](wmc::MediaStreamSource const&, wmc::MediaStreamSourceSampleRequestedEventArgs const& e) {
        std::lock_guard<std::mutex> l(s->mu);
        if (!s->Open()) return;
        for (;;) {
            DWORD flags = 0;
            LONGLONG ts = 0;
            IMFSample* smp = nullptr;
            if (FAILED(s->rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &ts, &smp))) return;
            if (!smp) {
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return;  // keine Probe = Ende
                continue;
            }
            IMFMediaBuffer* buf = nullptr;
            BYTE* data = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(smp->ConvertToContiguousBuffer(&buf)) && SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
                size_t n = len / sizeof(float);
                wss::Buffer out((uint32_t)(n * sizeof(short)));
                out.Length((uint32_t)(n * sizeof(short)));
                s->filter.Process((const float*)data, (short*)out.data(), n);
                buf->Unlock();
                auto sample = wmc::MediaStreamSample::CreateFromBuffer(out, winrt::Windows::Foundation::TimeSpan(ts));
                sample.Duration(winrt::Windows::Foundation::TimeSpan((long long)(n / kChannels) * 10000000 / kRate));
                e.Request().Sample(sample);
            }
            if (buf) buf->Release();
            smp->Release();
            return;
        }
    });
    mss.Closed([s](wmc::MediaStreamSource const&, wmc::MediaStreamSourceClosedEventArgs const&) {
        std::lock_guard<std::mutex> l(s->mu);
        s->Close();
    });
    return wmc::MediaSource::CreateFromMediaStreamSource(mss);
}

}  // namespace eq
