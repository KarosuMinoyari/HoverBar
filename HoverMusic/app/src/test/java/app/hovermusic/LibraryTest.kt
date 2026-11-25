package app.hovermusic

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** Prüft, dass das Format, das HoverBar (PC) schreibt, richtig gelesen wird (Beispiel wie von HoverBar erzeugt). */
class LibraryTest {
    private val sample = """
{
  "format": "HoverBar-Bibliothek",
  "version": 1,
  "updated": "2026-11-25T12:27:56",
  "songs": [
    {"id":"IDaP8LeYsrk","file":"Creo - Crazy.mp3","title":"Crazy","artist":"Creo","duration":216,"cover":"Cover/IDaP8LeYsrk_539913218.jpg","lyrics":"Creo - Crazy.lrc","url":"https://www.youtube.com/watch?v=IDaP8LeYsrk"},
    {"id":"x2","file":"Ohne Cover [Release].mp3","title":"Quote \"Test\" \\ Pfad","artist":"","duration":0,"cover":"","lyrics":"","url":""}
  ],
  "playlists": [
    {"name":"EDM/Techno","songs":["IDaP8LeYsrk","x2"]},
    {"name":"Leer","songs":[]}
  ],
  "favorites": ["x2"]
}
""".trimIndent()

    @Test
    fun readsSongsPlaylistsAndFavorites() {
        val lib = LibraryLoader.parse(sample, emptyMap(), emptyMap())
        assertEquals(2, lib.songs.size)
        assertEquals("Crazy", lib.byId["IDaP8LeYsrk"]?.title)
        assertEquals(216, lib.byId["IDaP8LeYsrk"]?.durationSec)
        assertEquals("Quote \"Test\" \\ Pfad", lib.byId["x2"]?.title)
        assertEquals(listOf("EDM/Techno", "Leer"), lib.playlists.map { it.name })
        assertEquals(listOf("IDaP8LeYsrk", "x2"), lib.playlists[0].songIds)
        assertEquals(listOf("x2"), lib.pcFavorites)
        assertEquals("2026-11-25T12:27:56", lib.updated)
    }

    @Test
    fun songWithoutFilesHasNoUris() {
        val lib = LibraryLoader.parse(sample, emptyMap(), emptyMap())
        assertNull(lib.byId["IDaP8LeYsrk"]?.uri)
        assertNull(lib.byId["x2"]?.coverUri)
    }
}
