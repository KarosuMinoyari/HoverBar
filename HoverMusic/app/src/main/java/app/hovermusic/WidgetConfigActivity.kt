package app.hovermusic

import android.appwidget.AppWidgetManager
import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** Erscheint beim Ablegen des Widgets und beim „Widget bearbeiten“ des Startbildschirms. */
class WidgetConfigActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val id = intent.getIntExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, AppWidgetManager.INVALID_APPWIDGET_ID)
        val result = Intent().putExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, id)
        setResult(RESULT_CANCELED, result)  // Zurück-Taste: Widget nicht ablegen
        if (id == AppWidgetManager.INVALID_APPWIDGET_ID) return finish()

        setContent {
            var style by remember { mutableStateOf(WidgetStyle.load(this, id)) }
            MaterialTheme(colorScheme = Look.load(this).colors()) {
                Surface(Modifier.fillMaxSize()) {
                    Column(
                        Modifier.systemBarsPadding().verticalScroll(rememberScrollState()).padding(20.dp),
                        verticalArrangement = Arrangement.spacedBy(14.dp),
                    ) {
                        Text("Widget anpassen", fontSize = 24.sp)
                        Text(if (style.filled) "Farbe der Fläche" else "Farbe der Knöpfe")
                        for (row in WidgetStyle.colors.indices.chunked(4)) {
                            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                                for (i in row) {
                                    Box(
                                        Modifier.size(52.dp).clip(CircleShape).background(Color(WidgetStyle.colors[i]))
                                            .border(if (style.color == i) 4.dp else 1.dp, if (style.color == i) MaterialTheme.colorScheme.primary else Color.Gray, CircleShape)
                                            .clickable { style = style.copy(color = i) },
                                    )
                                }
                            }
                        }
                        Text("Größe von Schrift und Knöpfen")
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            listOf("Klein", "Mittel", "Groß").forEachIndexed { i, n -> FilterChip(style.size == i, { style = style.copy(size = i) }, { Text(n) }) }
                        }
                        Text("Aussehen")
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            FilterChip(style.filled, { style = style.copy(filled = true) }, { Text("Gefüllt") })
                            FilterChip(!style.filled, { style = style.copy(filled = false) }, { Text("Schwebend") })
                        }
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Column(Modifier.weight(1f)) {
                                Text("Fortschrittsleiste")
                                Text("Wird jede Sekunde aktualisiert und braucht etwas mehr Akku.", fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                            Switch(style.progress, { style = style.copy(progress = it) })
                        }
                        Text("Breite und Höhe änderst du auf dem Startbildschirm: Widget lange drücken und an den Rändern ziehen.", fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        Button(
                            onClick = {
                                style.save(this@WidgetConfigActivity, id)
                                Widgets.redraw(this@WidgetConfigActivity)
                                setResult(RESULT_OK, result)
                                finish()
                            },
                            modifier = Modifier.fillMaxWidth(),
                        ) { Text("Fertig") }
                    }
                }
            }
        }
    }
}
