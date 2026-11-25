package app.hovermusic

import org.junit.Assert.assertEquals
import org.junit.Test

class UploadTest {
    @Test fun dateiname() {
        val s = Song("yt:ab_1", "ab_1.m4a", "Was? / Ein:Titel", "AC/DC", 1, null, null, null)
        assertEquals("AC DC - Was    Ein Titel [yt ab_1].m4a", Upload.fileName(s))
    }
}
