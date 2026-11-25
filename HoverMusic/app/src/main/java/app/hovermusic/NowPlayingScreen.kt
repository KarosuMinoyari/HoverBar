package app.hovermusic

import android.os.Build
import androidx.compose.ui.draw.blur
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.layout.ContentScale
import coil.compose.AsyncImage
import androidx.compose.foundation.background
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.interaction.collectIsDraggedAsState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.delay

/** "Läuft gerade": oben Cover und Songtext zum Wischen, unten Titel, Fortschritt und Steuerung. */
@Composable
fun NowPlayingScreen(vm: MusicViewModel, onClose: () -> Unit) {
    val library by vm.library.collectAsState()
    val id by vm.currentId.collectAsState()
    val playing by vm.isPlaying.collectAsState()
    val pos by vm.positionMs.collectAsState()
    val dur by vm.durationMs.collectAsState()
    val mode by vm.mode.collectAsState()
    val lyrics by vm.lyrics.collectAsState()
    val favorites by vm.favorites.collectAsState()
    val song = id?.let { library?.byId?.get(it) }

    Box(Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background)) {
    // weichgezeichnetes Cover als Hintergrund (Weichzeichner erst ab Android 12, davor nur sehr blass)
    song?.coverUri?.takeIf { vm.look.value.coverBackdrop }?.let {
        AsyncImage(
            model = it, contentDescription = null, contentScale = ContentScale.Crop,
            modifier = Modifier.fillMaxSize().blur(70.dp), alpha = if (Build.VERSION.SDK_INT >= 31) 0.55f else 0.12f,
        )
    }
    Box(Modifier.fillMaxSize().background(Brush.verticalGradient(listOf(Color.Transparent, MaterialTheme.colorScheme.background))))
    Column(
        Modifier.fillMaxSize().padding(horizontal = 24.dp)
            .pointerInput(Unit) { detectTapGestures { } },  // Berührungen nicht an die Bibliothek darunter durchlassen
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        var editing by remember { mutableStateOf<Song?>(null) }
        editing?.let { EditDialog(vm, it) { editing = null } }
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            IconButton(onClick = onClose) { Icon(HmIcons.Down, "Schließen", tint = MaterialTheme.colorScheme.onSurfaceVariant) }
            if (song != null) IconButton(onClick = { editing = song }) { Icon(HmIcons.Pencil, "Song bearbeiten", tint = MaterialTheme.colorScheme.onSurfaceVariant) }
        }
        // oben: wischen zwischen Cover (Seite 1) und Songtext (Seite 2)
        val pager = rememberPagerState { 2 }
        // die Datei ist ungeschnitten: Zeitstempel im Songtext um den Schnitt verschieben
        val cut = song?.trimFromMs ?: 0
        HorizontalPager(pager, Modifier.weight(1f).fillMaxWidth()) { page ->
            if (page == 0) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { Cover(song, null, Modifier.aspectRatio(1f), radius = 22) }
            } else {
                LyricsView(lyrics, pos + cut, hasFile = song?.lyricsUri != null, onSeek = { vm.seekTo((it - cut).coerceAtLeast(0)) }, modifier = Modifier.fillMaxSize())
            }
        }
        Row(Modifier.padding(vertical = 10.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            repeat(2) { i ->
                Box(Modifier.size(7.dp).clip(CircleShape).background(MaterialTheme.colorScheme.onBackground.copy(alpha = if (pager.currentPage == i) 0.9f else 0.3f)))
            }
        }

        // unten: Titel und Bedienung
        Text(song?.title ?: "Nichts läuft", fontSize = 22.sp, fontWeight = FontWeight.Bold, maxLines = 1, overflow = TextOverflow.Ellipsis, textAlign = TextAlign.Center)
        Text(song?.artist ?: "", fontSize = 15.sp, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1, overflow = TextOverflow.Ellipsis)
        Spacer(Modifier.height(8.dp))
        // Fortschritt
        var drag by remember { mutableStateOf<Float?>(null) }
        val frac = drag ?: if (dur > 0) (pos.toFloat() / dur).coerceIn(0f, 1f) else 0f
        Slider(
            value = frac,
            onValueChange = { drag = it },
            onValueChangeFinished = {
                drag?.let { vm.seekTo((it * dur).toLong()) }
                drag = null
            },
            modifier = Modifier.fillMaxWidth(),
        )
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Text(fmtTime((frac * dur).toLong()), fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Text(if (dur > 0) fmtTime(dur) else "–", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }

        // Steuerung: links Zufall/Wiederholen, mittig Zurück/Play/Weiter, rechts Herz
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.SpaceBetween) {
            IconButton(onClick = { vm.cycleMode() }) {
                val active = mode != PlayMode.Off
                Icon(
                    when (mode) { PlayMode.RepeatAll -> HmIcons.Repeat; PlayMode.RepeatOne -> HmIcons.RepeatOne; else -> HmIcons.Shuffle },
                    "Zufall / Wiederholen",
                    tint = if (active) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Row(verticalAlignment = Alignment.CenterVertically) {
                IconButton(onClick = { vm.previous() }, modifier = Modifier.size(52.dp)) { Icon(HmIcons.Previous, "Zurück", Modifier.size(32.dp)) }
                IconButton(
                    onClick = { vm.togglePlay() },
                    modifier = Modifier.size(68.dp).clip(CircleShape).background(Color(0xFFEDEDF2)),
                ) { Icon(if (playing) HmIcons.Pause else HmIcons.Play, if (playing) "Pause" else "Play", Modifier.size(36.dp), tint = Color(0xFF14141A)) }
                IconButton(onClick = { vm.next() }, modifier = Modifier.size(52.dp)) { Icon(HmIcons.Next, "Weiter", Modifier.size(32.dp)) }
            }
            IconButton(onClick = { id?.let { vm.toggleFavorite(it) } }) {
                val fav = id != null && id in favorites
                Icon(
                    if (fav) HmIcons.Heart else HmIcons.HeartOutline, "Favorit",
                    tint = if (fav) Color(0xFFFF5A78) else MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        Spacer(Modifier.height(16.dp))
    }
    }
}

/**
 * Songtext: die aktuelle Zeile bleibt oben im Blick. Wer durchscrollt, kann eine Zeile antippen, um dorthin zu springen;
 * nach 2 s ohne Berührung springt der Text zurück zur aktuellen Zeile.
 */
@Composable
private fun LyricsView(lyrics: Lyrics, posMs: Long, hasFile: Boolean, onSeek: (Long) -> Unit, modifier: Modifier) {
    if (lyrics.lines.isEmpty()) {
        Box(modifier, contentAlignment = Alignment.TopCenter) {
            Text(
                if (hasFile) "Songtext wird geladen …" else "Kein Songtext (am PC lädt HoverBar ihn beim Abspielen).",
                color = MaterialTheme.colorScheme.onSurfaceVariant, fontSize = 13.sp, textAlign = TextAlign.Center, modifier = Modifier.padding(top = 24.dp),
            )
        }
        return
    }
    val state = rememberLazyListState()
    val current = lyrics.indexAt(posMs)
    var browsing by remember { mutableStateOf(false) }
    var autoScrolling by remember { mutableStateOf(false) }
    val scrolling = state.isScrollInProgress
    val dragged by state.interactionSource.collectIsDraggedAsState()

    LaunchedEffect(scrolling, dragged) {
        if ((scrolling || dragged) && !autoScrolling) browsing = true
        if (!scrolling && !dragged && browsing) {
            delay(2000)  // 2 s keine Berührung: zurück zur aktuellen Zeile
            browsing = false
        }
    }
    LaunchedEffect(current, browsing) {
        if (!browsing) {
            autoScrolling = true
            if (current >= 0) state.animateScrollToItem((current - 1).coerceAtLeast(0)) else state.animateScrollToItem(0)
            autoScrolling = false
        }
    }
    LazyColumn(modifier, state = state, horizontalAlignment = Alignment.CenterHorizontally) {
        itemsIndexed(lyrics.lines) { i, l ->
            val now = lyrics.synced && i == current
            Text(
                l.text.ifEmpty { "♪" },
                fontSize = 20.sp,
                fontWeight = FontWeight.Bold,
                textAlign = TextAlign.Center,
                color = if (!lyrics.synced) MaterialTheme.colorScheme.onSurface else if (now) Color.White else Color(0xFF8E8EA0),
                modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp).let {
                    if (lyrics.synced) it.clickable { browsing = false; onSeek(l.timeMs) } else it
                },
            )
        }
        item { Spacer(Modifier.height(160.dp)) }
    }
}
