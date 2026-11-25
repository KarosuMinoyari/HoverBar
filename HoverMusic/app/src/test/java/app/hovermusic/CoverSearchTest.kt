package app.hovermusic

import org.junit.Assert.assertEquals
import org.junit.Test

class CoverSearchTest {
    @Test fun antworten() {
        assertEquals(listOf("https://x/1.jpg"), CoverSearch.deezer("""{"data":[{"album":{"cover_xl":"https://x/1.jpg"}},{"album":{}}]}"""))
        assertEquals(listOf("https://x/600x600bb.jpg"), CoverSearch.itunes("""{"results":[{"artworkUrl100":"https://x/100x100bb.jpg"}]}"""))
    }
}
