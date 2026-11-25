package app.hovermusic

import org.json.JSONObject

/**
 * Einrichtung vom QR-Code in HoverBar (Musik-Sync): welcher Cloud-Dienst, welcher Ordner.
 * Der Code enthält einen Link auf die HoverMusic-Downloadseite, hinter "#hm=" steht die Einrichtung als Base64-JSON.
 * Keine Zugangsdaten: angemeldet wird in der Cloud-App auf dem Handy.
 */
data class SetupInfo(
    val cloud: String,     // z. B. "MEGA", "Google Drive"; "Cloud" = eigener Ordner
    val folder: String,    // Name des Ordners, der gewählt werden soll (HoverBar-Musik)
    val pkg: String,       // Android-Paketname der Cloud-App ("" = unbekannt)
    val parent: String,    // Ordner davor, z. B. "Meine Ablage" ("" = direkt im Stamm)
) {
    val knownCloud: Boolean get() = cloud.isNotEmpty() && cloud != "Cloud"

    fun toJson(): String = JSONObject().put("cloud", cloud).put("folder", folder).put("pkg", pkg).put("parent", parent).toString()

    companion object {
        const val MARKER = "#hm="

        /** Liest den Inhalt des QR-Codes (oder einen eingefügten Link). Null, wenn es kein HoverBar-Code ist. */
        fun parse(text: String): SetupInfo? {
            val i = text.indexOf(MARKER)
            if (i < 0) return null
            return try {
                val raw = java.util.Base64.getUrlDecoder().decode(text.substring(i + MARKER.length).trim())
                val o = JSONObject(String(raw, Charsets.UTF_8))
                if (o.optInt("v") != 1) null
                else SetupInfo(o.optString("cloud"), o.optString("folder").ifEmpty { "HoverBar-Musik" }, o.optString("pkg"), o.optString("parent"))
            } catch (e: Exception) {
                null
            }
        }

        fun fromJson(json: String): SetupInfo? = try {
            val o = JSONObject(json)
            SetupInfo(o.optString("cloud"), o.optString("folder"), o.optString("pkg"), o.optString("parent"))
        } catch (e: Exception) {
            null
        }
    }
}
