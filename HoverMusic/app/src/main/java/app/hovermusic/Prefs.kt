package app.hovermusic

import android.content.Context

/** Kleine Einstellungen auf dem Handy: gewählter Ordner, Favoriten und zuletzt gespielt (lokal), Zwischenspeicher der Bibliothek. */
class Prefs(ctx: Context) {
    private val sp = ctx.getSharedPreferences("hovermusic", Context.MODE_PRIVATE)

    var treeUri: String?
        get() = sp.getString("tree", null)
        set(v) { sp.edit().putString("tree", v).apply() }

    var jsonCache: String?
        get() = sp.getString("json", null)
        set(v) { sp.edit().putString("json", v).apply() }

    /** Einrichtung aus dem QR-Code (JSON), bis der Ordner gewählt ist. */
    var setupJson: String?
        get() = sp.getString("setup", null)
        set(v) { sp.edit().putString("setup", v).apply() }

    private fun list(key: String): List<String> = (sp.getString(key, "") ?: "").split('|').filter { it.isNotEmpty() }
    private fun setList(key: String, l: List<String>) { sp.edit().putString(key, l.joinToString("|")).apply() }

    fun favorites(): List<String> = list("favs")
    fun setFavorites(l: List<String>) = setList("favs", l)
    fun recent(): List<String> = list("recent")
    fun setRecent(l: List<String>) = setList("recent", l)

    var ytdlpUpdated: Boolean
        get() = sp.getBoolean("ytdlp_updated", false)
        set(v) { sp.edit().putBoolean("ytdlp_updated", v).apply() }

    var sortByPlays: Boolean
        get() = sp.getBoolean("sort_plays", false)
        set(v) { sp.edit().putBoolean("sort_plays", v).apply() }

    /** Wiedergaben auf diesem Handy (zählt der Wiedergabedienst). */
    fun plays(id: String): Int = sp.getInt("plays_$id", 0)
    fun addPlay(id: String) { sp.edit().putInt("plays_$id", plays(id) + 1).apply() }

    /** Beim ersten Mal die Favoriten vom PC übernehmen. */
    var favoritesSeeded: Boolean
        get() = sp.getBoolean("favs_seeded", false)
        set(v) { sp.edit().putBoolean("favs_seeded", v).apply() }
}
