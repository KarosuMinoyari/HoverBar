package app.hovermusic

import android.content.Context
import android.media.AudioAttributes
import android.media.MediaPlayer
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import coil.compose.AsyncImage
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

data class Idea(val title: String, val artist: String, val seconds: Int, val preview: String, val cover: String)

/** Vorschläge wie am PC: ähnliche Künstler zu den meistgehörten (Deezer, ohne Konto; übertragen werden nur Künstlernamen). */
object Suggestions {
    val list = MutableStateFlow<List<Idea>?>(null)
    val busy = MutableStateFlow(false)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    fun ids(json: String): List<Long> {
        val a = JSONObject(json).optJSONArray("data") ?: return emptyList()
        return (0 until a.length()).map { a.getJSONObject(it).optLong("id") }.filter { it > 0 }
    }

    fun tracks(json: String): List<Idea> {
        val a = JSONObject(json).optJSONArray("data") ?: return emptyList()
        return (0 until a.length()).mapNotNull {
            val t = a.getJSONObject(it)
            val preview = t.optString("preview")
            if (t.optString("title").isEmpty() || !preview.startsWith("https://")) null
            else Idea(
                t.optString("title"), t.optJSONObject("artist")?.optString("name").orEmpty(), t.optInt("duration"), preview,
                t.optJSONObject("album")?.optString("cover_medium")?.takeIf { c -> c.startsWith("https://") }.orEmpty(),
            )
        }
    }

    /** Die meistgehörten Künstler (Wiedergaben zählen, sonst die Anzahl der Songs). */
    fun topArtists(songs: List<Song>, plays: (Song) -> Int, count: Int = 5): List<String> =
        songs.mapNotNull { s -> s.artist.split(',', '&', ';').first().trim().takeIf { it.isNotEmpty() }?.let { it to 1 + plays(s) } }
            .groupBy({ it.first.lowercase() }, { it })
            .map { (_, v) -> v.first().first to v.sumOf { it.second } }
            .sortedByDescending { it.second }
            .take(count)
            .map { it.first }

    private fun get(url: String): String {
        val c = URL(url).openConnection() as HttpURLConnection
        c.connectTimeout = 10000
        c.readTimeout = 15000
        return c.inputStream.use { it.readBytes().toString(Charsets.UTF_8) }
    }

    private val shown = HashSet<String>()  // in dieser Sitzung schon vorgeschlagen

    fun load(artists: List<String>, owned: Set<String>) {
        if (busy.value) return
        busy.value = true
        scope.launch {
            val seen = HashSet(owned + shown)  // beim Neuladen andere Songs
            val out = ArrayList<Idea>()
            fun add(artistId: Long, take: Int) {
                tracks(get("https://api.deezer.com/artist/$artistId/top?limit=30")).shuffled()
                    .filter { seen.add(it.title.lowercase()) }.take(take).forEach { out.add(it) }
            }
            for (name in artists.shuffled().take(5)) runCatching {
                val id = ids(get("https://api.deezer.com/search/artist?limit=1&q=" + URLEncoder.encode(name, "UTF-8"))).firstOrNull() ?: return@runCatching
                ids(get("https://api.deezer.com/artist/$id/related?limit=20")).shuffled().take(3).forEach { add(it, 2) }
                add(id, 1)
            }
            out.forEach { shown.add(it.title.lowercase()) }
            if (out.isEmpty()) shown.clear()  // alles durch: beim nächsten Mal von vorn
            list.value = out
            busy.value = false
        }
    }
}

@Composable
fun SuggestScreen(vm: MusicViewModel, onClose: () -> Unit) {
    val ctx = LocalContext.current
    val library by vm.library.collectAsState()
    val ideas by Suggestions.list.collectAsState()
    val busy by Suggestions.busy.collectAsState()
    val matched by Downloader.matched.collectAsState()
    val progress by Downloader.progress.collectAsState()
    var playing by remember { mutableStateOf<String?>(null) }
    val player = remember { MediaPlayer().apply { setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build()) } }
    DisposableEffect(Unit) { onDispose { player.release() } }
    BackHandler { onClose() }
    // eigener Song läuft an (auch über Benachrichtigung oder Widget): Hörprobe aus
    val own by vm.isPlaying.collectAsState()
    LaunchedEffect(own) {
        if (own && playing != null) {
            player.reset()
            playing = null
        }
    }

    fun reload() {
        val songs = library?.songs.orEmpty()
        Suggestions.load(Suggestions.topArtists(songs, { vm.plays(it) }, 12), songs.map { it.title.lowercase() }.toSet())
    }
    LaunchedEffect(library != null) { if (ideas == null && library != null) reload() }

    fun preview(url: String) {
        player.reset()
        if (playing == url) {
            playing = null
            return
        }
        playing = url
        vm.pause()
        runCatching {
            player.setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build())
            player.setDataSource(url)
            player.setOnPreparedListener { it.start() }
            player.setOnCompletionListener { playing = null }
            player.prepareAsync()
        }.onFailure { playing = null }
    }

    val look = vm.look.value
    var query by remember { mutableStateOf("") }
    val results by Downloader.results.collectAsState()
    val searching by Downloader.searching.collectAsState()
    val focus = androidx.compose.ui.platform.LocalFocusManager.current
    fun closeSearch() {
        Downloader.results.value = null
        query = ""
    }
    BackHandler(enabled = results != null) { closeSearch() }
    Column(Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background).pointerInput(Unit) { detectTapGestures { } }) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            IconButton(onClick = onClose) { Icon(HmIcons.Back, "Zurück") }
            Text("Vorschläge", fontSize = 24.sp, fontWeight = FontWeight.Bold, modifier = Modifier.weight(1f))
            TextButton(onClick = { player.reset(); playing = null; reload() }) { Text("Neue Songs") }
        }
        androidx.compose.material3.OutlinedTextField(
            value = query, onValueChange = { query = it }, singleLine = true,
            placeholder = { Text("Song auf YouTube suchen") },
            shape = RoundedCornerShape(26.dp),
            keyboardOptions = androidx.compose.foundation.text.KeyboardOptions(imeAction = androidx.compose.ui.text.input.ImeAction.Search),
            keyboardActions = androidx.compose.foundation.text.KeyboardActions(onSearch = { focus.clearFocus(); player.reset(); playing = null; Downloader.search(ctx, query) }),
            trailingIcon = { if (results != null) TextButton(onClick = { closeSearch() }) { Text("✕") } },
            modifier = Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, bottom = 8.dp),
        )
        val hits = results
        if (hits != null) {  // eigene Suche statt der Vorschläge
            SearchResults(hits, searching) { Downloader.download(ctx, "https://youtu.be/${it.id}", toast = true) }
            return@Column
        }
        Row(Modifier.fillMaxWidth().padding(horizontal = 20.dp), verticalAlignment = Alignment.CenterVertically) {
            Text("Album-Cover zeigen", Modifier.weight(1f), fontSize = 14.sp)
            Switch(look.suggestCovers, { vm.setLook(look.copy(suggestCovers = it)) })
        }
        if (busy) LinearProgressIndicator(Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 8.dp))
        val items = ideas.orEmpty()
        if (!busy && items.isEmpty()) {
            Text("Nichts gefunden. Dafür braucht es Songs mit Künstler und Internet.", Modifier.padding(20.dp), color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        LazyColumn(Modifier.fillMaxSize()) {
            items(items, key = { it.preview }) { idea ->
                val video = matched[idea.preview]
                val p = if (video == "") -1f else video?.let { progress[it] }
                Column(Modifier.fillMaxWidth().clickable { preview(idea.preview) }.padding(horizontal = 16.dp, vertical = 8.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        if (look.suggestCovers && idea.cover.isNotEmpty()) {
                            AsyncImage(idea.cover, null, Modifier.size(52.dp).clip(RoundedCornerShape(12.dp)), contentScale = ContentScale.Crop)
                            Spacer(Modifier.width(12.dp))
                        }
                        Icon(if (playing == idea.preview) HmIcons.Pause else HmIcons.Play, "Hörprobe", Modifier.size(22.dp))
                        Spacer(Modifier.width(10.dp))
                        Column(Modifier.weight(1f)) {
                            Text(idea.title, maxLines = 1, overflow = TextOverflow.Ellipsis, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
                            Text(idea.artist, maxLines = 1, overflow = TextOverflow.Ellipsis, fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                        if (p == 1f) Text("✓", fontSize = 20.sp, color = MaterialTheme.colorScheme.primary)
                        else IconButton(onClick = { Downloader.downloadMatch(ctx, idea) }, enabled = video == null) {
                            Icon(HmIcons.Download, "Laden", tint = MaterialTheme.colorScheme.primary)
                        }
                    }
                    if (p != null && p < 1f) {
                        val bar = Modifier.fillMaxWidth().padding(top = 8.dp).height(4.dp).clip(CircleShape)
                        if (p <= 0f) LinearProgressIndicator(bar) else LinearProgressIndicator(progress = { p }, modifier = bar)
                    }
                }
            }
        }
    }
}
