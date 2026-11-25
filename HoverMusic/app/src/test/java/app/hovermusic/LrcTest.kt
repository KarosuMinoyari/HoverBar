package app.hovermusic

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class LrcTest {
    @Test
    fun parsesTimedLinesAndSortsThem() {
        val l = Lyrics.parse("[ar:Test]\n[00:12.50]zweite\n[00:01.00]erste\n[01:02.250]dritte\n")
        assertTrue(l.synced)
        assertEquals(listOf("erste", "zweite", "dritte"), l.lines.map { it.text })
        assertEquals(listOf(1000L, 12500L, 62250L), l.lines.map { it.timeMs })
    }

    @Test
    fun currentLineFollowsPosition() {
        val l = Lyrics.parse("[00:05.00]a\n[00:10.00]b\n[00:20.00]c")
        assertEquals(-1, l.indexAt(1000))
        assertEquals(0, l.indexAt(5000))
        assertEquals(1, l.indexAt(15000))
        assertEquals(2, l.indexAt(99000))
    }

    @Test
    fun multipleStampsOnOneLineRepeatTheText() {
        val l = Lyrics.parse("[00:10.00][00:30.00]Refrain\n[00:20.00]Strophe")
        assertEquals(listOf("Refrain", "Strophe", "Refrain"), l.lines.map { it.text })
    }

    @Test
    fun plainTextWithoutStampsIsNotSynced() {
        val l = Lyrics.parse("Zeile eins\nZeile zwei\n\n[ti:Titel]\n")
        assertFalse(l.synced)
        assertEquals(listOf("Zeile eins", "Zeile zwei"), l.lines.map { it.text })
        assertEquals(-1, l.indexAt(5000))
    }
}
