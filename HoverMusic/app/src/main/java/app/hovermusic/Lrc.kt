package app.hovermusic

data class LyricLine(val timeMs: Long, val text: String)

data class Lyrics(val lines: List<LyricLine>, val synced: Boolean) {
    /** Index der aktuell gesungenen Zeile (-1 vor der ersten oder ohne Zeitstempel). */
    fun indexAt(posMs: Long): Int {
        if (!synced) return -1
        var k = -1
        for (i in lines.indices) if (lines[i].timeMs <= posMs + 150) k = i else break
        return k
    }

    companion object {
        val EMPTY = Lyrics(emptyList(), false)

        /** LRC-Text zerlegen: "[mm:ss.xx] Text"; Zeilen ohne Zeitstempel bleiben als einfacher Text. */
        fun parse(text: String): Lyrics {
            val timed = ArrayList<LyricLine>()
            val plain = ArrayList<LyricLine>()
            val stamp = Regex("""\[(\d+):(\d+(?:\.\d+)?)]""")
            for (raw in text.lineSequence()) {
                val line = raw.trimEnd()
                val stamps = stamp.findAll(line).toList()
                if (stamps.isNotEmpty()) {
                    val body = line.substring(stamps.last().range.last + 1).trim()
                    for (m in stamps) {
                        val ms = (m.groupValues[1].toLong() * 60_000 + (m.groupValues[2].toDouble() * 1000).toLong())
                        timed.add(LyricLine(ms, body))
                    }
                } else if (line.isNotEmpty() && !line.startsWith("[")) {
                    plain.add(LyricLine(-1, line))
                }
            }
            return if (timed.isNotEmpty()) Lyrics(timed.sortedBy { it.timeMs }, true) else Lyrics(plain, false)
        }
    }
}
