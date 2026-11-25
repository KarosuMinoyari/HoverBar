package app.hovermusic

import android.annotation.SuppressLint
import android.app.Activity
import android.app.PendingIntent
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.service.quicksettings.TileService
import androidx.core.content.pm.ShortcutInfoCompat
import androidx.core.content.pm.ShortcutManagerCompat
import androidx.core.graphics.drawable.IconCompat

/** Unsichtbar: nimmt den geteilten (oder kopierten) Link, startet den Download und ist wieder weg. */
class ShareActivity : Activity() {
    private var done = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (intent.action == Intent.ACTION_SEND) finishWith(intent.getStringExtra(Intent.EXTRA_TEXT).orEmpty())
    }

    // Die Zwischenablage darf erst gelesen werden, wenn das Fenster den Fokus hat
    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (!hasFocus || done) return
        val clip = getSystemService(ClipboardManager::class.java)?.primaryClip
        finishWith(clip?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.text?.toString().orEmpty())
    }

    private fun finishWith(text: String) {
        done = true
        Downloader.download(this, text, toast = true)
        finish()
    }
}

/** Kachel in den Schnelleinstellungen: lädt den kopierten YouTube-Link. */
class DownloadTile : TileService() {
    @SuppressLint("StartActivityAndCollapseDeprecated")
    override fun onClick() {
        val intent = Intent(this, ShareActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        if (Build.VERSION.SDK_INT >= 34) {
            startActivityAndCollapse(PendingIntent.getActivity(this, 0, intent, PendingIntent.FLAG_IMMUTABLE))
        } else {
            @Suppress("DEPRECATION")
            startActivityAndCollapse(intent)
        }
    }
}

/** „Song laden“ ganz oben im Teilen-Menü (Direktziel). */
fun publishShareShortcut(ctx: Context) {
    val shortcut = ShortcutInfoCompat.Builder(ctx, "download")
        .setShortLabel("Song laden")
        .setIcon(IconCompat.createWithResource(ctx, R.mipmap.ic_launcher))
        .setIntent(Intent(ctx, MainActivity::class.java).setAction(Intent.ACTION_MAIN))
        .setCategories(setOf("app.hovermusic.SHARE"))
        .setLongLived(true)
        .build()
    runCatching { ShortcutManagerCompat.pushDynamicShortcut(ctx, shortcut) }
}
