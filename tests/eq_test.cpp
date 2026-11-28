// cl /EHsc /std:c++17 /utf-8 tests\eq_test.cpp && eq_test.exe [Musikdatei]
// Ohne Datei: nur die Filter. Mit Datei: zusätzlich stumm über den Windows-MediaPlayer abspielen und spulen.
#define NOMINMAX
#include <algorithm>
#include <cstdio>
#include "../src/eq.h"
#include <winrt/Windows.Media.Playback.h>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "propsys.lib")

#define CHECK(x) if (!(x)) { printf("FEHLER Zeile %d: %s\n", __LINE__, #x); return 1; }

// Lautstärke eines Sinus nach dem Filter, im Verhältnis zu "Flat" (dB)
static double Response(int preset, double freq) {
    auto level = [&](int p) {
        eq::g_preset = p;
        eq::Filter f;
        std::vector<float> in(eq::kRate * 2);
        std::vector<short> out(in.size());
        for (size_t i = 0; i < in.size(); i++) in[i] = 0.2f * (float)std::sin(2 * 3.14159265358979 * freq * (i / 2) / eq::kRate);
        f.Process(in.data(), out.data(), in.size());
        double sum = 0;
        for (size_t i = in.size() / 2; i < in.size(); i++) sum += (double)out[i] * out[i];
        return std::sqrt(sum);
    };
    return 20 * std::log10(level(preset) / level(0));
}

int wmain(int argc, wchar_t** argv) {
    int bass = 0, treble = 0;
    for (int i = 0; i < eq::kPresetCount; i++) {
        if (!wcscmp(eq::kPresets[i].name, L"Bass Booster")) bass = i;
        if (!wcscmp(eq::kPresets[i].name, L"Treble Booster")) treble = i;
    }
    CHECK(Response(bass, 100) - Response(bass, 6400) > 4);
    CHECK(Response(treble, 12800) - Response(treble, 100) > 4);
    if (argc > 1) {
        namespace wmp = winrt::Windows::Media::Playback;
        winrt::init_apartment();
        eq::g_preset = bass;
        wmp::MediaPlayer p;
        p.Volume(0);
        p.Source(eq::Source(argv[1], 180));
        p.Play();
        Sleep(4000);
        double a = p.PlaybackSession().Position().count() / 1e7;
        p.PlaybackSession().Position(winrt::Windows::Foundation::TimeSpan(600000000));
        Sleep(3000);
        double b = p.PlaybackSession().Position().count() / 1e7;
        printf("nach 4 s: %.1f s, nach Spulen auf 60 s und 3 s: %.1f s\n", a, b);
        CHECK(a > 2 && a < 6);
        CHECK(b > 61 && b < 66);
        p.Close();
    }
    puts("ok");
    return 0;
}
