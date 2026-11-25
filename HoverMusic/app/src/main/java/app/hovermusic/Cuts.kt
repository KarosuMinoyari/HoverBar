package app.hovermusic

/**
 * Schnitte eines Songs als Text: Arbeitsschritte mit ";" getrennt, je Schritt entfernte Bereiche "von-bis" (ms, bezogen auf die
 * ungeschnittene Datei) mit "," getrennt. Rückgängig = letzten Schritt weglassen. Die Datei selbst bleibt auf dem Handy unverändert.
 */
object Cuts {
    /** Alle entfernten Bereiche, sortiert und ohne Überschneidungen. */
    fun removed(cuts: String): List<LongRange> {
        val all = cuts.split(';', ',').mapNotNull {
            val p = it.split('-')
            val a = p.getOrNull(0)?.toLongOrNull()
            val b = p.getOrNull(1)?.toLongOrNull()
            if (p.size == 2 && a != null && b != null && b > a) a..b else null
        }.sortedBy { it.first }
        val out = ArrayList<LongRange>()
        for (r in all) {
            val last = out.lastOrNull()
            if (last != null && r.first <= last.last) out[out.lastIndex] = last.first..maxOf(last.last, r.last) else out.add(r)
        }
        return out
    }

    private fun step(cuts: String, ranges: List<LongRange>): String {
        val text = ranges.filter { it.last > it.first }.joinToString(",") { "${it.first}-${it.last}" }
        return if (text.isEmpty()) cuts else if (cuts.isEmpty()) text else "$cuts;$text"
    }

    fun removeSelection(cuts: String, from: Long, to: Long) = step(cuts, listOf(from..to))
    fun keepSelection(cuts: String, from: Long, to: Long, length: Long) = step(cuts, listOf(0L..from, to..length))
    fun undo(cuts: String) = cuts.substringBeforeLast(';', "")

    /** Länge, die noch gespielt wird. */
    fun kept(cuts: String, length: Long) = visible(cuts, length)

    /** Position in der Datei -> Position im geschnittenen Song. */
    fun visible(cuts: String, pos: Long): Long = pos - removed(cuts).sumOf { (minOf(pos, it.last) - it.first).coerceAtLeast(0) }

    /** Position im geschnittenen Song -> Position in der Datei. */
    fun original(cuts: String, pos: Long): Long {
        var p = pos
        for (r in removed(cuts)) if (p >= r.first) p += r.last - r.first
        return p
    }
}
