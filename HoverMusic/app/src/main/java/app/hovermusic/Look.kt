package app.hovermusic

import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** Grafik-Einstellungen wie in HoverBar. Alle Extras sind von Haus aus an. */
data class Look(
    val accent: Int = 0,
    val image: String = "",
    val imageStrength: Float = 0.35f,
    val blur: Boolean = true,       // Hintergrundbild weichzeichnen (Glas)
    val font: Int = 0,
    val bold: Boolean = false,
    val speed: Int = 1,             // Ausfahren: Langsam, Normal, Schnell, Sofort
    val tileCovers: Boolean = true,
    val lines: Boolean = true,
    val coverBackdrop: Boolean = true,
) {
    val fontFamily: FontFamily get() = listOf(FontFamily.Default, FontFamily.Serif, FontFamily.Monospace, FontFamily.Cursive)[font.coerceIn(0, 3)]
    val millis: Int get() = listOf(600, 300, 150, 0)[speed.coerceIn(0, 3)]

    fun colors() = accents[accent.coerceIn(0, accents.lastIndex)].let { a ->
        darkColorScheme(
            primary = a,
            onPrimary = Color.White,
            secondaryContainer = lerp(Color(0xFF24242F), a, 0.4f),
            onSecondaryContainer = Color.White,
            background = lerp(Color(0xFF0E0E13), a, 0.04f),
            surface = lerp(Color(0xFF191922), a, 0.06f),
            surfaceVariant = lerp(Color(0xFF24242F), a, 0.08f),
            surfaceContainerHigh = lerp(Color(0xFF24242F), a, 0.08f),
            onBackground = Color(0xFFF5F5F8),
            onSurface = Color(0xFFF5F5F8),
            onSurfaceVariant = Color(0xFFC4C4CF),
        )
    }

    fun save(ctx: Context) {
        ctx.getSharedPreferences("look", Context.MODE_PRIVATE).edit()
            .putInt("accent", accent).putString("image", image).putFloat("strength", imageStrength).putBoolean("blur", blur)
            .putInt("font", font).putBoolean("bold", bold).putInt("speed", speed).putBoolean("tiles", tileCovers)
            .putBoolean("lines", lines).putBoolean("backdrop", coverBackdrop).apply()
    }

    companion object {
        val accents = listOf(Color(0xFF4C8DFF), Color(0xFF8E6CFF), Color(0xFFE0558F), Color(0xFFE0654C), Color(0xFF2FA77A), Color(0xFF7A808C))

        fun load(ctx: Context): Look {
            val p = ctx.getSharedPreferences("look", Context.MODE_PRIVATE)
            val d = Look()
            return Look(
                p.getInt("accent", d.accent), p.getString("image", "") ?: "", p.getFloat("strength", d.imageStrength), p.getBoolean("blur", d.blur),
                p.getInt("font", d.font), p.getBoolean("bold", d.bold), p.getInt("speed", d.speed), p.getBoolean("tiles", d.tileCovers),
                p.getBoolean("lines", d.lines), p.getBoolean("backdrop", d.coverBackdrop),
            )
        }
    }
}

@Composable
fun SettingsScreen(vm: MusicViewModel, onClose: () -> Unit, onPickFolder: () -> Unit) {
    val ctx = LocalContext.current
    val look = vm.look.value
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        if (uri != null) {
            runCatching { ctx.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION) }
            vm.setLook(vm.look.value.copy(image = uri.toString()))
        }
    }
    BackHandler { onClose() }

    Column(Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background).pointerInput(Unit) { detectTapGestures { } }.verticalScroll(rememberScrollState()).padding(horizontal = 20.dp, vertical = 8.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            IconButton(onClick = onClose) { Icon(HmIcons.Back, "Zurück") }
            Text("Einstellungen", fontSize = 24.sp, fontWeight = FontWeight.Bold)
        }

        Head("Aussehen")
        Text("Farbe")
        Row(Modifier.fillMaxWidth().padding(vertical = 10.dp), horizontalArrangement = Arrangement.SpaceBetween) {
            Look.accents.forEachIndexed { i, c ->
                Box(
                    Modifier.size(44.dp).clip(CircleShape).background(c)
                        .border(if (look.accent == i) 3.dp else 0.dp, if (look.accent == i) Color.White else c, CircleShape)
                        .clickable { vm.setLook(look.copy(accent = i)) },
                )
            }
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Hintergrundbild", Modifier.weight(1f))
            TextButton(onClick = { picker.launch(arrayOf("image/*")) }) { Text("Wählen") }
            if (look.image.isNotEmpty()) TextButton(onClick = { vm.setLook(look.copy(image = "")) }) { Text("Entfernen") }
        }
        if (look.image.isNotEmpty()) {
            Text("Bildstärke")
            Slider(look.imageStrength, { vm.setLook(look.copy(imageStrength = it)) }, valueRange = 0.05f..0.8f)
            Toggle("Glas-Effekt (Bild weichzeichnen, ab Android 12)", look.blur) { vm.setLook(look.copy(blur = it)) }
        }

        Head("Schrift & Bewegung")
        Chips(listOf("Standard", "Serif", "Mono", "Schreibschrift"), look.font) { vm.setLook(look.copy(font = it)) }
        Toggle("Fette Schrift", look.bold) { vm.setLook(look.copy(bold = it)) }
        Text("Ausfahren")
        Chips(listOf("Langsam", "Normal", "Schnell", "Sofort"), look.speed) { vm.setLook(look.copy(speed = it)) }

        Head("Extras")
        Toggle("Cover in den Kacheln", look.tileCovers) { vm.setLook(look.copy(tileCovers = it)) }
        Toggle("Trennlinien zwischen Songs", look.lines) { vm.setLook(look.copy(lines = it)) }
        Toggle("Cover als Hintergrund bei „Läuft gerade“", look.coverBackdrop) { vm.setLook(look.copy(coverBackdrop = it)) }

        Head("Musik")
        TextButton(onClick = onPickFolder) { Text("Musikordner wählen") }
        TextButton(onClick = { Downloader.prepare(ctx, update = true) }) { Text("yt-dlp aktualisieren") }
        Text(
            "Mit einer Hand laden:\n• In YouTube auf „Teilen“, dann oben auf „Song laden“ (HoverMusic). Du bleibst in YouTube.\n" +
                "• Oder „Link kopieren“ und in den Schnelleinstellungen die Kachel „Song laden“ antippen (einmal über den Stift hinzufügen).",
            fontSize = 14.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(32.dp))
    }
}

@Composable
private fun Head(text: String) {
    Text(text, Modifier.padding(top = 20.dp, bottom = 8.dp), color = MaterialTheme.colorScheme.primary, fontWeight = FontWeight.SemiBold, fontSize = 15.sp)
}

@Composable
private fun Toggle(text: String, on: Boolean, set: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth().clickable { set(!on) }.padding(vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(text, Modifier.weight(1f).padding(end = 12.dp))
        Switch(on, set)
    }
}

@Composable
private fun Chips(names: List<String>, selected: Int, set: (Int) -> Unit) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
        names.forEachIndexed { i, n -> FilterChip(selected == i, { set(i) }, { Text(n, fontSize = 12.sp, maxLines = 1) }) }
    }
}
