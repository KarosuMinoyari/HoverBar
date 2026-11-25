package app.hovermusic

import com.google.zxing.BinaryBitmap
import com.google.zxing.RGBLuminanceSource
import com.google.zxing.common.HybridBinarizer
import com.google.zxing.qrcode.QRCodeReader
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Gegenprobe für den QR-Encoder von HoverBar (src/qrcode.cpp): die von "HoverBar.exe --qrtest" geschriebenen Beispiele
 * (qr-samples.txt) werden mit ZXing, einem unabhängigen QR-Leser, dekodiert.
 */
class QrDecodeTest {
    private class Sample(val text: String, val ecc: Char, val version: Int, val rows: List<String>)

    private fun load(): List<Sample> {
        val lines = javaClass.getResourceAsStream("/qr-samples.txt")!!.readBytes().toString(Charsets.UTF_8).lines()
        val out = ArrayList<Sample>()
        var i = 0
        while (i < lines.size) {
            if (!lines[i].startsWith("T:")) { i++; continue }
            val text = lines[i].removePrefix("T:")
            val ecc = lines[i + 1].removePrefix("E:")[0]
            val ver = lines[i + 2].removePrefix("V:").toInt()
            i += 3
            val rows = ArrayList<String>()
            while (i < lines.size && lines[i].isNotEmpty()) rows.add(lines[i++])
            out.add(Sample(text, ecc, ver, rows))
        }
        return out
    }

    private fun decode(s: Sample): String {
        val scale = 6
        val quiet = 4
        val n = s.rows.size
        val dim = (n + 2 * quiet) * scale
        val px = IntArray(dim * dim) { 0xFFFFFFFF.toInt() }
        for (y in 0 until n) for (x in 0 until n) if (s.rows[y][x] == '#') {
            for (dy in 0 until scale) for (dx in 0 until scale) px[((y + quiet) * scale + dy) * dim + (x + quiet) * scale + dx] = 0xFF000000.toInt()
        }
        val bmp = BinaryBitmap(HybridBinarizer(RGBLuminanceSource(dim, dim, px)))
        return QRCodeReader().decode(bmp).text
    }

    @Test
    fun hasSamplesOfSeveralVersions() {
        val vs = load().map { it.version }.toSet()
        assertTrue("zu wenige Beispiele: $vs", vs.size >= 6)
    }

    @Test
    fun everySampleDecodesToItsText() {
        val samples = load()
        assertTrue(samples.size >= 10)
        for (s in samples) {
            assertEquals("Version ${s.version}, Stufe ${s.ecc}, ${s.text.length} Zeichen", s.text, decode(s))
        }
    }
}
