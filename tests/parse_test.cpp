// cl /EHsc /std:c++17 /utf-8 tests\parse_test.cpp && parse_test.exe
#include <cstdio>
#include "../src/parse.h"

#define CHECK(x) if (!(x)) { printf("FEHLER Zeile %d: %s\n", __LINE__, #x); return 1; }

int main() {
    auto r = ParseCsv("\xEF\xBB\xBF\"Track Name\",\"Artist Name(s)\"\r\n\"Say \"\"Hi\"\", Bob\",\"A;B\"\r\n\r\nplain,x");
    CHECK(r.size() == 3);
    CHECK(r[0][0] == "Track Name");
    CHECK(r[1][0] == "Say \"Hi\", Bob" && r[1][1] == "A;B");
    CHECK(r[2][1] == "x");
    auto o = JsonObjects("{\"data\":[{\"t\":\"a}\\\"]\",\"x\":{\"y\":[1]}},{\"t\":\"b\"}],\"total\":2}");
    CHECK(o.size() == 2 && o[1] == "{\"t\":\"b\"}");
    CHECK(JsonObjects("{\"error\":{}}").empty());
    std::string l = "WARNING: x 200\naaaaaaaaaaa 340.0\r\nbbbbbbbbbbb NA\nccccccccccc 337\n";
    CHECK(PickByDuration(l, 337) == "aaaaaaaaaaa");
    CHECK(PickByDuration(l, 330) == "");
    CHECK(PickByDuration(l, 332) == "ccccccccccc");
    auto f = ParseSearch("WARNING: a|b|c|d\naaaaaaaaaaa|340.0|Kanal|Lied | Live\r\n");
    CHECK(f.size() == 1 && f[0].seconds == 340 && f[0].channel == "Kanal" && f[0].title == "Lied | Live");
    puts("ok");
    return 0;
}
