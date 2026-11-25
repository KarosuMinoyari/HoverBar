package app.hovermusic

import android.content.Context
import android.media.MediaCodec
import android.media.MediaExtractor
import android.media.MediaFormat
import android.net.Uri
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.media3.common.MediaItem
import androidx.media3.exoplayer.ExoPlayer
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import kotlin.math.abs
import kotlin.coroutines.coroutineContext

private class Wave(val peaks: FloatArray, val lengthMs: Long)

/** Lautstärke-Spitzen der Datei (dekodiert sie einmal ganz). */
private suspend fun loadWave(ctx: Context, uri: Uri, buckets: Int = 240): Wave? {
    val ex = MediaExtractor()
    var codec: MediaCodec? = null
    try {
        ex.setDataSource(ctx, uri, null)
        val track = (0 until ex.trackCount).firstOrNull { ex.getTrackFormat(it).getString(MediaFormat.KEY_MIME)?.startsWith("audio/") == true } ?: return null
        val format = ex.getTrackFormat(track)
        val us = format.getLong(MediaFormat.KEY_DURATION)
        if (us <= 0) return null
        ex.selectTrack(track)
        val c = MediaCodec.createDecoderByType(format.getString(MediaFormat.KEY_MIME)!!)
        codec = c
        c.configure(format, null, null, 0)
        c.start()
        val peaks = FloatArray(buckets)
        val info = MediaCodec.BufferInfo()
        var inputDone = false
        while (true) {
            coroutineContext.ensureActive()
            if (!inputDone) {
                val i = c.dequeueInputBuffer(5000)
                if (i >= 0) {
                    val n = ex.readSampleData(c.getInputBuffer(i)!!, 0)
                    if (n < 0) {
                        c.queueInputBuffer(i, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                        inputDone = true
                    } else {
                        c.queueInputBuffer(i, 0, n, ex.sampleTime, 0)
                        ex.advance()
                    }
                }
            }
            val o = c.dequeueOutputBuffer(info, 5000)
            if (o >= 0) {
                val samples = c.getOutputBuffer(o)!!.asShortBuffer()
                val b = (info.presentationTimeUs * buckets / us).toInt().coerceIn(0, buckets - 1)
                var k = 0
                while (k < samples.limit()) {  // jede 32. Probe genügt für das Bild
                    peaks[b] = maxOf(peaks[b], abs(samples.get(k).toInt()) / 32768f)
                    k += 32
                }
                c.releaseOutputBuffer(o, false)
                if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) break
            }
        }
        return Wave(peaks, us / 1000)
    } catch (e: kotlinx.coroutines.CancellationException) {
        throw e
    } catch (e: Exception) {
        return null  // ohne Wellenform geht Schneiden trotzdem
    } finally {
        runCatching { codec?.release() }
        ex.release()
    }
}

private fun ms(v: Long) = "%d:%02d.%03d".format(v / 60000, v / 1000 % 60, v % 1000)

/** Schneiden wie in HoverBar: Wellenform, zwei Linien (millisekundengenau), Probehören, Auswahl behalten/entfernen, Rückgängig. */
@Composable
fun CutScreen(song: Song, initial: String, onDone: (String) -> Unit, onCancel: () -> Unit) {
    val ctx = LocalContext.current
    val wave by produceState<Wave?>(null, song.uri) { value = withContext(Dispatchers.Default) { song.uri?.let { loadWave(ctx, it) } } }
    val length = wave?.lengthMs ?: (song.durationSec * 1000L)
    var cuts by remember { mutableStateOf(initial) }
    var from by remember { mutableLongStateOf(0L) }
    var to by remember { mutableLongStateOf(song.durationSec * 1000L) }
    val len by rememberUpdatedState(length)
    val player = remember { ExoPlayer.Builder(ctx).build() }
    DisposableEffect(Unit) { onDispose { player.release() } }

    fun setFrom(v: Long) { from = v.coerceIn(0, (to - 100).coerceAtLeast(0)) }
    fun setTo(v: Long) { to = v.coerceIn(from + 100, maxOf(len, from + 100)) }
    fun preview(a: Long, b: Long) {
        val uri = song.uri ?: return
        player.setMediaItem(
            MediaItem.Builder().setUri(uri).setClippingConfiguration(
                MediaItem.ClippingConfiguration.Builder().setStartPositionMs(a.coerceAtLeast(0)).setEndPositionMs(b).build(),
            ).build(),
        )
        player.prepare()
        player.play()
    }
    fun apply(next: String) {
        if (Cuts.kept(next, len) >= 1000) cuts = next  // mindestens 1 s muss bleiben
    }

    Dialog(onDismissRequest = onCancel, properties = DialogProperties(usePlatformDefaultWidth = false)) {
        Column(
            Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background).systemBarsPadding().verticalScroll(rememberScrollState()).padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Text("Audio schneiden", fontSize = 22.sp, fontWeight = FontWeight.Bold, color = MaterialTheme.colorScheme.onBackground)
            Text("${song.title} · neue Länge ${ms(Cuts.kept(cuts, length))}", color = MaterialTheme.colorScheme.onSurfaceVariant, fontSize = 13.sp)
            if (wave == null) Text("Wellenform wird geladen …", color = MaterialTheme.colorScheme.onSurfaceVariant, fontSize = 13.sp)

            val accent = MaterialTheme.colorScheme.primary
            Canvas(
                Modifier.fillMaxWidth().height(140.dp).background(MaterialTheme.colorScheme.surface).pointerInput(Unit) {
                    var left = true
                    fun at(x: Float) = (x / size.width * len).toLong()
                    detectHorizontalDragGestures(
                        onDragStart = { p -> left = abs(at(p.x) - from) <= abs(at(p.x) - to) },
                    ) { change, _ -> if (left) setFrom(at(change.position.x)) else setTo(at(change.position.x)) }
                },
            ) {
                if (length <= 0) return@Canvas
                fun x(t: Long) = size.width * t / length
                val peaks = wave?.peaks
                if (peaks != null) {
                    val w = size.width / peaks.size
                    for (i in peaks.indices) {
                        val t = length * i / peaks.size
                        val h = maxOf(2f, peaks[i] * size.height * 0.9f)
                        drawRect(if (t in from..to) accent else Color(0xFF55596A), Offset(i * w, (size.height - h) / 2), Size(w * 0.7f, h))
                    }
                }
                for (r in Cuts.removed(cuts)) drawRect(Color(0x99C0303C), Offset(x(r.first), 0f), Size(x(minOf(r.last, length)) - x(r.first), size.height))
                for (t in listOf(from, to)) drawRect(Color.White, Offset(x(t) - 2.dp.toPx(), 0f), Size(4.dp.toPx(), size.height))
            }

            @Composable
            fun Fine(label: String, value: Long, set: (Long) -> Unit) {
                Text("$label  ${ms(value)}", color = MaterialTheme.colorScheme.onBackground)
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    for ((text, d) in listOf("−1 s" to -1000L, "−10 ms" to -10L, "+10 ms" to 10L, "+1 s" to 1000L)) {
                        FilledTonalButton(onClick = { set(value + d) }, modifier = Modifier.weight(1f), contentPadding = androidx.compose.foundation.layout.PaddingValues(0.dp)) {
                            Text(text, fontSize = 13.sp)
                        }
                    }
                }
            }
            Fine("Anfang", from, ::setFrom)
            Fine("Ende", to, ::setTo)

            @Composable
            fun Pair(a: String, onA: () -> Unit, b: String, onB: () -> Unit) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    FilledTonalButton(onClick = onA, modifier = Modifier.weight(1f)) { Text(a, fontSize = 13.sp) }
                    FilledTonalButton(onClick = onB, modifier = Modifier.weight(1f)) { Text(b, fontSize = 13.sp) }
                }
            }
            Pair("▶  Anfang", { preview(from, minOf(from + 3000, to)) }, "▶  Ende", { preview(maxOf(from, to - 3000), to) })
            Pair("Nur Auswahl behalten", { apply(Cuts.keepSelection(cuts, from, to, len)) }, "Auswahl entfernen", { apply(Cuts.removeSelection(cuts, from, to)) })
            FilledTonalButton(onClick = { cuts = Cuts.undo(cuts) }, enabled = cuts.isNotEmpty(), modifier = Modifier.fillMaxWidth()) { Text("↶  Rückgängig") }
            Spacer(Modifier.height(4.dp))
            Button(onClick = { onDone(cuts) }, modifier = Modifier.fillMaxWidth()) { Text("Fertig") }
            TextButton(onClick = onCancel, modifier = Modifier.fillMaxWidth()) { Text("Abbrechen") }
        }
    }
}
