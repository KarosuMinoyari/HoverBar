package app.hovermusic

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import org.json.JSONObject
import java.io.File

/**
 * Auf dem Handy geladene Songs in den Cloud-Ordner legen, damit HoverBar am PC sie einlesen kann.
 * Der Name "<Künstler> - <Titel> [yt <id>].m4a" ist das Austauschformat; das Cover heißt in Cover/ genauso.
 */
object Upload {
    fun marker(localId: String) = "[yt ${localId.removePrefix("yt:")}]"

    fun fileName(s: Song): String {
        val name = listOf(s.artist, s.title).filter { it.isNotBlank() }.joinToString(" - ")
        return name.replace(Regex("""[\\/:*?"<>|\u0000-\u001f]"""), " ").trim().take(100).trim() + " " + marker(s.id) + ".m4a"
    }

    /**
     * Merker je Download, nicht je Video: "epoch" ist der Zeitpunkt, zu dem yt-dlp den Song geholt hat.
     * Neu geladen (nach dem Löschen) wird also wieder hochgeladen.
     */
    private fun key(ctx: Context, s: Song): String {
        val info = File(Downloader.dir(ctx), s.id.removePrefix("yt:") + ".info.json")
        return s.id + "@" + runCatching { JSONObject(info.readText()).optLong("epoch") }.getOrDefault(0L)
    }

    /** Jeden Download nur einmal: wer ihn am PC löscht, bekommt ihn nicht zurück. */
    fun pending(ctx: Context, local: List<Song>, cloud: Library): List<Song> {
        val done = ctx.getSharedPreferences("upload", Context.MODE_PRIVATE)
        return local.filter { s -> !done.getBoolean(key(ctx, s), false) && cloud.songs.none { marker(s.id) in it.file } }
    }

    fun upload(ctx: Context, tree: Uri, folderId: String, s: Song) {
        val cr = ctx.contentResolver
        fun write(parent: String, name: String, source: File) {
            val doc = DocumentsContract.createDocument(cr, DocumentsContract.buildDocumentUriUsingTree(tree, parent), "application/octet-stream", name)
                ?: error("Konnte $name nicht anlegen")
            try {
                cr.openOutputStream(doc, "w")?.use { out -> source.inputStream().use { it.copyTo(out) } } ?: error("Nicht beschreibbar")
            } catch (e: Exception) {
                runCatching { DocumentsContract.deleteDocument(cr, doc) }  // keine halbe Datei liegen lassen
                throw e
            }
        }
        val audio = File(s.uri?.path ?: return)
        val file = fileName(s)
        s.coverUri?.path?.let { path ->  // Cover zuerst, damit es beim Einlesen schon da ist
            val cover = File(path)
            var dir: String? = null
            cr.query(
                DocumentsContract.buildChildDocumentsUriUsingTree(tree, folderId),
                arrayOf(DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME), null, null, null,
            )?.use { c -> while (c.moveToNext()) if (c.getString(1) == "Cover") dir = c.getString(0) }
            val coverDir = dir ?: DocumentsContract.getDocumentId(
                DocumentsContract.createDocument(cr, DocumentsContract.buildDocumentUriUsingTree(tree, folderId), DocumentsContract.Document.MIME_TYPE_DIR, "Cover")
                    ?: error("Konnte Cover nicht anlegen"),
            )
            write(coverDir, file.removeSuffix(".m4a") + "." + cover.extension, cover)
        }
        write(folderId, file, audio)
        ctx.getSharedPreferences("upload", Context.MODE_PRIVATE).edit().putBoolean(key(ctx, s), true).apply()
    }
}
