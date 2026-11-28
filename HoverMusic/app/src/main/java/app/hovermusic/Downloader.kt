package app.hovermusic

import android.content.Context
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.widget.Toast
import com.yausername.youtubedl_android.YoutubeDL
import com.yausername.youtubedl_android.YoutubeDLRequest
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import org.json.JSONObject
import java.io.File

data class Found(val id: String, val title: String, val channel: String, val seconds: Int)

data class SongInfo(val id: String, val title: String, val artist: String, val durationSec: Int, val cuts: String = "")

/**
 * yt-dlp auf dem Handy: holt die Tonspur (m4a, ohne Umwandeln) in den App-Ordner "music".
 * shortcut: läuft ohne Vordergrunddienst; falls Android lange Downloads abbricht, WorkManager nehmen.
 * Die App legt zusätzlich eine Kopie in den Cloud-Ordner (siehe Upload).
 */
object Downloader {
    /** Suchtreffer; null = keine Suche offen. */
    val results = MutableStateFlow<List<Found>?>(null)
    val searching = MutableStateFlow(false)
    /** Fortschritt je Video-ID: -1 wartet, 0..1 lädt (1 = gespeichert). */
    val progress = MutableStateFlow<Map<String, Float>>(emptyMap())

    /** Zeilen "<Video-ID>|<Sekunden>|<Kanal>|<Titel>"; andere (Warnungen) zählen nicht. */
    fun parseSearch(out: String): List<Found> = out.lineSequence().mapNotNull {
        val p = it.trimEnd('\r').split('|', limit = 4)
        if (p.size == 4 && p[0].length == 11) Found(p[0], p[3], p[2], p[1].toDoubleOrNull()?.toInt() ?: 0) else null
    }.toList()

    /** Vorschlag -> gefundene Video-ID ("" = wird noch gesucht). */
    val matched = MutableStateFlow<Map<String, String>>(emptyMap())

    /** Wie am PC: nur ein Video, dessen Länge höchstens 5 s abweicht. */
    fun pickByDuration(hits: List<Found>, seconds: Int): Found? =
        if (seconds <= 0) hits.firstOrNull() else hits.firstOrNull { it.seconds > 0 && kotlin.math.abs(it.seconds - seconds) <= 5 }

    fun downloadMatch(ctx: Context, idea: Idea) {
        val app = ctx.applicationContext
        if (idea.preview in matched.value) return
        matched.value = matched.value + (idea.preview to "")
        scope.launch {
            var found: Found? = null
            mutex.withLock {
                try {
                    init(app, false)
                    val request = YoutubeDLRequest("ytsearch10:${idea.artist} ${idea.title}")
                    request.addOption("--flat-playlist")
                    request.addOption("--print", "%(id)s|%(duration)s|%(channel)s|%(title)s")
                    found = pickByDuration(parseSearch(YoutubeDL.getInstance().execute(request, null, null).out), idea.seconds)
                    if (found == null) status.value = "Kein Video mit passender Länge (±5 s) für „${idea.title}“."
                } catch (e: Throwable) {
                    status.value = "Suche fehlgeschlagen (Internet?)."
                }
            }
            val f = found
            if (f == null) matched.value = matched.value - idea.preview
            else {
                matched.value = matched.value + (idea.preview to f.id)
                download(app, "https://youtu.be/${f.id}", toast = true)
            }
        }
    }

    fun search(ctx: Context, query: String) {
        val app = ctx.applicationContext
        if (query.isBlank() || searching.value) return
        searching.value = true
        results.value = emptyList()
        scope.launch {
            mutex.withLock {
                try {
                    init(app, false)
                    val request = YoutubeDLRequest("ytsearch12:${query.trim()}")
                    request.addOption("--flat-playlist")
                    request.addOption("--print", "%(id)s|%(duration)s|%(channel)s|%(title)s")
                    val found = parseSearch(YoutubeDL.getInstance().execute(request, null, null).out)
                    if (searching.value) results.value = found
                } catch (e: Throwable) {
                    status.value = "Suche fehlgeschlagen (Internet?)."
                    results.value = null
                }
                searching.value = false
            }
        }
    }

    /** Adresse der Tonspur zum Probehören, oder null. Ohne die Warteschlange, damit ein laufender Download nicht aufhält. */
    suspend fun streamUrl(ctx: Context, id: String): String? = withContext(Dispatchers.IO) {
        runCatching {
            init(ctx.applicationContext, false)
            val request = YoutubeDLRequest("https://youtu.be/$id")
            request.addOption("--no-playlist")
            request.addOption("-f", "bestaudio[ext=m4a]/bestaudio")
            request.addOption("-g")
            YoutubeDL.getInstance().execute(request, null, null).out.lineSequence().lastOrNull { it.startsWith("https://") }?.trim()
        }.getOrNull()
    }

    /** Meldung für die Anzeige, null = nichts zu melden. */
    val status = MutableStateFlow<String?>(null)
    val toolVersion = MutableStateFlow<String?>(null)

    /** Zählt hoch, wenn ein neuer Song da ist. */
    val finished = MutableStateFlow(0)

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val mutex = Mutex()
    private var ready = false

    fun dir(ctx: Context) = File(ctx.getExternalFilesDir(null) ?: ctx.filesDir, "music").apply { mkdirs() }

    /** Nur YouTube-Links annehmen (der Text kommt von anderen Apps). */
    fun youtubeUrl(text: String): String? =
        Regex("""https://(www\.|m\.|music\.)?(youtube\.com/|youtu\.be/)\S+""").find(text)?.value

    fun parseInfo(json: String): SongInfo {
        val o = JSONObject(json)
        val artist = o.optString("artist").ifEmpty { o.optString("uploader") }.removeSuffix(" - Topic")
        return SongInfo(o.getString("id"), o.optString("track").ifEmpty { o.optString("title") }, artist, o.optInt("duration"), o.optString("hm_cuts"))
    }

    /** Von yt-dlp geladene Songs (eine .info.json je Song). */
    fun localSongs(ctx: Context): List<Song> {
        val files = dir(ctx).listFiles() ?: return emptyList()
        return files.filter { it.name.endsWith(".info.json") }.mapNotNull { f ->
            runCatching {
                val i = parseInfo(f.readText())
                val audio = File(f.parentFile, "${i.id}.m4a")
                if (!audio.exists()) return@mapNotNull null
                val cover = listOf("jpg", "webp", "png").map { File(f.parentFile, "${i.id}.$it") }.firstOrNull { it.exists() }
                Song("yt:${i.id}", audio.name, i.title, i.artist, i.durationSec, Uri.fromFile(audio), cover?.let { Uri.fromFile(it) }, null, cuts = i.cuts)
            }.getOrNull()
        }
    }

    private fun init(ctx: Context, update: Boolean) {
        if (!ready) {
            YoutubeDL.getInstance().init(ctx)
            ready = true
        }
        val prefs = Prefs(ctx)
        if (update || !prefs.ytdlpUpdated) {
            status.value = "yt-dlp wird auf die neueste Version gebracht …"
            YoutubeDL.getInstance().updateYoutubeDL(ctx, YoutubeDL.UpdateChannel.STABLE)
            prefs.ytdlpUpdated = true
        }
        toolVersion.value = YoutubeDL.getInstance().versionName(ctx) ?: YoutubeDL.getInstance().version(ctx)
    }

    /** Beim Einrichten (und auf Wunsch): yt-dlp holen bzw. aktualisieren. */
    fun prepare(ctx: Context, update: Boolean = false) {
        val app = ctx.applicationContext
        scope.launch {
            mutex.withLock {
                try {
                    init(app, update)
                    status.value = if (update) "yt-dlp ist aktuell." else null
                } catch (e: Throwable) {
                    status.value = "yt-dlp konnte nicht geladen werden (Internet?)."
                }
            }
        }
    }

    private fun say(ctx: Context, text: String, toast: Boolean) {
        status.value = text
        if (toast) Handler(Looper.getMainLooper()).post { Toast.makeText(ctx, text, Toast.LENGTH_SHORT).show() }
    }

    fun download(ctx: Context, sharedText: String, toast: Boolean = false) {
        val app = ctx.applicationContext
        val url = youtubeUrl(sharedText)
        if (url == null) {
            say(app, "Das ist kein YouTube-Link.", toast)
            return
        }
        if (toast) say(app, "Song wird geladen …", true)
        val video = Regex("""(?:youtu\.be/|[?&]v=)([\w-]{11})""").find(url)?.groupValues?.get(1)
        fun mark(p: Float?) {
            if (video == null) return
            progress.value = if (p == null) progress.value - video else progress.value + (video to p)
        }
        mark(-1f)
        scope.launch {
            mutex.withLock {
                try {
                    init(app, false)
                    status.value = "Song wird geladen …"
                    val request = YoutubeDLRequest(url)
                    request.addOption("--no-playlist")
                    request.addOption("-f", "bestaudio[ext=m4a]/bestaudio[acodec^=mp4a]")
                    request.addOption("--write-info-json")
                    request.addOption("--write-thumbnail")
                    request.addOption("-o", File(dir(app), "%(id)s.%(ext)s").absolutePath)
                    mark(0f)
                    YoutubeDL.getInstance().execute(request, null) { percent, _, _ ->
                        if (percent > 0) {
                            status.value = "Song wird geladen … ${percent.toInt()} %"
                            mark((percent / 100f).coerceAtMost(0.99f))
                        }
                    }
                    mark(1f)
                    finished.value++
                    say(app, "Song gespeichert.", toast)
                } catch (e: Throwable) {
                    mark(null)
                    say(app, "Download fehlgeschlagen: " + (e.message?.lineSequence()?.lastOrNull { it.isNotBlank() }?.take(140) ?: "unbekannt"), toast)
                }
            }
        }
    }
}
