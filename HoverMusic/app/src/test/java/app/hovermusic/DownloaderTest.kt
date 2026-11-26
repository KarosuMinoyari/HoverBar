package app.hovermusic

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class DownloaderTest {
    @Test fun nurYoutubeLinks() {
        assertEquals("https://youtu.be/abc?si=1", Downloader.youtubeUrl("Schau mal https://youtu.be/abc?si=1 super"))
        assertEquals("https://music.youtube.com/watch?v=x", Downloader.youtubeUrl("https://music.youtube.com/watch?v=x"))
        assertNull(Downloader.youtubeUrl("https://evil.example/youtube.com/"))
        assertNull(Downloader.youtubeUrl("--exec rm"))
    }

    @Test fun suchtreffer() {
        val r = Downloader.parseSearch("WARNING: x\naaaaaaaaaaa|340.0|Kanal|Lied | Live\n")
        assertEquals(listOf(Found("aaaaaaaaaaa", "Lied | Live", "Kanal", 340)), r)
    }

    @Test fun infoJson() {
        val i = Downloader.parseInfo("""{"id":"abc","title":"Video","uploader":"Band - Topic","duration":125.4}""")
        assertEquals(SongInfo("abc", "Video", "Band", 125), i)
    }
}
