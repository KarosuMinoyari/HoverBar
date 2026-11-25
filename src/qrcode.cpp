#include "qrcode.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace {

// Fehlerkorrektur-Bytes je Block und Anzahl Blöcke, [0] = L, [1] = M, Index = Version (1–40, hier bis 20 genutzt)
const int kEccPerBlock[2][41] = {
    {-1, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28, 28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
    {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26, 26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28},
};
const int kNumBlocks[2][41] = {
    {-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 4, 4, 6, 6, 6, 6, 7, 8, 8, 9, 9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25},
    {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49},
};
const int kMaxVersion = 20;

int rawDataModules(int ver) {
    int result = (16 * ver + 128) * ver + 64;
    if (ver >= 2) {
        int numAlign = ver / 7 + 2;
        result -= (25 * numAlign - 10) * numAlign - 55;
        if (ver >= 7) result -= 36;
    }
    return result;
}
int dataCodewords(int ver, int ecc) { return rawDataModules(ver) / 8 - kEccPerBlock[ecc][ver] * kNumBlocks[ecc][ver]; }

// ---- Reed-Solomon über GF(256), Polynom 0x11D
uint8_t rsMul(uint8_t x, uint8_t y) {
    int z = 0;
    for (int i = 7; i >= 0; i--) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return (uint8_t)z;
}
std::vector<uint8_t> rsDivisor(int degree) {
    std::vector<uint8_t> result(degree);
    result[degree - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; i++) {
        for (int j = 0; j < degree; j++) {
            result[j] = rsMul(result[j], root);
            if (j + 1 < degree) result[j] ^= result[j + 1];
        }
        root = rsMul(root, 0x02);
    }
    return result;
}
std::vector<uint8_t> rsRemainder(const uint8_t* data, int len, const std::vector<uint8_t>& divisor) {
    std::vector<uint8_t> result(divisor.size());
    for (int i = 0; i < len; i++) {
        uint8_t factor = data[i] ^ result[0];
        result.erase(result.begin());
        result.push_back(0);
        for (size_t j = 0; j < result.size(); j++) result[j] ^= rsMul(divisor[j], factor);
    }
    return result;
}

struct Builder {
    int size, ver, ecc;
    std::vector<char> mod, func;  // Module / Funktionsbereich (nicht von Daten und Maske betroffen)

    Builder(int v, int e) : size(v * 4 + 17), ver(v), ecc(e), mod((size_t)size * size, 0), func((size_t)size * size, 0) {}

    void set(int x, int y, bool dark) {  // Funktionsmodul setzen
        mod[(size_t)y * size + x] = dark;
        func[(size_t)y * size + x] = 1;
    }
    bool get(int x, int y) const { return mod[(size_t)y * size + x] != 0; }

    void drawFinder(int cx, int cy) {
        for (int dy = -4; dy <= 4; dy++)
            for (int dx = -4; dx <= 4; dx++) {
                int dist = std::max(std::abs(dx), std::abs(dy)), x = cx + dx, y = cy + dy;
                if (x >= 0 && x < size && y >= 0 && y < size) set(x, y, dist != 2 && dist != 4);
            }
    }
    void drawAlignment(int cx, int cy) {
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++) set(cx + dx, cy + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
    }
    std::vector<int> alignPositions() const {
        std::vector<int> r;
        if (ver == 1) return r;
        int num = ver / 7 + 2;
        int step = (ver == 32) ? 26 : (ver * 4 + num * 2 + 1) / (num * 2 - 2) * 2;
        r.resize(num);
        r[0] = 6;
        for (int i = num - 1, pos = size - 7; i >= 1; i--, pos -= step) r[i] = pos;
        return r;
    }
    void drawFormat(int mask) {
        int data = (ecc == 0 ? 1 : 0) << 3 | mask;  // L = 01, M = 00
        int rem = data;
        for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        int bits = (data << 10 | rem) ^ 0x5412;
        auto bit = [&](int i) { return ((bits >> i) & 1) != 0; };
        for (int i = 0; i <= 5; i++) set(8, i, bit(i));
        set(8, 7, bit(6));
        set(8, 8, bit(7));
        set(7, 8, bit(8));
        for (int i = 9; i < 15; i++) set(14 - i, 8, bit(i));
        for (int i = 0; i < 8; i++) set(size - 1 - i, 8, bit(i));
        for (int i = 8; i < 15; i++) set(8, size - 15 + i, bit(i));
        set(8, size - 8, true);
    }
    void drawVersion() {
        if (ver < 7) return;
        int rem = ver;
        for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        int bits = ver << 12 | rem;
        for (int i = 0; i < 18; i++) {
            bool b = ((bits >> i) & 1) != 0;
            int a = size - 11 + i % 3, c = i / 3;
            set(a, c, b);
            set(c, a, b);
        }
    }
    void drawFunctionPatterns() {
        for (int i = 0; i < size; i++) {
            set(6, i, i % 2 == 0);
            set(i, 6, i % 2 == 0);
        }
        drawFinder(3, 3);
        drawFinder(size - 4, 3);
        drawFinder(3, size - 4);
        std::vector<int> pos = alignPositions();
        int n = (int)pos.size();
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++)
                if (!((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0))) drawAlignment(pos[i], pos[j]);
        drawFormat(0);  // nur um den Bereich zu belegen, wird später überschrieben
        drawVersion();
    }
    void drawCodewords(const std::vector<uint8_t>& data) {
        size_t i = 0;
        for (int right = size - 1; right >= 1; right -= 2) {
            if (right == 6) right = 5;
            for (int vert = 0; vert < size; vert++)
                for (int j = 0; j < 2; j++) {
                    int x = right - j;
                    bool upward = ((right + 1) & 2) == 0;
                    int y = upward ? size - 1 - vert : vert;
                    if (!func[(size_t)y * size + x] && i < data.size() * 8) {
                        mod[(size_t)y * size + x] = ((data[i >> 3] >> (7 - (i & 7))) & 1) != 0;
                        i++;
                    }
                }
        }
    }
    void applyMask(int mask) {
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++) {
                bool inv = false;
                switch (mask) {
                case 0: inv = (x + y) % 2 == 0; break;
                case 1: inv = y % 2 == 0; break;
                case 2: inv = x % 3 == 0; break;
                case 3: inv = (x + y) % 3 == 0; break;
                case 4: inv = (x / 3 + y / 2) % 2 == 0; break;
                case 5: inv = x * y % 2 + x * y % 3 == 0; break;
                case 6: inv = (x * y % 2 + x * y % 3) % 2 == 0; break;
                case 7: inv = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                }
                if (inv && !func[(size_t)y * size + x]) mod[(size_t)y * size + x] ^= 1;
            }
    }
    long penalty() const {
        long result = 0;
        auto line = [&](bool rows) {
            for (int a = 0; a < size; a++) {
                int run = 1;
                std::vector<char> v(size);
                for (int b = 0; b < size; b++) v[b] = rows ? get(b, a) : get(a, b);
                for (int b = 1; b < size; b++) {
                    if (v[b] == v[b - 1]) {
                        run++;
                        if (run == 5) result += 3;
                        else if (run > 5) result++;
                    } else run = 1;
                }
                // Muster 1:1:3:1:1 mit hellem Rand
                static const char p1[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0}, p2[11] = {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};
                for (int b = 0; b + 11 <= size; b++) {
                    bool m1 = true, m2 = true;
                    for (int k = 0; k < 11; k++) {
                        if (v[b + k] != p1[k]) m1 = false;
                        if (v[b + k] != p2[k]) m2 = false;
                    }
                    if (m1) result += 40;
                    if (m2) result += 40;
                }
            }
        };
        line(true);
        line(false);
        for (int y = 0; y + 1 < size; y++)
            for (int x = 0; x + 1 < size; x++) {
                bool c = get(x, y);
                if (c == get(x + 1, y) && c == get(x, y + 1) && c == get(x + 1, y + 1)) result += 3;
            }
        long dark = 0;
        for (char c : mod) dark += c != 0;
        long total = (long)size * size;
        long k = (std::labs(dark * 20 - total * 10) + total - 1) / total - 1;
        if (k > 0) result += k * 10;
        return result;
    }
};

std::vector<uint8_t> eccAndInterleave(const std::vector<uint8_t>& data, int ver, int ecc) {
    int numBlocks = kNumBlocks[ecc][ver], blockEccLen = kEccPerBlock[ecc][ver];
    int rawCodewords = rawDataModules(ver) / 8;
    int numShort = numBlocks - rawCodewords % numBlocks, shortLen = rawCodewords / numBlocks;
    std::vector<std::vector<uint8_t>> blocks;
    std::vector<uint8_t> divisor = rsDivisor(blockEccLen);
    for (int i = 0, k = 0; i < numBlocks; i++) {
        int datLen = shortLen - blockEccLen + (i < numShort ? 0 : 1);
        std::vector<uint8_t> dat(data.begin() + k, data.begin() + k + datLen);
        k += datLen;
        std::vector<uint8_t> e = rsRemainder(dat.data(), (int)dat.size(), divisor);
        if (i < numShort) dat.push_back(0);
        dat.insert(dat.end(), e.begin(), e.end());
        blocks.push_back(dat);
    }
    std::vector<uint8_t> result;
    for (size_t i = 0; i < blocks[0].size(); i++)
        for (int j = 0; j < numBlocks; j++)
            if (i != (size_t)(shortLen - blockEccLen) || j >= numShort) result.push_back(blocks[j][i]);
    return result;
}

}  // namespace

bool Qr_Encode(const std::string& text, bool eccMedium, QrCode& out) {
    int ecc = eccMedium ? 1 : 0;
    int ver = 0, ccBits = 8;
    for (int v = 1; v <= kMaxVersion; v++) {
        int cc = v <= 9 ? 8 : 16;
        if (4 + cc + 8 * (int)text.size() <= dataCodewords(v, ecc) * 8) { ver = v; ccBits = cc; break; }
    }
    if (!ver) return false;

    // Datenbits: Modus Byte (0100), Länge, Daten, Abschluss, Füllbytes
    std::vector<char> bits;
    auto put = [&](unsigned val, int len) {
        for (int i = len - 1; i >= 0; i--) bits.push_back((val >> i) & 1);
    };
    put(0x4, 4);
    put((unsigned)text.size(), ccBits);
    for (unsigned char c : text) put(c, 8);
    size_t cap = (size_t)dataCodewords(ver, ecc) * 8;
    put(0, (int)std::min<size_t>(4, cap - bits.size()));
    while (bits.size() % 8) bits.push_back(0);
    std::vector<uint8_t> data(bits.size() / 8);
    for (size_t i = 0; i < bits.size(); i++) data[i >> 3] |= (uint8_t)(bits[i] << (7 - (i & 7)));
    for (uint8_t pad = 0xEC; data.size() < cap / 8; pad ^= 0xEC ^ 0x11) data.push_back(pad);

    std::vector<uint8_t> all = eccAndInterleave(data, ver, ecc);
    Builder b(ver, ecc);
    b.drawFunctionPatterns();
    b.drawCodewords(all);

    int best = 0;
    long bestPen = -1;
    for (int m = 0; m < 8; m++) {
        b.applyMask(m);
        b.drawFormat(m);
        long p = b.penalty();
        b.applyMask(m);  // zurücknehmen
        if (bestPen < 0 || p < bestPen) { best = m; bestPen = p; }
    }
    b.applyMask(best);
    b.drawFormat(best);

    out.size = b.size;
    out.version = ver;
    out.modules = b.mod;
    return true;
}
