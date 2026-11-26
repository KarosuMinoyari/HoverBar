package app.hovermusic

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.clickable
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import coil.compose.AsyncImage
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
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
data class Edit(val id: String, val title: String, val artist: String, val cover: String, val cuts: String = "")

object Edits {
    const val FILE_NAME = "HoverMusic-Aenderungen.txt"
    const val PLAYLIST_FILE = "HoverMusic-Playlists.txt"

    /** Zeilen "Playlist<Tab>Song-ID" -> Songs je Playlist. */
    fun parsePlaylists(text: String): Map<String, List<String>> =
        text.lineSequence().map { it.trimEnd('\r').split('\t') }.filter { it.size == 2 && it[0].isNotEmpty() }
            .groupBy({ it[0] }, { it[1] })

    fun addToPlaylist(ctx: Context, tree: Uri, folderId: String, playlist: String, ids: List<String>) {
        val old = child(ctx, tree, folderId, PLAYLIST_FILE)
        val text = old?.let { u -> ctx.contentResolver.openInputStream(u)?.use { it.readBytes().toString(Charsets.UTF_8) } } ?: ""
        val target = old ?: create(ctx, tree, folderId, "application/octet-stream", PLAYLIST_FILE)
        write(ctx, target, (text + ids.joinToString("") { "$playlist\t$it\n" }).toByteArray(Charsets.UTF_8))
    }

    const val DELETE = "DELETE"  // steht in der Cover-Spalte: der PC soll den Song löschen

    fun parse(text: String): Map<String, Edit> =
        text.lineSequence().map { it.trimEnd('\r').split('\t') }.filter { it.size >= 4 }
            .associate { it[0] to Edit(it[0], it[1], it[2], it[3], it.getOrNull(4) ?: "") }

    /** Neue Änderung dazunehmen; ein früheres Cover desselben Songs bleibt, wenn kein neues kommt. */
    fun merge(text: String, e: Edit, keepCuts: Boolean = false): String {
        val all = parse(text).toMutableMap()
        fun clean(s: String) = s.replace(Regex("[\\t\\r\\n]"), " ").trim()
        val old = all[e.id]
        val cuts = if (keepCuts) old?.cuts ?: "" else e.cuts
        all[e.id] = Edit(e.id, clean(e.title), clean(e.artist), e.cover.ifEmpty { old?.cover ?: "" }, cuts)
        return all.values.joinToString("") { "${it.id}\t${it.title}\t${it.artist}\t${it.cover}\t${it.cuts}\n" }
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

    /** Änderung eines PC-Songs in den Cloud-Ordner schreiben. [folderId] = Ordner mit der Bibliotheksdatei. [cuts] null = Schnitt unverändert. */
    fun saveToCloud(ctx: Context, tree: Uri, folderId: String, id: String, title: String, artist: String, newCover: Uri?, cuts: String?) {
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
        val edit = Edit(id, title, artist, cover, cuts ?: "")
        write(ctx, target, merge(text, edit, keepCuts = cuts == null).toByteArray(Charsets.UTF_8))
    }

    /** PC-Song zum Löschen vormerken; HoverBar entfernt Datei und Eintrag. */
    fun deleteInCloud(ctx: Context, tree: Uri, folderId: String, id: String, title: String, artist: String) {
        val old = child(ctx, tree, folderId, FILE_NAME)
        val text = old?.let { u -> ctx.contentResolver.openInputStream(u)?.use { it.readBytes().toString(Charsets.UTF_8) } } ?: ""
        val target = old ?: create(ctx, tree, folderId, "application/octet-stream", FILE_NAME)
        write(ctx, target, merge(text, Edit(id, title, artist, DELETE)).toByteArray(Charsets.UTF_8))
    }

    /** Handy-Download entfernen: Dateien in der App und, falls schon hochgeladen und noch nicht eingelesen, die Kopie in der Cloud. */
    fun deleteDownload(ctx: Context, marker: String, tree: Uri?, folderId: String?) {
        val key = marker.removePrefix("[yt ").removeSuffix("]")
        Downloader.dir(ctx).listFiles()?.filter { it.name.startsWith("$key.") }?.forEach { it.delete() }
        if (tree == null || folderId == null) return
        ctx.contentResolver.query(
            DocumentsContract.buildChildDocumentsUriUsingTree(tree, folderId),
            arrayOf(DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME), null, null, null,
        )?.use { c ->
            while (c.moveToNext()) if (c.getString(1)?.contains(marker) == true) {
                DocumentsContract.deleteDocument(ctx.contentResolver, DocumentsContract.buildDocumentUriUsingTree(tree, c.getString(0)))
            }
        }
    }

    /**
     * Wurde ein Handy-Download bearbeitet, bevor der PC ihn eingelesen hatte, steht die Änderung nur auf dem Handy.
     * Sobald der Song in der Bibliothek vom PC auftaucht, wird sie nachgereicht. Liefert die Anzahl.
     */
    fun forwardDownloadEdits(ctx: Context, tree: Uri, folderId: String, local: List<Song>, cloud: Library): Int {
        var count = 0
        for (s in local) {
            val info = File(Downloader.dir(ctx), s.id.removePrefix("yt:") + ".info.json")
            val o = runCatching { JSONObject(info.readText()) }.getOrNull() ?: continue
            if (!o.optBoolean("hm_edited")) continue
            val onPc = cloud.songs.firstOrNull { Upload.marker(s.id) in it.file } ?: continue
            saveToCloud(ctx, tree, folderId, onPc.id, s.title, s.artist, if (o.optBoolean("hm_cover")) s.coverUri else null, s.cuts.ifEmpty { null })
            o.remove("hm_edited")
            o.remove("hm_cover")
            info.writeText(o.toString())
            count++
        }
        return count
    }

    /** Änderung eines auf dem Handy geladenen Songs. */
    fun saveLocal(ctx: Context, id: String, title: String, artist: String, newCover: Uri?, cuts: String?) {
        val dir = Downloader.dir(ctx)
        val key = id.removePrefix("yt:")
        val info = File(dir, "$key.info.json")
        val o = JSONObject(info.readText())
        o.put("track", title).put("artist", artist)
        o.put("hm_edited", true)  // noch an den PC weiterzugeben, sobald er den Song eingelesen hat
        if (newCover != null) o.put("hm_cover", true)
        if (cuts != null) o.put("hm_cuts", cuts)
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
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    var found by remember(song.id) { mutableStateOf<List<String>>(emptyList()) }
    var searchText by remember(song.id) { mutableStateOf<String?>(null) }
    var cuts by remember(song.id) { mutableStateOf(song.cuts) }
    var cutting by remember(song.id) { mutableStateOf(false) }
    if (cutting) CutScreen(song, cuts, onDone = { cuts = it; cutting = false }, onCancel = { cutting = false })
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.GetContent()) { cover = it }
    var confirmDelete by remember(song.id) { mutableStateOf(false) }
    if (confirmDelete) {
        AlertDialog(
            onDismissRequest = { confirmDelete = false },
            title = { Text("Song löschen?") },
            text = {
                Text(
                    if (song.id.startsWith("yt:")) "„${song.title}“ wird vom Handy gelöscht."
                    else "„${song.title}“ verschwindet hier. HoverBar löscht die Datei beim nächsten Öffnen auch am PC. Das lässt sich nicht rückgängig machen.",
                )
            },
            confirmButton = { TextButton(onClick = { vm.deleteSong(song); confirmDelete = false; onClose() }) { Text("Löschen") } },
            dismissButton = { TextButton(onClick = { confirmDelete = false }) { Text("Abbrechen") } },
        )
    }
    AlertDialog(
        onDismissRequest = onClose,
        title = { Text("Song bearbeiten") },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                OutlinedTextField(title, { title = it }, label = { Text("Titel") }, singleLine = true)
                OutlinedTextField(artist, { artist = it }, label = { Text("Künstler") }, singleLine = true)
                TextButton(onClick = { picker.launch("image/*") }) { Text(if (cover == null) "Cover vom Handy" else "Neues Cover gewählt ✓") }
                TextButton(onClick = {
                    searchText = "Suche …"
                    scope.launch {
                        val r = withContext(Dispatchers.IO) { runCatching { CoverSearch.search(title, artist) } }
                        found = r.getOrDefault(emptyList())
                        searchText = if (r.isFailure) "Keine Verbindung." else if (found.isEmpty()) "Nichts gefunden." else "Antippen zum Wählen:"
                    }
                }) { Text("Cover im Web suchen") }
                searchText?.let { Text(it, fontSize = 13.sp) }
                for (row in found.chunked(3)) Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    for (url in row) AsyncImage(
                        model = url, contentDescription = "Cover-Vorschlag", contentScale = ContentScale.Crop,
                        modifier = Modifier.weight(1f).aspectRatio(1f).padding(bottom = 6.dp).clip(RoundedCornerShape(10.dp)).clickable {
                            scope.launch {
                                val r = withContext(Dispatchers.IO) { runCatching { CoverSearch.download(ctx, url) } }
                                r.onSuccess { cover = it; found = emptyList(); searchText = null }
                                r.onFailure { searchText = "Bild konnte nicht geladen werden." }
                            }
                        },
                    )
                    repeat(3 - row.size) { Spacer(Modifier.weight(1f)) }
                }
                TextButton(onClick = { confirmDelete = true }) { Text("Song löschen", color = Color(0xFFFF8080)) }
                if (song.uri != null) {
                    Button(onClick = { vm.pause(); cutting = true }, modifier = Modifier.fillMaxWidth()) { Text("Audio schneiden") }
                    if (cuts != song.cuts) Text("Schnitt wird mit „Speichern“ übernommen.", fontSize = 12.sp)
                    if (!song.id.startsWith("yt:")) Text("Am PC schneidet HoverBar die Datei beim nächsten Öffnen (dort rückgängig machbar).", fontSize = 12.sp)
                }
            }
        },
        confirmButton = {
            TextButton(onClick = {
                if (title.isBlank()) return@TextButton
                vm.editSong(song, title, artist, cover, cuts.takeIf { it != song.cuts })
                onClose()
            }) { Text("Speichern") }
        },
        dismissButton = { TextButton(onClick = onClose) { Text("Abbrechen") } },
    )
}
