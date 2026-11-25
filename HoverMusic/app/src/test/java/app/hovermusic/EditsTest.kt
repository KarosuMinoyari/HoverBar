package app.hovermusic

import org.junit.Assert.assertEquals
import org.junit.Test

class EditsTest {
    @Test fun zusammenfuehren() {
        var t = Edits.merge("", Edit("a", "Alt", "X", "Cover/1.jpg"))
        t = Edits.merge(t, Edit("b", "Zwei\tmit Tab", "", ""))
        t = Edits.merge(t, Edit("a", "Neu", "Y", ""))
        assertEquals("a\tNeu\tY\tCover/1.jpg\t0\t0\nb\tZwei mit Tab\t\t\t0\t0\n", t)
        t = Edits.merge(t, Edit("a", "Neu", "Y", "", 1000, 5000))
        t = Edits.merge(t, Edit("a", "Neuer", "Y", ""), keepTrim = true)
        assertEquals(5000L, Edits.parse(t)["a"]?.toMs)
        t = Edits.merge(t, Edit("a", "Neu", "Y", ""))
        assertEquals(0L, Edits.parse(t)["a"]?.toMs)
        assertEquals("Neu", Edits.parse(t)["a"]?.title)
    }
}
