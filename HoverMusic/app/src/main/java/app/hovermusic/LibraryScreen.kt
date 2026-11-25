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
import androidx.compose.material3.Tab
import androidx.compose.material3.TabRow
import androidx.compose.material3.Text
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

private val tabNames = listOf("Alle Songs", "Favoriten", "Playlists", "Zuletzt")

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
fun LibraryScreen(vm: MusicViewModel, onOpenNow: () -> Unit, onPickFolder: () -> Unit) {
    val library by vm.library.collectAsState()
    val loading by vm.loading.collectAsState()
    val error by vm.error.collectAsState()
    val favorites by vm.favorites.collectAsState()
    val recent by vm.recent.collectAsState()
    val currentId by vm.currentId.collectAsState()
    var tab by rememberSaveable { mutableIntStateOf(0) }
    var openPlaylist by rememberSaveable { mutableStateOf<String?>(null) }

    Column(Modifier.fillMaxSize()) {
        // Kopfzeile
        Row(Modifier.fillMaxWidth().padding(start = 20.dp, end = 8.dp, top = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            Text("HoverMusic", fontSize = 24.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f), color = MaterialTheme.colorScheme.onBackground)
            IconButton(onClick = { vm.refresh() }) { Icon(HmIcons.Refresh, "Aktualisieren", tint = MaterialTheme.colorScheme.onSurfaceVariant) }
            IconButton(onClick = onPickFolder) { Icon(HmIcons.Folder, "Ordner wählen", tint = MaterialTheme.colorScheme.onSurfaceVariant) }
        }
        TabRow(selectedTabIndex = tab, containerColor = MaterialTheme.colorScheme.background, contentColor = MaterialTheme.colorScheme.primary) {
            tabNames.forEachIndexed { i, n ->
                Tab(selected = tab == i, onClick = { tab = i; openPlaylist = null }, text = { Text(n, maxLines = 1, fontSize = 13.sp) })
            }
        }
        if (loading) LinearProgressIndicator(Modifier.fillMaxWidth())
        error?.let {
            Column(Modifier.fillMaxWidth().padding(16.dp)) {
                Text(it, color = androidx.compose.ui.graphics.Color(0xFFFF9696))
                Spacer(Modifier.height(8.dp))
                Button(onClick = onPickFolder) { Text("Anderen Ordner wählen") }
            }
        }

        Box(Modifier.weight(1f).fillMaxWidth()) {
            val lib = library
            if (lib == null) {
                if (!loading && error == null) Text("Keine Bibliothek geladen.", Modifier.padding(20.dp), color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else when {
                tab == 2 && openPlaylist == null -> PlaylistList(lib, onOpen = { openPlaylist = it })
                tab == 2 -> {
                    val pl = lib.playlists.firstOrNull { it.name == openPlaylist }
                    val songs = pl?.let { vm.songs(it.songIds) } ?: emptyList()
                    Column(Modifier.fillMaxSize()) {
                        Row(Modifier.fillMaxWidth().clickable { openPlaylist = null }.padding(horizontal = 12.dp, vertical = 10.dp), verticalAlignment = Alignment.CenterVertically) {
                            Icon(HmIcons.Back, null, tint = MaterialTheme.colorScheme.onSurfaceVariant)
                            Spacer(Modifier.width(10.dp))
                            Text(pl?.name ?: "", fontSize = 18.sp, fontWeight = FontWeight.SemiBold)
                        }
                        SongList(vm, songs, currentId, favorites, emptyText = "Diese Playlist ist leer.")
                    }
                }
                else -> {
                    val songs = when (tab) {
                        1 -> vm.songs(favorites)
                        3 -> vm.songs(recent)
                        else -> lib.songs.sortedBy { it.title.lowercase() }
                    }
                    SongList(
                        vm, songs, currentId, favorites,
                        emptyText = when (tab) { 1 -> "Noch keine Favoriten – tippe aufs Herz."; 3 -> "Noch nichts gespielt."; else -> "Keine Songs." },
                    )
                }
            }
        }
        MiniPlayer(vm, onOpenNow)
    }
}

@Composable
private fun PlaylistList(lib: Library, onOpen: (String) -> Unit) {
    if (lib.playlists.isEmpty()) {
        Text("Keine Playlists. Lege sie in HoverBar am PC an.", Modifier.padding(20.dp), color = MaterialTheme.colorScheme.onSurfaceVariant)
        return
    }
    LazyColumn(Modifier.fillMaxSize()) {
        itemsIndexed(lib.playlists) { _, p ->
            Row(Modifier.fillMaxWidth().clickable { onOpen(p.name) }.padding(horizontal = 20.dp, vertical = 16.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(p.name, fontSize = 17.sp, fontWeight = FontWeight.Medium, modifier = Modifier.weight(1f))
                Text("${p.songIds.size} Songs", color = MaterialTheme.colorScheme.onSurfaceVariant, fontSize = 13.sp)
            }
        }
    }
}

@Composable
private fun SongList(vm: MusicViewModel, songs: List<Song>, currentId: String?, favorites: List<String>, emptyText: String) {
    if (songs.isEmpty()) {
        Text(emptyText, Modifier.padding(20.dp), color = MaterialTheme.colorScheme.onSurfaceVariant)
        return
    }
    LazyColumn(Modifier.fillMaxSize()) {
        item {
            Button(onClick = { vm.play(songs, 0) }, modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp)) {
                Icon(HmIcons.Play, null, Modifier.size(20.dp))
                Spacer(Modifier.width(8.dp))
                Text("Alle abspielen")
            }
        }
        itemsIndexed(songs, key = { _, s -> s.id }) { i, s ->
            val playing = s.id == currentId
            Row(
                Modifier.fillMaxWidth().clickable { vm.play(songs, i) }.padding(horizontal = 16.dp, vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Cover(s, 52.dp)
                Spacer(Modifier.width(12.dp))
                Column(Modifier.weight(1f)) {
                    Text(
                        s.title, maxLines = 1, overflow = TextOverflow.Ellipsis, fontSize = 16.sp, fontWeight = FontWeight.Medium,
                        color = if (playing) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurface,
                    )
                    Text(
                        listOf(s.artist, if (s.uri == null) "Datei fehlt" else "").filter { it.isNotEmpty() }.joinToString(" · "),
                        maxLines = 1, overflow = TextOverflow.Ellipsis, fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                IconButton(onClick = { vm.toggleFavorite(s.id) }) {
                    val fav = s.id in favorites
                    Icon(
                        if (fav) HmIcons.Heart else HmIcons.HeartOutline, "Favorit",
                        tint = if (fav) androidx.compose.ui.graphics.Color(0xFFFF5A78) else MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
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
    Column(Modifier.fillMaxWidth().background(MaterialTheme.colorScheme.surface).clickable { onOpen() }) {
        LinearProgressIndicator(progress = { if (dur > 0) (pos.toFloat() / dur).coerceIn(0f, 1f) else 0f }, modifier = Modifier.fillMaxWidth().height(2.dp))
        Row(Modifier.padding(horizontal = 12.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            Cover(song, 46.dp)
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f)) {
                Text(song.title, maxLines = 1, overflow = TextOverflow.Ellipsis, fontWeight = FontWeight.Medium)
                Text(song.artist, maxLines = 1, overflow = TextOverflow.Ellipsis, fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            IconButton(onClick = { vm.previous() }) { Icon(HmIcons.Previous, "Zurück") }
            IconButton(onClick = { vm.togglePlay() }) { Icon(if (playing) HmIcons.Pause else HmIcons.Play, if (playing) "Pause" else "Play") }
            IconButton(onClick = { vm.next() }) { Icon(HmIcons.Next, "Weiter") }
        }
    }
}
