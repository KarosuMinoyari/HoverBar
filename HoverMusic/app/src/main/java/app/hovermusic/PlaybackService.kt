package app.hovermusic

import android.os.Handler
import android.os.Looper
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
        // Schnitte: entfernte Bereiche beim Abspielen überspringen (die Datei bleibt unverändert)
        handler.post(object : Runnable {
            override fun run() {
                skipCuts(player)
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

    override fun onGetSession(controllerInfo: MediaSession.ControllerInfo): MediaSession? = session

    override fun onTaskRemoved(rootIntent: android.content.Intent?) {
        val p = session?.player
        if (p == null || !p.playWhenReady || p.mediaItemCount == 0) stopSelf()
    }

    override fun onDestroy() {
        handler.removeCallbacksAndMessages(null)
        session?.run {
            player.release()
            release()
        }
        session = null
        super.onDestroy()
    }
}
