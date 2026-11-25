package app.hovermusic

import android.content.Context
import android.net.Uri
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

/** Cover bei Deezer und iTunes suchen (wie HoverBar). Übertragen werden nur Titel und Künstler. */
object CoverSearch {
    fun deezer(json: String): List<String> {
        val a = JSONObject(json).optJSONArray("data") ?: return emptyList()
        return (0 until a.length()).mapNotNull { a.getJSONObject(it).optJSONObject("album")?.optString("cover_xl")?.takeIf { u -> u.startsWith("https://") } }
    }

    fun itunes(json: String): List<String> {
        val a = JSONObject(json).optJSONArray("results") ?: return emptyList()
        return (0 until a.length()).mapNotNull {
            a.getJSONObject(it).optString("artworkUrl100").takeIf { u -> u.startsWith("https://") }?.replace("100x100bb", "600x600bb")
        }
    }

    private fun fetch(url: String): ByteArray {
        val c = URL(url).openConnection() as HttpURLConnection
        c.connectTimeout = 10000
        c.readTimeout = 15000
        c.setRequestProperty("User-Agent", "HoverMusic")
        return c.inputStream.use { it.readBytes() }
    }

    /** Blockiert; nur im Hintergrund aufrufen. Wirft, wenn beide Dienste nicht erreichbar sind. */
    fun search(title: String, artist: String): List<String> {
        val q = URLEncoder.encode("$artist $title".trim(), "UTF-8")
        val results = listOf(
            runCatching { deezer(fetch("https://api.deezer.com/search?limit=12&q=$q").toString(Charsets.UTF_8)) },
            runCatching { itunes(fetch("https://itunes.apple.com/search?entity=song&limit=12&term=$q").toString(Charsets.UTF_8)) },
        )
        if (results.all { it.isFailure }) error("Keine Verbindung")
        return results.flatMap { it.getOrDefault(emptyList()) }.distinct().take(12)
    }

    /** Gewähltes Bild in den Zwischenspeicher laden. */
    fun download(ctx: Context, url: String): Uri {
        val f = File(ctx.cacheDir, "cover_web.jpg")
        f.writeBytes(fetch(url))
        return Uri.fromFile(f)
    }
}
