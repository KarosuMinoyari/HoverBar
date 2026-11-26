package app.hovermusic

import android.content.Context
import android.content.Intent
import androidx.core.content.FileProvider
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import org.json.JSONArray
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

data class Release(val version: String, val url: String, val sha256: String, val notes: String = "")

/**
 * Update aus den GitHub-Releases (Tags hovermusic-vX.Y.Z, ohne Entwürfe und Vorabversionen).
 * Die Datei wird gegen die SHA-256 von GitHub geprüft; Android installiert sie nur, wenn sie mit demselben Schlüssel signiert ist.
 */
object Updater {
    private const val API = "https://api.github.com/repos/KarosuMinoyari/HoverBar/releases?per_page=30"
    private const val DOWNLOADS = "https://github.com/KarosuMinoyari/HoverBar/releases/download/"

    val status = MutableStateFlow<String?>(null)
    val available = MutableStateFlow<Release?>(null)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    @Volatile private var busy = false

    fun installed(ctx: Context): String = ctx.packageManager.getPackageInfo(ctx.packageName, 0).versionName ?: "0"

    fun isNewer(candidate: String, current: String): Boolean {
        fun parts(v: String) = v.split('.').map { it.toIntOrNull() ?: 0 }
        val a = parts(candidate)
        val b = parts(current)
        for (i in 0 until maxOf(a.size, b.size)) {
            val d = a.getOrElse(i) { 0 } - b.getOrElse(i) { 0 }
            if (d != 0) return d > 0
        }
        return false
    }

    /** Neueste App-Version aus der Release-Liste, oder null. */
    fun newest(json: String): Release? {
        val list = JSONArray(json)
        var best: Release? = null
        for (i in 0 until list.length()) {
            val r = list.getJSONObject(i)
            val tag = r.optString("tag_name")
            if (r.optBoolean("draft") || r.optBoolean("prerelease") || !tag.startsWith("hovermusic-v")) continue
            val version = tag.removePrefix("hovermusic-v")
            val assets = r.optJSONArray("assets") ?: continue
            for (k in 0 until assets.length()) {
                val a = assets.getJSONObject(k)
                val url = a.optString("browser_download_url")
                val digest = a.optString("digest")
                if (a.optString("name") != "HoverMusic-$version.apk" || !url.startsWith(DOWNLOADS) || !digest.startsWith("sha256:")) continue
                if (best == null || isNewer(version, best.version)) best = Release(version, url, digest.removePrefix("sha256:"))
            }
        }
        return best
    }

    /** Was in allen Versionen nach [current] neu ist, ohne Markdown-Zeichen. */
    fun notesSince(json: String, current: String): String {
        val list = JSONArray(json)
        val out = StringBuilder()
        for (i in 0 until list.length()) {
            val r = list.getJSONObject(i)
            val tag = r.optString("tag_name")
            if (r.optBoolean("draft") || r.optBoolean("prerelease") || !tag.startsWith("hovermusic-v")) continue
            val version = tag.removePrefix("hovermusic-v")
            if (!isNewer(version, current)) continue
            val body = r.optString("body").lines().filterNot { it.startsWith("**Full Changelog") || it.isBlank() }
                .joinToString("\n") { it.replace("**", "").replace("`", "") }
            out.append(version).append('\n').append(body).append("\n\n")
        }
        return out.toString().trim().take(1500)
    }

    private fun open(url: String) = (URL(url).openConnection() as HttpURLConnection).apply {
        connectTimeout = 15000
        readTimeout = 30000
        setRequestProperty("User-Agent", "HoverMusic")
        if (responseCode != 200) error("Server antwortet mit $responseCode")
    }

    private fun run(what: String, block: () -> Unit) {
        if (busy) return
        busy = true
        scope.launch {
            try {
                block()
            } catch (e: Exception) {
                status.value = "$what fehlgeschlagen: ${e.message}"
            }
            busy = false
        }
    }

    fun check(ctx: Context) {
        val current = installed(ctx)
        status.value = "Suche nach Updates …"
        run("Suche") {
            val json = open(API).inputStream.use { it.readBytes().toString(Charsets.UTF_8) }
            val r = newest(json)
            available.value = r?.takeIf { isNewer(it.version, current) }?.copy(notes = notesSince(json, current))
            status.value = available.value?.let { "Version ${it.version} ist verfügbar." } ?: "Du hast die neueste Version."
        }
    }

    /** Herunterladen, prüfen und den Android-Installer öffnen. */
    fun install(ctx: Context, r: Release) {
        val app = ctx.applicationContext
        status.value = "Update wird geladen …"
        run("Update") {
            val file = File(File(app.cacheDir, "update").apply { mkdirs() }, "HoverMusic.apk")
            val sha = MessageDigest.getInstance("SHA-256")
            open(r.url).inputStream.use { input ->
                file.outputStream().use { out ->
                    val buf = ByteArray(64 * 1024)
                    while (true) {
                        val n = input.read(buf)
                        if (n < 0) break
                        sha.update(buf, 0, n)
                        out.write(buf, 0, n)
                    }
                }
            }
            if (!sha.digest().joinToString("") { "%02x".format(it) }.equals(r.sha256, ignoreCase = true)) {
                file.delete()
                error("Prüfsumme stimmt nicht")
            }
            val uri = FileProvider.getUriForFile(app, "${app.packageName}.update", file)
            app.startActivity(
                Intent(Intent.ACTION_VIEW).setDataAndType(uri, "application/vnd.android.package-archive")
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_GRANT_READ_URI_PERMISSION),
            )
            status.value = "Installation bestätigen (beim ersten Mal „aus dieser Quelle“ erlauben und nochmal tippen)."
        }
    }
}
