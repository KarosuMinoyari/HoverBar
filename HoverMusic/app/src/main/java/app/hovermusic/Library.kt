package app.hovermusic

import android.content.ContentResolver
import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import org.json.JSONObject

/** Ein Song aus HoverBar-Bibliothek.json. Die Uris zeigen auf die Dateien im gewählten Cloud-Ordner. */
data class Song(
    val id: String,
    val file: String,
    val title: String,
    val artist: String,
    val durationSec: Int,
    val uri: Uri?,
    val coverUri: Uri?,
    val lyricsUri: Uri?,
    val pcPlays: Int = 0,
    val trimFromMs: Long = 0,  // Schnitt vom Handy, den der PC noch nicht übernommen hat; 0/0 = keiner
    val trimToMs: Long = 0,
)

data class Playlist(val name: String, val songIds: List<String>)

data class Library(
    val songs: List<Song>,
    val playlists: List<Playlist>,
    val pcFavorites: List<String>,
    val updated: String,
) {
    val byId: Map<String, Song> = songs.associateBy { it.id }
}

/** Liest die Bibliotheksdatei, die HoverBar (PC) in den Cloud-Ordner schreibt, und ordnet die Dateien den Songs zu. */
object LibraryLoader {
    const val FILE_NAME = "HoverBar-Bibliothek.json"

    private data class Child(val name: String, val docId: String, val isDir: Boolean)

    private fun children(cr: ContentResolver, tree: Uri, parentDocId: String): List<Child> {
        val uri = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentDocId)
        val out = ArrayList<Child>()
        cr.query(
            uri,
            arrayOf(
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
            ),
            null, null, null,
        )?.use { c ->
            while (c.moveToNext()) {
                out.add(Child(c.getString(1) ?: "", c.getString(0), c.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR))
            }
        }
        return out
    }

    class Loaded(val library: Library, val rawJson: String, val folderId: String)

    /** Lädt die Bibliothek aus dem gewählten Ordner. Wirft eine Exception mit verständlicher Meldung. */
    fun load(ctx: Context, tree: Uri): Loaded {
        val cr = ctx.contentResolver
        val rootId = DocumentsContract.getTreeDocumentId(tree)
        var baseId = rootId
        var kids = children(cr, tree, rootId)
        var json = kids.firstOrNull { !it.isDir && it.name == FILE_NAME }
        if (json == null) {  // der Nutzer hat vielleicht den Elternordner gewählt
            val sub = kids.firstOrNull { it.isDir && it.name == "HoverBar-Musik" }
            if (sub != null) {
                baseId = sub.docId
                kids = children(cr, tree, baseId)
                json = kids.firstOrNull { !it.isDir && it.name == FILE_NAME }
            }
        }
        if (json == null) {
            throw IllegalStateException("In diesem Ordner liegt keine $FILE_NAME. Wähle den Ordner „HoverBar-Musik“ aus deinem Cloud-Ordner (Musik-Sync in HoverBar am PC einrichten).")
        }
        val text = cr.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, json.docId))
            ?.use { it.readBytes().toString(Charsets.UTF_8) }
            ?: throw IllegalStateException("Die Bibliotheksdatei konnte nicht gelesen werden.")
        val files = kids.filter { !it.isDir }.associate { it.name to DocumentsContract.buildDocumentUriUsingTree(tree, it.docId) }
        val coverDir = kids.firstOrNull { it.isDir && it.name == "Cover" }
        val covers = if (coverDir != null) {
            children(cr, tree, coverDir.docId).filter { !it.isDir }
                .associate { "Cover/" + it.name to DocumentsContract.buildDocumentUriUsingTree(tree, it.docId) }
        } else emptyMap()
        val lyricsDir = kids.firstOrNull { it.isDir && it.name == "Songtexte" }
        val lyrics = if (lyricsDir != null) {
            children(cr, tree, lyricsDir.docId).filter { !it.isDir }
                .associate { "Songtexte/" + it.name to DocumentsContract.buildDocumentUriUsingTree(tree, it.docId) }
        } else emptyMap()
        var library = parse(text, files + lyrics, covers)
        // Änderungen vom Handy, die der PC noch nicht übernommen hat
        files[Edits.FILE_NAME]?.let { u ->
            val edits = Edits.parse(cr.openInputStream(u)?.use { it.readBytes().toString(Charsets.UTF_8) } ?: "")
            library = library.copy(songs = library.songs.map { s ->
                val e = edits[s.id] ?: return@map s
                s.copy(title = e.title.ifEmpty { s.title }, artist = e.artist, coverUri = covers[e.cover] ?: s.coverUri, trimFromMs = e.fromMs, trimToMs = e.toMs)
            })
        }
        return Loaded(library, text, baseId)
    }

    /** Aus dem Zwischenspeicher (ohne Dateizugriff): Titel und Listen sofort, Dateien erst nach dem Aktualisieren. */
    fun parseCached(text: String): Library = parse(text, emptyMap(), emptyMap())

    internal fun parse(text: String, files: Map<String, Uri>, covers: Map<String, Uri>): Library {
        val root = JSONObject(text)
        val songs = ArrayList<Song>()
        val arr = root.optJSONArray("songs")
        if (arr != null) for (i in 0 until arr.length()) {
            val o = arr.getJSONObject(i)
            val file = o.optString("file")
            val cover = o.optString("cover")
            val lyrics = o.optString("lyrics")
            songs.add(
                Song(
                    id = o.getString("id"),
                    file = file,
                    title = o.optString("title").ifEmpty { file.removeSuffix(".mp3") },
                    artist = o.optString("artist"),
                    durationSec = o.optInt("duration"),
                    uri = files[file],
                    coverUri = if (cover.isNotEmpty()) covers[cover] else null,
                    lyricsUri = if (lyrics.isNotEmpty()) files[lyrics] else null,
                    pcPlays = o.optInt("plays"),
                ),
            )
        }
        val pls = ArrayList<Playlist>()
        root.optJSONArray("playlists")?.let { a ->
            for (i in 0 until a.length()) {
                val o = a.getJSONObject(i)
                val ids = ArrayList<String>()
                o.optJSONArray("songs")?.let { s -> for (k in 0 until s.length()) ids.add(s.getString(k)) }
                pls.add(Playlist(o.optString("name"), ids))
            }
        }
        val favs = ArrayList<String>()
        root.optJSONArray("favorites")?.let { a -> for (i in 0 until a.length()) favs.add(a.getString(i)) }
        return Library(songs, pls, favs, root.optString("updated"))
    }
}
