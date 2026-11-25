package app.hovermusic

import org.junit.Assert.assertEquals
import org.junit.Test

class CutsTest {
    @Test fun schritteUndPositionen() {
        var c = Cuts.removeSelection("", 2000, 3000)
        c = Cuts.keepSelection(c, 1000, 8000, 10000)
        assertEquals(listOf(0L..1000L, 2000L..3000L, 8000L..10000L), Cuts.removed(c))
        assertEquals(6000L, Cuts.kept(c, 10000))
        assertEquals(1000L, Cuts.visible(c, 2500))
        assertEquals(3500L, Cuts.original(c, 1500))
        assertEquals("2000-3000", Cuts.undo(c))
        assertEquals("", Cuts.undo(Cuts.undo(c)))
    }
}
