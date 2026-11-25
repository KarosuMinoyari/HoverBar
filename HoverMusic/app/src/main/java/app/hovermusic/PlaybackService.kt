package app.hovermusic

import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import androidx.media3.common.AudioAttributes
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.Player
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.session.MediaSession
import androidx.media3.session.MediaSessionService

/** Spielt im Hintergrund weiter und zeigt die Steuerung in der Benachrichtigung und auf dem Sperrbildschirm. */
class PlaybackService : MediaSessionService() {
    private var session: MediaSession? = null

    override fun onCreate() {
        super.onCreate()
        val player = ExoPlayer.Builder(this)
            .setAudioAttributes(
                AudioAttributes.Builder().setUsage(C.USAGE_MEDIA).setContentType(C.AUDIO_CONTENT_TYPE_MUSIC).build(),
                true,
            )
            .setHandleAudioBecomingNoisy(true)  // Kopfhörer raus -> Pause
            .setWakeMode(C.WAKE_MODE_NETWORK)
            .build()
        val prefs = Prefs(this)
        player.addListener(object : Player.Listener {
            override fun onMediaItemTransition(mediaItem: MediaItem?, reason: Int) {
                mediaItem?.mediaId?.let { prefs.addPlay(it) }
            }
        })
        session = MediaSession.Builder(this, player).build()
        // Widget bei jedem Wechsel neu zeichnen
        player.addListener(object : Player.Listener {
            override fun onEvents(p: Player, events: Player.Events) {
                if (events.containsAny(Player.EVENT_MEDIA_ITEM_TRANSITION, Player.EVENT_IS_PLAYING_CHANGED, Player.EVENT_PLAY_WHEN_READY_CHANGED,
                        Player.EVENT_MEDIA_METADATA_CHANGED, Player.EVENT_POSITION_DISCONTINUITY, Player.EVENT_PLAYBACK_STATE_CHANGED)) publishWidget(p)
            }
        })
        publishWidget(player)  // frisch gestartet: ein altes „läuft“ im Widget zurücksetzen
        // Schnitte: entfernte Bereiche beim Abspielen überspringen (die Datei bleibt unverändert)
        handler.post(object : Runnable {
            private var tick = 0
            override fun run() {
                skipCuts(player)
                // Fortschritt im Widget: einmal pro Sekunde, nur beim Spielen, bei Bildschirm an und wenn ein Widget ihn zeigt
                if (++tick % 10 == 0 && player.isPlaying && getSystemService(PowerManager::class.java).isInteractive &&
                    Widgets.wantsProgress(this@PlaybackService)
                ) Widgets.progress(this@PlaybackService, permille(player))
                handler.postDelayed(this, 100)
            }
        })
    }

    private val handler = Handler(Looper.getMainLooper())

    private fun skipCuts(p: Player) {
        if (!p.isPlaying) return
        val cuts = p.currentMediaItem?.mediaMetadata?.extras?.getString("cuts").orEmpty()
        if (cuts.isEmpty()) return
        val pos = p.currentPosition
        val r = Cuts.removed(cuts).firstOrNull { pos >= it.first && pos < it.last - 50 } ?: return
        if (p.duration <= 0 || r.last < p.duration - 300) p.seekTo(r.last)
        else if (p.repeatMode == Player.REPEAT_MODE_ONE) p.seekTo(0)
        else if (p.hasNextMediaItem()) p.seekToNextMediaItem()
        else {
            p.pause()
            p.seekTo(0)
        }
    }

    /** Fortschritt 0..1000 ohne die herausgeschnittenen Stellen. */
    private fun permille(p: Player): Int {
        val cuts = p.currentMediaItem?.mediaMetadata?.extras?.getString("cuts").orEmpty()
        val total = if (p.duration > 0) Cuts.kept(cuts, p.duration) else 0
        return if (total > 0) (Cuts.visible(cuts, p.currentPosition) * 1000 / total).toInt().coerceIn(0, 1000) else 0
    }

    private fun publishWidget(p: Player) {
        val m = p.currentMediaItem?.mediaMetadata
        if (m == null) {  // nichts geladen: letzten Titel stehen lassen, Knöpfe öffnen die App
            Widgets.idle(this)
            return
        }
        val playing = p.playWhenReady && p.playbackState != Player.STATE_ENDED
        Widgets.publish(this, NowState(m.title?.toString().orEmpty(), m.artist?.toString().orEmpty(), m.artworkUri?.toString().orEmpty(), playing, true, permille(p)))
    }

    override fun onGetSession(controllerInfo: MediaSession.ControllerInfo): MediaSession? = session

    override fun onTaskRemoved(rootIntent: android.content.Intent?) {
        val p = session?.player
        if (p == null || !p.playWhenReady || p.mediaItemCount == 0) stopSelf()
    }

    override fun onDestroy() {
        handler.removeCallbacksAndMessages(null)
        Widgets.idle(this)
        session?.run {
            player.release()
            release()
        }
        session = null
        super.onDestroy()
    }
}
