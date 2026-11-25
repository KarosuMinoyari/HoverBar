package app.hovermusic

import android.app.Application
import android.content.ComponentName
import android.content.Intent
import android.net.Uri
import androidx.core.content.ContextCompat
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import androidx.media3.common.MediaItem
import androidx.media3.common.MediaMetadata
import androidx.media3.common.Player
import androidx.media3.session.MediaController
import androidx.media3.session.SessionToken
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** Wie im HoverBar-Player: jeder Klick schaltet weiter. */
enum class PlayMode { Off, Shuffle, RepeatAll, RepeatOne }

class MusicViewModel(app: Application) : AndroidViewModel(app) {
    private val prefs = Prefs(app)

    val library = MutableStateFlow<Library?>(null)
    val loading = MutableStateFlow(false)
    val error = MutableStateFlow<String?>(null)
    val hasFolder = MutableStateFlow(prefs.treeUri != null)
    val favorites = MutableStateFlow(prefs.favorites())
    val recent = MutableStateFlow(prefs.recent())

    val currentId = MutableStateFlow<String?>(null)
    val isPlaying = MutableStateFlow(false)
    val positionMs = MutableStateFlow(0L)
    val durationMs = MutableStateFlow(0L)
    val mode = MutableStateFlow(PlayMode.Off)
    val lyrics = MutableStateFlow(Lyrics.EMPTY)

    /** Einrichtung aus dem QR-Code von HoverBar (Cloud-Dienst, Ordner); bleibt gespeichert, bis der Ordner gewählt ist. */
    val setup = MutableStateFlow(prefs.setupJson?.let { SetupInfo.fromJson(it) })

    private var controller: MediaController? = null

    init {
        prefs.jsonCache?.let { runCatching { library.value = LibraryLoader.parseCached(it) } }
        connect()
        if (prefs.treeUri != null) refresh()
        viewModelScope.launch {
            while (true) {
                delay(300)
                controller?.let {
                    positionMs.value = it.currentPosition.coerceAtLeast(0)
                    val d = it.duration
                    if (d > 0) durationMs.value = d
                }
            }
        }
    }

    // ------------------------------------------------------------ Ordner und Bibliothek

    /** Text aus dem QR-Code oder einem eingefügten Link übernehmen. False, wenn es kein HoverBar-Code ist. */
    fun applySetup(text: String): Boolean {
        val s = SetupInfo.parse(text.trim()) ?: return false
        prefs.setupJson = s.toJson()
        setup.value = s
        return true
    }

    fun clearSetup() {
        prefs.setupJson = null
        setup.value = null
    }

    fun setFolder(uri: Uri) {
        runCatching {
            getApplication<Application>().contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        prefs.treeUri = uri.toString()
        hasFolder.value = true
        refresh()
    }

    fun refresh() {
        val tree = prefs.treeUri ?: return
        viewModelScope.launch {
            loading.value = true
            error.value = null
            try {
                val loaded = withContext(Dispatchers.IO) { LibraryLoader.load(getApplication(), Uri.parse(tree)) }
                prefs.jsonCache = loaded.rawJson
                library.value = loaded.library
                if (!prefs.favoritesSeeded) {  // einmalig: Favoriten vom PC übernehmen
                    prefs.setFavorites((prefs.favorites() + loaded.library.pcFavorites).distinct())
                    favorites.value = prefs.favorites()
                    prefs.favoritesSeeded = true
                }
                currentId.value?.let { loadLyrics(it) }
            } catch (e: Exception) {
                error.value = e.message ?: "Die Bibliothek konnte nicht geladen werden."
            }
            loading.value = false
        }
    }

    fun songs(ids: List<String>): List<Song> {
        val by = library.value?.byId ?: return emptyList()
        return ids.mapNotNull { by[it] }
    }

    // ------------------------------------------------------------ Favoriten / zuletzt gespielt

    fun toggleFavorite(id: String) {
        val l = favorites.value
        val n = if (id in l) l - id else listOf(id) + l
        favorites.value = n
        prefs.setFavorites(n)
    }

    private fun addRecent(id: String) {
        val n = (listOf(id) + recent.value.filter { it != id }).take(40)
        recent.value = n
        prefs.setRecent(n)
    }

    // ------------------------------------------------------------ Wiedergabe

    private fun connect() {
        val ctx = getApplication<Application>()
        val token = SessionToken(ctx, ComponentName(ctx, PlaybackService::class.java))
        val future = MediaController.Builder(ctx, token).buildAsync()
        future.addListener({
            val c = runCatching { future.get() }.getOrNull() ?: return@addListener
            controller = c
            c.addListener(object : Player.Listener {
                override fun onIsPlayingChanged(isPlaying: Boolean) { this@MusicViewModel.isPlaying.value = isPlaying }
                override fun onMediaItemTransition(mediaItem: MediaItem?, reason: Int) { onItem(mediaItem?.mediaId) }
                override fun onShuffleModeEnabledChanged(shuffleModeEnabled: Boolean) { updateMode() }
                override fun onRepeatModeChanged(repeatMode: Int) { updateMode() }
            })
            isPlaying.value = c.isPlaying
            onItem(c.currentMediaItem?.mediaId)
            updateMode()
        }, ContextCompat.getMainExecutor(ctx))
    }

    private fun onItem(id: String?) {
        currentId.value = id
        positionMs.value = 0
        durationMs.value = (id?.let { library.value?.byId?.get(it)?.durationSec } ?: 0) * 1000L
        lyrics.value = Lyrics.EMPTY
        if (id != null) {
            addRecent(id)
            loadLyrics(id)
        }
    }

    private fun loadLyrics(id: String) {
        val uri = library.value?.byId?.get(id)?.lyricsUri ?: return
        viewModelScope.launch {
            val l = withContext(Dispatchers.IO) {
                runCatching {
                    getApplication<Application>().contentResolver.openInputStream(uri)?.use { Lyrics.parse(it.readBytes().toString(Charsets.UTF_8)) }
                }.getOrNull()
            }
            if (currentId.value == id && l != null) lyrics.value = l
        }
    }

    /** Liste ab Position [index] abspielen (nur Songs, deren Datei im Ordner gefunden wurde). */
    fun play(list: List<Song>, index: Int) {
        val c = controller ?: return
        val playable = list.filter { it.uri != null }
        if (playable.isEmpty()) return
        val start = playable.indexOfFirst { it.id == list.getOrNull(index)?.id }.coerceAtLeast(0)
        val items = playable.map { s ->
            MediaItem.Builder()
                .setMediaId(s.id)
                .setUri(s.uri)
                .setMediaMetadata(
                    MediaMetadata.Builder().setTitle(s.title).setArtist(s.artist).setArtworkUri(s.coverUri).build(),
                )
                .build()
        }
        c.setMediaItems(items, start, 0L)
        c.prepare()
        c.play()
    }

    fun togglePlay() {
        val c = controller ?: return
        if (c.isPlaying) c.pause() else c.play()
    }

    fun next() { controller?.seekToNext() }
    fun previous() { controller?.seekToPrevious() }
    fun seekTo(ms: Long) {
        controller?.seekTo(ms.coerceAtLeast(0))
        positionMs.value = ms
    }

    /** Aus → Zufall → Playlist wiederholen → Song wiederholen → Aus */
    fun cycleMode() {
        val c = controller ?: return
        val next = when (mode.value) {
            PlayMode.Off -> PlayMode.Shuffle
            PlayMode.Shuffle -> PlayMode.RepeatAll
            PlayMode.RepeatAll -> PlayMode.RepeatOne
            PlayMode.RepeatOne -> PlayMode.Off
        }
        c.shuffleModeEnabled = next == PlayMode.Shuffle
        c.repeatMode = when (next) {
            PlayMode.RepeatAll -> Player.REPEAT_MODE_ALL
            PlayMode.RepeatOne -> Player.REPEAT_MODE_ONE
            else -> Player.REPEAT_MODE_OFF
        }
        mode.value = next
    }

    private fun updateMode() {
        val c = controller ?: return
        mode.value = when {
            c.repeatMode == Player.REPEAT_MODE_ONE -> PlayMode.RepeatOne
            c.repeatMode == Player.REPEAT_MODE_ALL -> PlayMode.RepeatAll
            c.shuffleModeEnabled -> PlayMode.Shuffle
            else -> PlayMode.Off
        }
    }

    override fun onCleared() {
        controller?.release()
        controller = null
    }
}
