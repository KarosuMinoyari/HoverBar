package app.hovermusic

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Column
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import org.json.JSONObject
import java.io.File

/** Eine Zeile in HoverMusic-Aenderungen.txt; HoverBar am PC übernimmt sie und löscht die Datei. */
data class Edit(val id: String, val title: String, val artist: String, val cover: String)

object Edits {
    const val FILE_NAME = "HoverMusic-Aenderungen.txt"

    fun parse(text: String): Map<String, Edit> =
        text.lineSequence().map { it.trimEnd('\r').split('\t') }.filter { it.size >= 4 }
            .associate { it[0] to Edit(it[0], it[1], it[2], it[3]) }

    /** Neue Änderung dazunehmen; ein früheres Cover desselben Songs bleibt, wenn kein neues kommt. */
    fun merge(text: String, e: Edit): String {
        val all = parse(text).toMutableMap()
        fun clean(s: String) = s.replace(Regex("[\\t\\r\\n]"), " ").trim()
        all[e.id] = Edit(e.id, clean(e.title), clean(e.artist), e.cover.ifEmpty { all[e.id]?.cover ?: "" })
        return all.values.joinToString("") { "${it.id}\t${it.title}\t${it.artist}\t${it.cover}\n" }
    }

    private fun child(ctx: Context, tree: Uri, parent: String, name: String): Uri? {
        val cr = ctx.contentResolver
        cr.query(
            DocumentsContract.buildChildDocumentsUriUsingTree(tree, parent),
            arrayOf(DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME), null, null, null,
        )?.use { c ->
            while (c.moveToNext()) if (c.getString(1) == name) return DocumentsContract.buildDocumentUriUsingTree(tree, c.getString(0))
        }
        return null
    }

    private fun create(ctx: Context, tree: Uri, parent: String, mime: String, name: String): Uri =
        DocumentsContract.createDocument(ctx.contentResolver, DocumentsContract.buildDocumentUriUsingTree(tree, parent), mime, name)
            ?: error("Konnte $name nicht anlegen")

    private fun write(ctx: Context, uri: Uri, bytes: ByteArray) {
        ctx.contentResolver.openOutputStream(uri, "wt")?.use { it.write(bytes) } ?: error("Nicht beschreibbar")
    }

    /** Änderung eines PC-Songs in den Cloud-Ordner schreiben. [folderId] = Ordner mit der Bibliotheksdatei. */
    fun saveToCloud(ctx: Context, tree: Uri, folderId: String, id: String, title: String, artist: String, newCover: Uri?) {
        var cover = ""
        if (newCover != null) {
            val bytes = ctx.contentResolver.openInputStream(newCover)?.use { it.readBytes() } ?: error("Bild nicht lesbar")
            val dir = child(ctx, tree, folderId, "Cover") ?: create(ctx, tree, folderId, DocumentsContract.Document.MIME_TYPE_DIR, "Cover")
            val name = "hm_${System.currentTimeMillis()}.jpg"
            write(ctx, create(ctx, tree, DocumentsContract.getDocumentId(dir), "image/jpeg", name), bytes)
            cover = "Cover/$name"
        }
        val old = child(ctx, tree, folderId, FILE_NAME)
        val text = old?.let { u -> ctx.contentResolver.openInputStream(u)?.use { it.readBytes().toString(Charsets.UTF_8) } } ?: ""
        // octet-stream: sonst hängen manche Anbieter eine zweite Endung an
        val target = old ?: create(ctx, tree, folderId, "application/octet-stream", FILE_NAME)
        write(ctx, target, merge(text, Edit(id, title, artist, cover)).toByteArray(Charsets.UTF_8))
    }

    /** Änderung eines auf dem Handy geladenen Songs. */
    fun saveLocal(ctx: Context, id: String, title: String, artist: String, newCover: Uri?) {
        val dir = Downloader.dir(ctx)
        val key = id.removePrefix("yt:")
        val info = File(dir, "$key.info.json")
        val o = JSONObject(info.readText())
        o.put("track", title).put("artist", artist)
        info.writeText(o.toString())
        if (newCover != null) {
            val bytes = ctx.contentResolver.openInputStream(newCover)?.use { it.readBytes() } ?: error("Bild nicht lesbar")
            for (ext in listOf("jpg", "webp", "png")) File(dir, "$key.$ext").delete()
            File(dir, "$key.jpg").writeBytes(bytes)
        }
    }
}

@Composable
fun EditDialog(vm: MusicViewModel, song: Song, onClose: () -> Unit) {
    var title by remember(song.id) { mutableStateOf(song.title) }
    var artist by remember(song.id) { mutableStateOf(song.artist) }
    var cover by remember(song.id) { mutableStateOf<Uri?>(null) }
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.GetContent()) { cover = it }
    AlertDialog(
        onDismissRequest = onClose,
        title = { Text("Song bearbeiten") },
        text = {
            Column {
                OutlinedTextField(title, { title = it }, label = { Text("Titel") }, singleLine = true)
                OutlinedTextField(artist, { artist = it }, label = { Text("Künstler") }, singleLine = true)
                TextButton(onClick = { picker.launch("image/*") }) { Text(if (cover == null) "Cover wählen" else "Neues Cover gewählt ✓") }
            }
        },
        confirmButton = {
            TextButton(onClick = { if (title.isNotBlank()) { vm.editSong(song, title, artist, cover); onClose() } }) { Text("Speichern") }
        },
        dismissButton = { TextButton(onClick = onClose) { Text("Abbrechen") } },
    )
}
