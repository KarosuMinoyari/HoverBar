package app.hovermusic

import org.junit.Assert.assertEquals
import org.junit.Test

class EditsTest {
    @Test fun zusammenfuehren() {
        var t = Edits.merge("", Edit("a", "Alt", "X", "Cover/1.jpg"))
        t = Edits.merge(t, Edit("b", "Zwei\tmit Tab", "", ""))
        t = Edits.merge(t, Edit("a", "Neu", "Y", ""))
        assertEquals("a\tNeu\tY\tCover/1.jpg\nb\tZwei mit Tab\t\t\n", t)
        assertEquals("Neu", Edits.parse(t)["a"]?.title)
    }
}
