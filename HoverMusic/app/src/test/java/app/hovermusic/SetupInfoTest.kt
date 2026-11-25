package app.hovermusic

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** Liest die Einrichtungs-Links, die HoverBar (PC) in den QR-Code schreibt (qr-samples.txt, Zeilen "U:"). */
class SetupInfoTest {
    private fun urls(): List<String> =
        javaClass.getResourceAsStream("/qr-samples.txt")!!.readBytes().toString(Charsets.UTF_8).lines()
            .filter { it.startsWith("U:") }.map { it.removePrefix("U:") }

    @Test
    fun readsTheLinksFromHoverBar() {
        val u = urls()
        assertEquals(3, u.size)
        val mega = SetupInfo.parse(u[0])
        assertNotNull(mega)
        assertEquals("MEGA", mega!!.cloud)
        assertEquals("HoverBar-Musik", mega.folder)
        assertEquals("mega.privacy.android.app", mega.pkg)
        assertEquals("", mega.parent)
        assertTrue(mega.knownCloud)

        val drive = SetupInfo.parse(u[1])!!
        assertEquals("Google Drive", drive.cloud)
        assertEquals("com.google.android.apps.docs", drive.pkg)
        assertEquals("Meine Ablage", drive.parent)

        val own = SetupInfo.parse(u[2])!!   // eigener Ordner, mit Umlauten und Anführungszeichen
        assertFalse(own.knownCloud)
        assertEquals("Müsik Ü \"Test\"", own.parent)
        assertEquals("", own.pkg)
    }

    @Test
    fun rejectsForeignInput() {
        assertNull(SetupInfo.parse("https://example.com/"))
        assertNull(SetupInfo.parse("https://github.com/x#hm=!!!kein-base64"))
        assertNull(SetupInfo.parse(""))
    }

    @Test
    fun survivesJsonRoundTrip() {
        val s = SetupInfo("MEGA", "HoverBar-Musik", "mega.privacy.android.app", "")
        assertEquals(s, SetupInfo.fromJson(s.toJson()))
    }
}
