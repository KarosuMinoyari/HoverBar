package app.hovermusic

import org.junit.Assert.assertEquals
import org.junit.Test

class SuggestTest {
    @Test fun deezerAntworten() {
        assertEquals(listOf(27L), Suggestions.ids("""{"data":[{"id":27},{"name":"x"}]}"""))
        val t = Suggestions.tracks("""{"data":[{"title":"A","duration":100,"preview":"https://p","artist":{"name":"B"},"album":{"cover_medium":"https://c"}},{"title":"ohne"}]}""")
        assertEquals(listOf(Idea("A", "B", 100, "https://p", "https://c")), t)
    }

    @Test fun passendeLaenge() {
        val hits = listOf(Found("aaaaaaaaaaa", "x", "y", 200), Found("bbbbbbbbbbb", "x", "y", 104))
        assertEquals("bbbbbbbbbbb", Downloader.pickByDuration(hits, 100)?.id)
        assertEquals(null, Downloader.pickByDuration(hits, 150))
    }
}
