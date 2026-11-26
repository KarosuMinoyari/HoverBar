package app.hovermusic

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
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
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.HorizontalDivider
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.combinedClickable
import androidx.compose.material3.TextButton
import androidx.compose.material3.Text
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.runtime.remember
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import coil.compose.AsyncImage

fun fmtTime(ms: Long): String {
    val s = (ms / 1000).coerceAtLeast(0)
    return "%d:%02d".format(s / 60, s % 60)
}

@Composable
fun Cover(song: Song?, size: androidx.compose.ui.unit.Dp?, modifier: Modifier = Modifier, radius: Int = 10) {
    val m = (if (size != null) modifier.size(size) else modifier).clip(RoundedCornerShape(radius.dp))
    if (song?.coverUri != null) {
        AsyncImage(model = song.coverUri, contentDescription = null, contentScale = ContentScale.Crop, modifier = m)
    } else {
        Box(m.background(MaterialTheme.colorScheme.surfaceVariant), contentAlignment = Alignment.Center) {
            Icon(HmIcons.Note, null, tint = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.size(28.dp))
        }
    }
}

@Composable
fun LibraryScreen(vm: MusicViewModel, onOpenNow: () -> Unit, onSettings: () -> Unit, onPickFolder: () -> Unit) {
    val library by vm.library.collectAsState()
    val loading by vm.loading.collectAsState()
    val error by vm.error.collectAsState()
    val favorites by vm.favorites.collectAsState()
    val recent by vm.recent.collectAsState()
    val currentId by vm.currentId.collectAsState()
    val byPlays by vm.sortByPlays.collectAsState()
    val playsTick by vm.playsTick.collectAsState()
    val download by Downloader.status.collectAsState()
    var tab by rememberSaveable { mutableIntStateOf(0) }  // 0 Alle Songs, 1 Favoriten, 2 Playlists, 3 Zuletzt
    var openPlaylist by rememberSaveable { mutableStateOf<String?>(null) }
    var showHelp by rememberSaveable { mutableStateOf(false) }
    var query by rememberSaveable { mutableStateOf("") }
    val results by Downloader.results.collectAsState()
    val searching by Downloader.searching.collectAsState()
    val context = androidx.compose.ui.platform.LocalContext.current
    BackHandler(enabled = results != null) { Downloader.results.value = null }

    BackHandler(enabled = tab != 0) { if (openPlaylist != null) openPlaylist = null else tab = 0 }

    Column(Modifier.fillMaxSize()) {
        Row(Modifier.fillMaxWidth().padding(start = 20.dp, end = 8.dp, top = 12.dp, bottom = 12.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text("Bibliothek", fontSize = 32.sp, fontWeight = FontWeight.Bold, color = MaterialTheme.colorScheme.onBackground)
                Text("${library?.songs?.size ?: 0} Songs", fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            IconButton(onClick = { showHelp = true }) { Icon(HmIcons.Download, "Von YouTube laden", tint = MaterialTheme.colorScheme.onSurfaceVariant) }
            IconButton(onClick = { vm.refresh() }) { Icon(HmIcons.Refresh, "Aktualisieren", tint = MaterialTheme.colorScheme.onSurfaceVariant) }
            IconButton(onClick = onSettings) { Icon(HmIcons.Gear, "Einstellungen", tint = MaterialTheme.colorScheme.onSurfaceVariant) }
        }
        // Suche auf YouTube: Treffer antippen lädt den Song
        val focus = androidx.compose.ui.platform.LocalFocusManager.current
        androidx.compose.material3.OutlinedTextField(
            value = query, onValueChange = { query = it }, singleLine = true,
            placeholder = { Text("Song auf YouTube suchen") },
            shape = RoundedCornerShape(26.dp),
            keyboardOptions = androidx.compose.foundation.text.KeyboardOptions(imeAction = androidx.compose.ui.text.input.ImeAction.Search),
            keyboardActions = androidx.compose.foundation.text.KeyboardActions(onSearch = { focus.clearFocus(); Downloader.search(context, query) }),
            trailingIcon = { if (results != null) TextButton(onClick = { Downloader.results.value = null; query = "" }) { Text("✕") } },
            modifier = Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, bottom = 12.dp),
        )
        Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            fun toggle(t: Int) { tab = if (tab == t) 0 else t; openPlaylist = null }
            // blass dahinter: zuletzt hinzugefügter Favorit / neueste Playlist / zuletzt gehört
            val show = vm.look.value.tileCovers
            fun cover(id: String?) = if (show) library?.byId?.get(id)?.coverUri else null
            Tile("Favoriten", HmIcons.Heart, Color(0xFFFF6482), tab == 1, cover(favorites.firstOrNull()), Modifier.weight(1f)) { toggle(1) }
            Tile("Playlists", HmIcons.Note, Color(0xFFB48CFF), tab == 2, cover(library?.playlists?.lastOrNull()?.songIds?.lastOrNull()), Modifier.weight(1f)) { toggle(2) }
            Tile("Zuletzt", HmIcons.Refresh, Color(0xFF5ED6B0), tab == 3, cover(recent.firstOrNull()), Modifier.weight(1f)) { toggle(3) }
        }
        Spacer(Modifier.height(10.dp))
        if (loading) LinearProgressIndicator(Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 8.dp))
        download?.let {
            Text(
                it, fontSize = 13.sp,
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp).clip(RoundedCornerShape(14.dp))
                    .background(MaterialTheme.colorScheme.surfaceVariant).clickable { Downloader.status.value = null }.padding(14.dp),
            )
        }
        error?.let {
            Column(Modifier.fillMaxWidth().padding(16.dp)) {
                Text(it, color = Color(0xFFFF9696))
                Spacer(Modifier.height(8.dp))
                Button(onClick = onPickFolder) { Text("Anderen Ordner wählen") }
            }
        }

        Box(Modifier.weight(1f).fillMaxWidth()) {
            val lib = library
            val hits = results
            if (hits != null) {
                SearchResults(hits, searching) { Downloader.download(context, "https://youtu.be/${it.id}", toast = true) }
            } else if (lib == null) {
                if (!loading && error == null) Text("Keine Bibliothek geladen.", Modifier.padding(20.dp), color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else when {
                tab == 2 && openPlaylist == null -> PlaylistList(vm, lib, onOpen = { openPlaylist = it })
                tab == 2 -> {
                    val pl = lib.playlists.firstOrNull { it.name == openPlaylist }
                    val songs = pl?.let { vm.songs(it.songIds) } ?: emptyList()
                    var adding by remember { mutableStateOf(false) }
                    SongList(vm, pl?.name ?: "", songs, currentId, favorites, emptyText = "Diese Playlist ist leer.", onAdd = { adding = true })
                    if (adding && pl != null) AddSongsDialog(lib.songs.filter { !it.id.startsWith("yt:") && it.id !in pl.songIds }, onClose = { adding = false }) {
                        vm.addToPlaylist(pl.name, it)
                        adding = false
                    }
                }
                else -> {
                    // Reihenfolge nur beim Umschalten oder neuer Bibliothek neu, nicht bei jedem Songwechsel
                    val all = remember(lib, byPlays) {
                        val byTitle = lib.songs.sortedBy { it.title.lowercase() }
                        if (byPlays) byTitle.sortedByDescending { vm.plays(it) } else byTitle
                    }
                    val songs = when (tab) {
                        1 -> vm.songs(favorites)
                        3 -> vm.songs(recent)
                        else -> all
                    }
                    SongList(
                        vm, when (tab) { 1 -> "Favoriten"; 3 -> "Zuletzt gespielt"; else -> "Alle Songs" }, songs, currentId, favorites,
                        emptyText = when (tab) { 1 -> "Noch keine Favoriten – tippe aufs Herz."; 3 -> "Noch nichts gespielt."; else -> "Keine Songs." },
                        sortable = tab == 0, playsTick = playsTick,
                    )
                }
            }
        }
        MiniPlayer(vm, onOpenNow)
    }

    if (showHelp) {
        val tool by Downloader.toolVersion.collectAsState()
        AlertDialog(
            onDismissRequest = { showHelp = false },
            title = { Text("Von YouTube laden") },
            text = { Text("Öffne das Video in YouTube, tippe auf „Teilen“ und wähle HoverMusic. Der Song landet auf diesem Handy.\n\nyt-dlp: ${tool ?: "noch nicht geladen"}") },
            confirmButton = { TextButton(onClick = { showHelp = false }) { Text("OK") } },
        )
    }
}

@Composable
private fun SearchResults(hits: List<Found>, searching: Boolean, onPick: (Found) -> Unit) {
    if (hits.isEmpty()) {
        Text(if (searching) "Suche …" else "Nichts gefunden.", Modifier.padding(20.dp), color = MaterialTheme.colorScheme.onSurfaceVariant)
        return
    }
    val progress by Downloader.progress.collectAsState()
    LazyColumn(Modifier.fillMaxSize()) {
        itemsIndexed(hits) { _, f ->
            val p = progress[f.id]
            Column(Modifier.fillMaxWidth().clickable(enabled = p == null) { onPick(f) }.padding(horizontal = 20.dp, vertical = 10.dp)) {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(f.title, maxLines = 2, overflow = TextOverflow.Ellipsis, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
                    Text("${f.channel} · ${fmtTime(f.seconds * 1000L)}", maxLines = 1, fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                if (p == 1f) Text("✓", fontSize = 20.sp, color = MaterialTheme.colorScheme.primary)
                else Icon(HmIcons.Download, "Laden", tint = if (p == null) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant)
            }
            if (p != null && p < 1f) {  // wartet oder lädt: Balken direkt unter dem Song
                val bar = Modifier.fillMaxWidth().padding(top = 8.dp).height(4.dp).clip(CircleShape)
                if (p <= 0f) LinearProgressIndicator(bar) else LinearProgressIndicator(progress = { p }, modifier = bar)
                Text(if (p < 0f) "Wartet …" else "${(p * 100).toInt()} %", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            }
        }
    }
}

@Composable
private fun AddSongsDialog(candidates: List<Song>, onClose: () -> Unit, onAdd: (List<String>) -> Unit) {
    var chosen by remember { mutableStateOf(setOf<String>()) }
    val sorted = remember(candidates) { candidates.sortedBy { it.title.lowercase() } }
    AlertDialog(
        onDismissRequest = onClose,
        title = { Text("Songs hinzufügen") },
        text = {
            if (sorted.isEmpty()) Text("Alle Songs sind schon in dieser Playlist.")
            else LazyColumn(Modifier.heightIn(max = 420.dp)) {
                itemsIndexed(sorted, key = { _, s -> s.id }) { _, s ->
                    val on = s.id in chosen
                    Row(Modifier.fillMaxWidth().clickable { chosen = if (on) chosen - s.id else chosen + s.id }.padding(vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                        androidx.compose.material3.Checkbox(checked = on, onCheckedChange = null)
                        Spacer(Modifier.width(10.dp))
                        Cover(s, 40.dp, radius = 10)
                        Spacer(Modifier.width(10.dp))
                        Column {
                            Text(s.title, maxLines = 1, overflow = TextOverflow.Ellipsis, fontSize = 15.sp)
                            Text(s.artist, maxLines = 1, overflow = TextOverflow.Ellipsis, fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    }
                }
            }
        },
        confirmButton = { TextButton(onClick = { onAdd(chosen.toList()) }, enabled = chosen.isNotEmpty()) { Text("Hinzufügen (${chosen.size})") } },
        dismissButton = { TextButton(onClick = onClose) { Text("Abbrechen") } },
    )
}

@Composable
private fun Tile(name: String, icon: ImageVector, tint: Color, selected: Boolean, cover: android.net.Uri?, modifier: Modifier, onClick: () -> Unit) {
    Box(
        modifier.clip(RoundedCornerShape(18.dp))
            .background(if (selected) tint.copy(alpha = 0.28f) else MaterialTheme.colorScheme.surface)
            .clickable { onClick() },
    ) {
        if (cover != null) AsyncImage(model = cover, contentDescription = null, contentScale = ContentScale.Crop, alpha = 0.35f, modifier = Modifier.matchParentSize())
        Column(Modifier.fillMaxWidth().padding(14.dp)) {
            Icon(icon, null, tint = tint, modifier = Modifier.size(22.dp))
            Spacer(Modifier.height(10.dp))
            Text(name, fontSize = 14.sp, fontWeight = FontWeight.SemiBold, maxLines = 1)
        }
    }
}

@Composable
private fun PlaylistList(vm: MusicViewModel, lib: Library, onOpen: (String) -> Unit) {
    if (lib.playlists.isEmpty()) {
        Text("Keine Playlists. Lege sie in HoverBar am PC an.", Modifier.padding(20.dp), color = MaterialTheme.colorScheme.onSurfaceVariant)
        return
    }
    LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(top = 12.dp)) {
        itemsIndexed(lib.playlists) { _, p ->
            Row(Modifier.fillMaxWidth().clickable { onOpen(p.name) }.padding(horizontal = 16.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                Cover(vm.songs(p.songIds).firstOrNull(), 72.dp, radius = 16)
                Spacer(Modifier.width(14.dp))
                Column {
                    Text(p.name, fontSize = 18.sp, fontWeight = FontWeight.SemiBold)
                    Text("${p.songIds.size} Songs", color = MaterialTheme.colorScheme.onSurfaceVariant, fontSize = 13.sp)
                }
            }
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun SongList(
    vm: MusicViewModel, title: String, songs: List<Song>, currentId: String?, favorites: List<String>,
    emptyText: String, sortable: Boolean = false, playsTick: Int = 0, onAdd: (() -> Unit)? = null,
) {
    val byPlays by vm.sortByPlays.collectAsState()
    var editing by remember { mutableStateOf<Song?>(null) }
    editing?.let { EditDialog(vm, it) { editing = null } }
    LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = 8.dp)) {
        item {
            Row(Modifier.fillMaxWidth().padding(start = 20.dp, end = 16.dp, top = 18.dp, bottom = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(title, fontSize = 18.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis)
                if (sortable) {
                    Text(
                        if (byPlays) "Meistgespielt" else "Nach Titel", fontSize = 13.sp,
                        modifier = Modifier.clip(CircleShape).background(MaterialTheme.colorScheme.surfaceVariant)
                            .clickable { vm.toggleSort() }.heightIn(min = 40.dp).padding(horizontal = 16.dp, vertical = 11.dp),
                    )
                    Spacer(Modifier.width(10.dp))
                }
                if (onAdd != null) {  // runder Plus-Knopf: Songs in die Playlist holen
                    IconButton(onClick = onAdd, modifier = Modifier.size(48.dp).clip(CircleShape).background(MaterialTheme.colorScheme.surfaceVariant)) {
                        Text("+", fontSize = 26.sp)
                    }
                    Spacer(Modifier.width(10.dp))
                }
                if (songs.isNotEmpty()) {
                    IconButton(onClick = { vm.play(songs, 0) }, modifier = Modifier.size(48.dp).clip(CircleShape).background(MaterialTheme.colorScheme.primary)) {
                        Icon(HmIcons.Play, "Alle abspielen", tint = Color.White)
                    }
                }
            }
        }
        if (songs.isEmpty()) item { Text(emptyText, Modifier.padding(20.dp), color = MaterialTheme.colorScheme.onSurfaceVariant) }
        itemsIndexed(songs, key = { _, s -> s.id }) { i, s ->
            val playing = s.id == currentId
            val count = if (byPlays && sortable) remember(s.id, playsTick) { vm.plays(s) } else null
            Row(
                Modifier.fillMaxWidth().padding(horizontal = 8.dp).clip(RoundedCornerShape(16.dp))
                    .background(if (playing) MaterialTheme.colorScheme.primary.copy(alpha = 0.16f) else Color.Transparent)
                    .combinedClickable(onLongClick = { editing = s }) { vm.play(songs, i) }  // lange drücken: bearbeiten
                    .padding(start = 8.dp, top = 8.dp, bottom = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Cover(s, 56.dp, radius = 14)
                Spacer(Modifier.width(14.dp))
                Column(Modifier.weight(1f)) {
                    Text(
                        s.title, maxLines = 1, overflow = TextOverflow.Ellipsis, fontSize = 16.sp, fontWeight = FontWeight.SemiBold,
                        color = if (playing) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurface,
                    )
                    Text(
                        listOfNotNull(s.artist.ifEmpty { null }, count?.let { "$it× gespielt" }, if (s.uri == null) "Datei fehlt" else null).joinToString(" · "),
                        maxLines = 1, overflow = TextOverflow.Ellipsis, fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                IconButton(onClick = { vm.toggleFavorite(s.id) }) {
                    val fav = s.id in favorites
                    Icon(
                        if (fav) HmIcons.Heart else HmIcons.HeartOutline, "Favorit", Modifier.size(20.dp),
                        tint = if (fav) Color(0xFFFF6482) else MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
            if (vm.look.value.lines && i < songs.lastIndex) HorizontalDivider(Modifier.padding(horizontal = 20.dp), color = Color.White.copy(alpha = 0.07f))
        }
    }
}

@Composable
private fun MiniPlayer(vm: MusicViewModel, onOpen: () -> Unit) {
    val library by vm.library.collectAsState()
    val id by vm.currentId.collectAsState()
    val playing by vm.isPlaying.collectAsState()
    val pos by vm.positionMs.collectAsState()
    val dur by vm.durationMs.collectAsState()
    val song = id?.let { library?.byId?.get(it) } ?: return
    Column(Modifier.fillMaxWidth().padding(10.dp).clip(RoundedCornerShape(22.dp)).background(MaterialTheme.colorScheme.surfaceVariant).clickable { onOpen() }) {
        Row(Modifier.padding(horizontal = 12.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            Cover(song, 48.dp, radius = 14)
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f)) {
                Text(song.title, maxLines = 1, overflow = TextOverflow.Ellipsis, fontWeight = FontWeight.Medium)
                Text(song.artist, maxLines = 1, overflow = TextOverflow.Ellipsis, fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            IconButton(onClick = { vm.previous() }) { Icon(HmIcons.Previous, "Zurück") }
            IconButton(onClick = { vm.togglePlay() }) { Icon(if (playing) HmIcons.Pause else HmIcons.Play, if (playing) "Pause" else "Play") }
            IconButton(onClick = { vm.next() }) { Icon(HmIcons.Next, "Weiter") }
        }
        LinearProgressIndicator(progress = { if (dur > 0) (pos.toFloat() / dur).coerceIn(0f, 1f) else 0f }, modifier = Modifier.fillMaxWidth().padding(start = 18.dp, end = 18.dp, bottom = 6.dp).height(2.dp))
    }
}
