package app.hovermusic

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class UpdaterTest {
    @Test fun neuerungen() {
        val json = """[{"tag_name":"hovermusic-v1.2.0","body":"**Neu:** A\n\n**Full Changelog**: x"},{"tag_name":"hovermusic-v1.1.0","body":"B"},{"tag_name":"v9.0.0","body":"PC"}]"""
        assertEquals("1.2.0\nNeu: A", Updater.notesSince(json, "1.1.0"))
    }

    @Test fun versionen() {
        assertTrue(Updater.isNewer("1.0.0", "0.4.0"))
        assertTrue(Updater.isNewer("1.10", "1.9.3"))
        assertFalse(Updater.isNewer("1.0.0", "1.0"))
    }

    @Test fun neuesteApp() {
        fun rel(tag: String, pre: Boolean, name: String, host: String = "https://github.com/KarosuMinoyari/HoverBar/releases/download/") =
            """{"tag_name":"$tag","prerelease":$pre,"assets":[{"name":"$name","browser_download_url":"$host$tag/$name","digest":"sha256:ab"}]}"""
        val json = "[" + listOf(
            rel("v9.0.0", false, "HoverBar.exe"),
            rel("hovermusic-v1.0.0", false, "HoverMusic-1.0.0.apk"),
            rel("hovermusic-v2.0.0", true, "HoverMusic-2.0.0.apk"),
            rel("hovermusic-v3.0.0", false, "HoverMusic-3.0.0.apk", "https://evil.example/"),
        ).joinToString(",") + "]"
        assertEquals("1.0.0", Updater.newest(json)?.version)
    }
}
