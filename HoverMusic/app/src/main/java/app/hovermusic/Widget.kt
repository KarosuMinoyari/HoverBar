package app.hovermusic

import android.app.PendingIntent
import android.appwidget.AppWidgetManager
import android.appwidget.AppWidgetProvider
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.BitmapShader
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Shader
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.util.TypedValue
import android.view.View
import android.widget.RemoteViews
import androidx.core.content.ContextCompat
import androidx.media3.common.Player
import androidx.media3.session.MediaController
import androidx.media3.session.SessionToken
import java.util.concurrent.Executors

/** Einstellungen eines einzelnen Widgets. */
data class WidgetStyle(val color: Int = 0, val size: Int = 1, val progress: Boolean = false, val filled: Boolean = true) {
    fun save(ctx: Context, id: Int) {
        ctx.getSharedPreferences("widget", Context.MODE_PRIVATE).edit()
            .putInt("color$id", color).putInt("size$id", size).putBoolean("progress$id", progress).putBoolean("filled$id", filled).apply()
    }

    companion object {
        val colors = listOf(0xFF191922, 0xFF4C8DFF, 0xFF8E6CFF, 0xFFE0558F, 0xFFE0654C, 0xFF2FA77A, 0xFFF5F5F8, 0xFF000000).map { it.toInt() }

        fun load(ctx: Context, id: Int): WidgetStyle {
            val p = ctx.getSharedPreferences("widget", Context.MODE_PRIVATE)
            return WidgetStyle(p.getInt("color$id", 0), p.getInt("size$id", 1), p.getBoolean("progress$id", false), p.getBoolean("filled$id", true))
        }

        /** Schrift/Symbole auf einer gefüllten Fläche: dunkel auf hell, sonst weiß. */
        fun contentColor(background: Int): Int {
            val light = ((background shr 16 and 255) * 299 + (background shr 8 and 255) * 587 + (background and 255) * 114) / 1000
            return if (light > 160) 0xFF14141A.toInt() else 0xFFFFFFFF.toInt()
        }
    }
}

/** Was gerade läuft; zuletzt Bekanntes bleibt gespeichert, damit das Widget auch ohne laufenden Dienst etwas zeigt. */
data class NowState(
    val title: String = "", val artist: String = "", val cover: String = "",
    val playing: Boolean = false, val active: Boolean = false, val progress: Int = 0,
)

object Widgets {
    private val io = Executors.newSingleThreadExecutor()  // Cover aus der Cloud können dauern
    private var coverKey = ""
    private var coverBitmap: Bitmap? = null

    private fun ids(ctx: Context): IntArray = AppWidgetManager.getInstance(ctx).getAppWidgetIds(ComponentName(ctx, WidgetProvider::class.java))

    fun wantsProgress(ctx: Context) = ids(ctx).any { WidgetStyle.load(ctx, it).progress }

    private fun load(ctx: Context): NowState {
        val p = ctx.getSharedPreferences("widget", Context.MODE_PRIVATE)
        return NowState(
            p.getString("title", "") ?: "", p.getString("artist", "") ?: "", p.getString("cover", "") ?: "",
            p.getBoolean("playing", false), p.getBoolean("active", false), p.getInt("pos", 0),
        )
    }

    /** Vom Wiedergabedienst bei jedem Wechsel (Song, Play/Pause, Dienst beendet). */
    fun publish(ctx: Context, s: NowState) {
        ctx.getSharedPreferences("widget", Context.MODE_PRIVATE).edit()
            .putString("title", s.title).putString("artist", s.artist).putString("cover", s.cover)
            .putBoolean("playing", s.playing).putBoolean("active", s.active).putInt("pos", s.progress).apply()
        redraw(ctx)
    }

    fun idle(ctx: Context) = publish(ctx, load(ctx).copy(playing = false, active = false))

    fun redraw(ctx: Context) {
        val app = ctx.applicationContext
        if (ids(app).isEmpty()) return
        io.execute {
            val s = load(app)
            val cover = cover(app, s.cover)
            val manager = AppWidgetManager.getInstance(app)
            for (id in ids(app)) manager.updateAppWidget(id, views(app, WidgetStyle.load(app, id), s, cover))
        }
    }

    /** Nur den Fortschritt nachziehen (jede Sekunde, solange gespielt wird und der Bildschirm an ist). */
    fun progress(ctx: Context, permille: Int) {
        val manager = AppWidgetManager.getInstance(ctx)
        for (id in ids(ctx)) {
            if (!WidgetStyle.load(ctx, id).progress) continue
            val v = RemoteViews(ctx.packageName, R.layout.widget)
            v.setProgressBar(R.id.w_progress, 1000, permille, false)
            manager.partiallyUpdateAppWidget(id, v)
        }
    }

    private fun cover(ctx: Context, uri: String): Bitmap? {
        if (uri == coverKey) return coverBitmap
        val bmp = runCatching {
            val u = Uri.parse(uri)
            val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            ctx.contentResolver.openInputStream(u)?.use { BitmapFactory.decodeStream(it, null, bounds) }
            val o = BitmapFactory.Options().apply { inSampleSize = maxOf(1, minOf(bounds.outWidth, bounds.outHeight) / 240) }
            val src = ctx.contentResolver.openInputStream(u)?.use { BitmapFactory.decodeStream(it, null, o) } ?: return@runCatching null
            val side = minOf(src.width, src.height)
            val square = Bitmap.createBitmap(src, (src.width - side) / 2, (src.height - side) / 2, side, side)
            val out = Bitmap.createBitmap(side, side, Bitmap.Config.ARGB_8888)
            val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply { shader = BitmapShader(square, Shader.TileMode.CLAMP, Shader.TileMode.CLAMP) }
            Canvas(out).drawRoundRect(0f, 0f, side.toFloat(), side.toFloat(), side * 0.16f, side * 0.16f, paint)
            out
        }.getOrNull()
        coverKey = uri
        coverBitmap = bmp
        return bmp
    }

    private fun views(ctx: Context, style: WidgetStyle, s: NowState, cover: Bitmap?): RemoteViews {
        val v = RemoteViews(ctx.packageName, R.layout.widget)
        val chosen = WidgetStyle.colors[style.color.coerceIn(0, WidgetStyle.colors.lastIndex)]
        val text = if (style.filled) WidgetStyle.contentColor(chosen) else 0xFFFFFFFF.toInt()
        val symbols = if (style.filled) text else chosen
        v.setViewVisibility(R.id.w_background, if (style.filled) View.VISIBLE else View.GONE)
        v.setInt(R.id.w_background, "setColorFilter", chosen)

        if (cover != null) v.setImageViewBitmap(R.id.w_cover, cover) else v.setImageViewResource(R.id.w_cover, R.mipmap.ic_launcher)
        v.setTextViewText(R.id.w_title, s.title.ifEmpty { "HoverMusic" })
        v.setTextViewText(R.id.w_artist, s.artist)
        v.setTextColor(R.id.w_title, text)
        v.setTextColor(R.id.w_artist, text)
        val sp = listOf(12f, 14f, 17f)[style.size.coerceIn(0, 2)]
        v.setTextViewTextSize(R.id.w_title, TypedValue.COMPLEX_UNIT_SP, sp)
        v.setTextViewTextSize(R.id.w_artist, TypedValue.COMPLEX_UNIT_SP, sp - 2)

        val pad = (listOf(11, 7, 2)[style.size.coerceIn(0, 2)] * ctx.resources.displayMetrics.density).toInt()
        v.setImageViewResource(R.id.w_play, if (s.playing) R.drawable.ic_w_pause else R.drawable.ic_w_play)
        val open = PendingIntent.getActivity(ctx, 0, Intent(ctx, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        for ((view, command) in listOf(R.id.w_prev to "prev", R.id.w_play to "toggle", R.id.w_next to "next")) {
            v.setInt(view, "setColorFilter", symbols)
            v.setViewPadding(view, pad, pad, pad, pad)
            // ohne laufende Wiedergabe gibt es nichts zu steuern: dann öffnet der Knopf die App
            v.setOnClickPendingIntent(
                view,
                if (!s.active) open
                else PendingIntent.getBroadcast(
                    ctx, command.hashCode(), Intent(ctx, WidgetProvider::class.java).setAction("app.hovermusic.widget.$command"),
                    PendingIntent.FLAG_IMMUTABLE,
                ),
            )
        }
        v.setOnClickPendingIntent(R.id.w_cover, open)
        v.setViewVisibility(R.id.w_progress, if (style.progress) View.VISIBLE else View.GONE)
        v.setProgressBar(R.id.w_progress, 1000, s.progress, false)
        return v
    }
}

class WidgetProvider : AppWidgetProvider() {
    override fun onUpdate(context: Context, appWidgetManager: AppWidgetManager, appWidgetIds: IntArray) = Widgets.redraw(context)

    override fun onReceive(context: Context, intent: Intent) {
        val command = intent.action?.removePrefix("app.hovermusic.widget.")
        if (command == intent.action || command == null) return super.onReceive(context, intent)
        val app = context.applicationContext
        val pending = goAsync()
        val future = MediaController.Builder(app, SessionToken(app, ComponentName(app, PlaybackService::class.java))).buildAsync()
        future.addListener({
            val c = runCatching { future.get() }.getOrNull()
            if (c != null) when (command) {
                "prev" -> c.seekToPrevious()
                "next" -> c.seekToNext()
                "toggle" -> if (c.playWhenReady && c.playbackState != Player.STATE_ENDED) c.pause() else c.play()
            }
            Handler(Looper.getMainLooper()).postDelayed({  // dem Befehl Zeit geben, beim Dienst anzukommen
                c?.release()
                pending.finish()
            }, 500)
        }, ContextCompat.getMainExecutor(app))
    }
}
