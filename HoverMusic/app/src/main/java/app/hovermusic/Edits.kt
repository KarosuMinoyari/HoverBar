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
import androidx.compose.material3.RangeSlider
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
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
data class Edit(val id: String, val title: String, val artist: String, val cover: String, val fromMs: Long = 0, val toMs: Long = 0)

object Edits {
    const val FILE_NAME = "HoverMusic-Aenderungen.txt"

    fun parse(text: String): Map<String, Edit> =
        text.lineSequence().map { it.trimEnd('\r').split('\t') }.filter { it.size >= 4 }
            .associate { it[0] to Edit(it[0], it[1], it[2], it[3], it.getOrNull(4)?.toLongOrNull() ?: 0, it.getOrNull(5)?.toLongOrNull() ?: 0) }

    /** Neue Änderung dazunehmen; ein früheres Cover desselben Songs bleibt, wenn kein neues kommt. */
    fun merge(text: String, e: Edit, keepTrim: Boolean = false): String {
        val all = parse(text).toMutableMap()
        fun clean(s: String) = s.replace(Regex("[\\t\\r\\n]"), " ").trim()
        val old = all[e.id]
        val trim = if (keepTrim) old else e
        all[e.id] = Edit(e.id, clean(e.title), clean(e.artist), e.cover.ifEmpty { old?.cover ?: "" }, trim?.fromMs ?: 0, trim?.toMs ?: 0)
        return all.values.joinToString("") { "${it.id}\t${it.title}\t${it.artist}\t${it.cover}\t${it.fromMs}\t${it.toMs}\n" }
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

    /** Änderung eines PC-Songs in den Cloud-Ordner schreiben. [folderId] = Ordner mit der Bibliotheksdatei. [trim] null = Schnitt unverändert. */
    fun saveToCloud(ctx: Context, tree: Uri, folderId: String, id: String, title: String, artist: String, newCover: Uri?, trim: Pair<Long, Long>?) {
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
        val edit = Edit(id, title, artist, cover, trim?.first ?: 0, trim?.second ?: 0)
        write(ctx, target, merge(text, edit, keepTrim = trim == null).toByteArray(Charsets.UTF_8))
    }

    /** Änderung eines auf dem Handy geladenen Songs. */
    fun saveLocal(ctx: Context, id: String, title: String, artist: String, newCover: Uri?, trim: Pair<Long, Long>?) {
        val dir = Downloader.dir(ctx)
        val key = id.removePrefix("yt:")
        val info = File(dir, "$key.info.json")
        val o = JSONObject(info.readText())
        o.put("track", title).put("artist", artist)
        if (trim != null) o.put("hm_from", trim.first).put("hm_to", trim.second)
        info.writeText(o.toString())
        if (newCover != null) {
            val bytes = ctx.contentResolver.openInputStream(newCover)?.use { it.readBytes() } ?: error("Bild nicht lesbar")
            for (ext in listOf("jpg", "webp", "png")) File(dir, "$key.$ext").delete()
            File(dir, "$key.jpg").writeBytes(bytes)
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun EditDialog(vm: MusicViewModel, song: Song, onClose: () -> Unit) {
    var title by remember(song.id) { mutableStateOf(song.title) }
    var artist by remember(song.id) { mutableStateOf(song.artist) }
    var cover by remember(song.id) { mutableStateOf<Uri?>(null) }
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    var found by remember(song.id) { mutableStateOf<List<String>>(emptyList()) }
    var searchText by remember(song.id) { mutableStateOf<String?>(null) }
    // Schnitt in Sekunden
    val length = song.durationSec.toFloat()
    val start = song.trimFromMs / 1000f..(if (song.trimToMs > 0) song.trimToMs / 1000f else length)
    var range by remember(song.id) { mutableStateOf(start) }
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.GetContent()) { cover = it }
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
                if (length > 1) {
                    Text("Schneiden: ${fmtTime((range.start * 1000).toLong())} – ${fmtTime((range.endInclusive * 1000).toLong())}")
                    RangeSlider(range, { if (it.endInclusive - it.start >= 1f) range = it }, valueRange = 0f..length)
                    if (!song.id.startsWith("yt:")) Text("Am PC schneidet HoverBar die Datei beim nächsten Öffnen (dort rückgängig machbar).", fontSize = 12.sp)
                }
            }
        },
        confirmButton = {
            TextButton(onClick = {
                if (title.isBlank()) return@TextButton
                val full = range.start < 0.5f && range.endInclusive > length - 0.5f
                val trim = if (range == start) null else if (full) 0L to 0L else (range.start * 1000).toLong() to (range.endInclusive * 1000).toLong()
                vm.editSong(song, title, artist, cover, trim)
                onClose()
            }) { Text("Speichern") }
        },
        dismissButton = { TextButton(onClick = onClose) { Text("Abbrechen") } },
    )
}
