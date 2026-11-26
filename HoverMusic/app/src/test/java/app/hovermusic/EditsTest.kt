package app.hovermusic

import org.junit.Assert.assertEquals
import org.junit.Test

class EditsTest {
    @Test fun playlists() {
        assertEquals(mapOf("Gym" to listOf("a", "b")), Edits.parsePlaylists("Gym\ta\r\nkaputt\nGym\tb\n"))
    }

    @Test fun zusammenfuehren() {
        var t = Edits.merge("", Edit("a", "Alt", "X", "Cover/1.jpg"))
        t = Edits.merge(t, Edit("b", "Zwei\tmit Tab", "", ""))
        t = Edits.merge(t, Edit("a", "Neu", "Y", ""))
        assertEquals("a\tNeu\tY\tCover/1.jpg\t\nb\tZwei mit Tab\t\t\t\n", t)
        t = Edits.merge(t, Edit("a", "Neu", "Y", "", "1000-5000"))
        t = Edits.merge(t, Edit("a", "Neu", "Y", ""), keepCuts = true)
        assertEquals("1000-5000", Edits.parse(t)["a"]?.cuts)
        assertEquals("Neu", Edits.parse(t)["a"]?.title)
    }
}
