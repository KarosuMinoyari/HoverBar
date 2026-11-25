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
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import org.json.JSONObject
import java.io.File

data class SongInfo(val id: String, val title: String, val artist: String, val durationSec: Int, val fromMs: Long = 0, val toMs: Long = 0)

/**
 * yt-dlp auf dem Handy: holt die Tonspur (m4a, ohne Umwandeln) in den App-Ordner "music".
 * shortcut: läuft ohne Vordergrunddienst; falls Android lange Downloads abbricht, WorkManager nehmen.
 * shortcut: die Songs bleiben auf dem Handy (nicht in der Cloud) und werden mit der App deinstalliert.
 */
object Downloader {
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
        return SongInfo(o.getString("id"), o.optString("track").ifEmpty { o.optString("title") }, artist, o.optInt("duration"), o.optLong("hm_from"), o.optLong("hm_to"))
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
                Song("yt:${i.id}", audio.name, i.title, i.artist, i.durationSec, Uri.fromFile(audio), cover?.let { Uri.fromFile(it) }, null, trimFromMs = i.fromMs, trimToMs = i.toMs)
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
                    YoutubeDL.getInstance().execute(request, null) { progress, _, _ ->
                        if (progress > 0) status.value = "Song wird geladen … ${progress.toInt()} %"
                    }
                    finished.value++
                    say(app, "Song gespeichert.", toast)
                } catch (e: Throwable) {
                    say(app, "Download fehlgeschlagen: " + (e.message?.lineSequence()?.lastOrNull { it.isNotBlank() }?.take(140) ?: "unbekannt"), toast)
                }
            }
        }
    }
}
