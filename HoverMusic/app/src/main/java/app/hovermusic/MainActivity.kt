package app.hovermusic

import android.content.ClipboardManager
import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.TextButton
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.mutableIntStateOf
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import com.google.mlkit.vision.barcode.common.Barcode
import com.google.mlkit.vision.codescanner.GmsBarcodeScannerOptions
import com.google.mlkit.vision.codescanner.GmsBarcodeScanning
import android.Manifest
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.material3.Button
import androidx.compose.material3.AlertDialog
import androidx.compose.foundation.layout.heightIn
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.LocalTextStyle
import androidx.compose.material3.Surface
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.animation.core.tween
import androidx.compose.ui.draw.blur
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import coil.compose.AsyncImage
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat

class MainActivity : ComponentActivity() {
    private val vm: MusicViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        publishShareShortcut(this)
        setContent { Themed(vm) { App(vm) } }
    }
}

/** Farben, Schrift und Hintergrundbild aus den Einstellungen; Surface sorgt für helle Schrift auf dunklem Grund. */
@Composable
private fun Themed(vm: MusicViewModel, content: @Composable () -> Unit) {
    val look = vm.look.value
    MaterialTheme(colorScheme = look.colors()) {
        Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background, contentColor = MaterialTheme.colorScheme.onBackground) {
            if (look.image.isNotEmpty()) {
                AsyncImage(
                    model = Uri.parse(look.image), contentDescription = null, contentScale = ContentScale.Crop, alpha = look.imageStrength,
                    modifier = Modifier.fillMaxSize().let { if (look.blur) it.blur(30.dp) else it },
                )
            }
            val style = LocalTextStyle.current.copy(fontFamily = look.fontFamily, fontWeight = if (look.bold) FontWeight.SemiBold else null)
            CompositionLocalProvider(LocalTextStyle provides style, content = content)
        }
    }
}

@Composable
private fun App(vm: MusicViewModel) {
    val ctx = androidx.compose.ui.platform.LocalContext.current
    val hasFolder by vm.hasFolder.collectAsState()
    val showNow by vm.showNow.collectAsState()
    var showSettings by remember { mutableStateOf(false) }
    var showSuggest by remember { mutableStateOf(false) }
    UpdatePopup()
    val look = vm.look.value

    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri != null) vm.setFolder(uri)
    }
    val notifPermission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { }
    LaunchedEffect(Unit) {  // Android 13+: Benachrichtigung für die Wiedergabe-Steuerung erlauben
        if (Build.VERSION.SDK_INT >= 33 &&
            ContextCompat.checkSelfPermission(ctx, Manifest.permission.POST_NOTIFICATIONS) != android.content.pm.PackageManager.PERMISSION_GRANTED
        ) notifPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
    }

    Box(Modifier.fillMaxSize().systemBarsPadding()) {
        if (!hasFolder) {
            SetupScreen(vm, onPick = { picker.launch(null) })
        } else {
            LibraryScreen(vm, onOpenNow = { vm.showNow.value = true }, onSettings = { showSettings = true }, onPickFolder = { picker.launch(null) }, onSuggest = { showSuggest = true })
            if (showSuggest) SuggestScreen(vm, onClose = { showSuggest = false })
            AnimatedVisibility(
                visible = showNow,
                enter = slideInVertically(tween(look.millis), initialOffsetY = { it }),
                exit = slideOutVertically(tween(look.millis), targetOffsetY = { it }),
            ) {
                NowPlayingScreen(vm, onClose = { vm.showNow.value = false })
            }
        }
        if (showSettings) SettingsScreen(vm, onClose = { showSettings = false }, onPickFolder = { picker.launch(null) })
    }
    BackHandler(enabled = showNow && !showSettings) { vm.showNow.value = false }
}

/** Bei jedem Öffnen nach Updates sehen; gibt es eines, zeigt ein Fenster, was neu ist. */
@Composable
private fun UpdatePopup() {
    val ctx = androidx.compose.ui.platform.LocalContext.current
    val update by Updater.available.collectAsState()
    val status by Updater.status.collectAsState()
    var later by remember { mutableStateOf(false) }
    val owner = androidx.lifecycle.compose.LocalLifecycleOwner.current
    DisposableEffect(owner) {
        val obs = LifecycleEventObserver { _, e ->
            if (e == Lifecycle.Event.ON_START) {
                later = false
                Updater.check(ctx)
            }
        }
        owner.lifecycle.addObserver(obs)
        onDispose { owner.lifecycle.removeObserver(obs) }
    }
    val u = update ?: return
    if (later) return
    AlertDialog(
        onDismissRequest = { later = true },
        title = { Text("Neue Version ${u.version}") },
        text = {
            Column(Modifier.heightIn(max = 360.dp).verticalScroll(rememberScrollState())) {
                Text(u.notes.ifEmpty { "Es gibt ein Update." }, fontSize = 14.sp)
                status?.let { Text(it, fontSize = 12.sp, color = MaterialTheme.colorScheme.primary, modifier = Modifier.padding(top = 10.dp)) }
            }
        },
        confirmButton = { TextButton(onClick = { Updater.install(ctx, u) }) { Text("Installieren") } },
        dismissButton = { TextButton(onClick = { later = true }) { Text("Später") } },
    )
}

/** Einrichtung: QR-Code aus HoverBar scannen, dann führt die App durch Cloud-App und Ordnerwahl. */
@Composable
private fun SetupScreen(vm: MusicViewModel, onPick: () -> Unit) {
    val ctx = androidx.compose.ui.platform.LocalContext.current
    val setup by vm.setup.collectAsState()
    var message by remember { mutableStateOf<String?>(null) }
    var resumeTick by remember { mutableIntStateOf(0) }
    val owner = androidx.lifecycle.compose.LocalLifecycleOwner.current
    DisposableEffect(owner) {  // beim Zurückkehren aus dem Play Store neu prüfen, ob die Cloud-App da ist
        val obs = LifecycleEventObserver { _, e -> if (e == Lifecycle.Event.ON_RESUME) resumeTick++ }
        owner.lifecycle.addObserver(obs)
        onDispose { owner.lifecycle.removeObserver(obs) }
    }

    fun scan() {
        message = null
        val options = GmsBarcodeScannerOptions.Builder().setBarcodeFormats(Barcode.FORMAT_QR_CODE).build()
        GmsBarcodeScanning.getClient(ctx, options).startScan()
            .addOnSuccessListener { b ->
                if (!vm.applySetup(b.rawValue.orEmpty())) message = "Das ist kein HoverBar-Code. Scanne den Code aus „Handy verbinden“ in HoverBar."
            }
            .addOnFailureListener {
                message = "Scannen ist hier nicht möglich (Google Play-Dienste nötig). Kopiere den Link und nutze „Link einfügen“."
            }
    }

    fun fromClipboard() {
        val cm = ctx.getSystemService(ClipboardManager::class.java)
        val text = cm?.primaryClip?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.text?.toString().orEmpty()
        message = if (vm.applySetup(text)) null else "In der Zwischenablage ist kein HoverBar-Link."
    }

    Column(
        Modifier.fillMaxSize().padding(28.dp).verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text("HoverMusic", fontSize = 32.sp, color = MaterialTheme.colorScheme.onBackground)
        Spacer(Modifier.height(12.dp))
        val s = setup
        if (s == null) {
            Text(
                "Spielt die Musik aus HoverBar auf deinem Handy.\n\n" +
                    "1. Richte am PC in HoverBar die Erweiterung „Musik-Sync“ ein. Am Ende erscheint ein QR-Code.\n" +
                    "2. Scanne ihn hier. Die App führt dich durch den Rest.",
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                textAlign = TextAlign.Center,
                fontSize = 15.sp,
            )
            Spacer(Modifier.height(24.dp))
            Button(onClick = { scan() }, modifier = Modifier.fillMaxWidth()) { Text("QR-Code scannen") }
            TextButton(onClick = { fromClipboard() }) { Text("Link aus der Zwischenablage") }
            TextButton(onClick = onPick) { Text("Ordner direkt wählen") }
        } else {
            val name = if (s.knownCloud) s.cloud else "deine Cloud"
            Text("Einrichtung mit $name", fontSize = 18.sp, color = MaterialTheme.colorScheme.onBackground)
            Spacer(Modifier.height(16.dp))
            if (s.pkg.isNotEmpty()) {
                val installed = remember(resumeTick, s.pkg) { ctx.packageManager.getLaunchIntentForPackage(s.pkg) != null }
                Text(
                    if (installed) "1. ${s.cloud} ist auf dem Handy installiert ✓" else "1. Installiere ${s.cloud} auf dem Handy und melde dich an.",
                    color = MaterialTheme.colorScheme.onSurfaceVariant, textAlign = TextAlign.Center, fontSize = 15.sp,
                )
                Spacer(Modifier.height(8.dp))
                Button(onClick = { openCloudApp(ctx, s.pkg, installed) }, modifier = Modifier.fillMaxWidth()) {
                    Text(if (installed) "${s.cloud} öffnen (anmelden)" else "${s.cloud} im Play Store öffnen")
                }
                Spacer(Modifier.height(20.dp))
            }
            val path = listOf(name, s.parent, s.folder).filter { it.isNotEmpty() }.joinToString(" › ")
            Text(
                "${if (s.pkg.isNotEmpty()) "2" else "1"}. Wähle in der Ordnerauswahl den Ordner:\n$path",
                color = MaterialTheme.colorScheme.onSurfaceVariant, textAlign = TextAlign.Center, fontSize = 15.sp,
            )
            Spacer(Modifier.height(8.dp))
            Button(onClick = onPick, modifier = Modifier.fillMaxWidth()) { Text("Ordner „${s.folder}“ wählen") }
            TextButton(onClick = { vm.clearSetup() }) { Text("Neu scannen") }
        }
        Spacer(Modifier.height(20.dp))
        val tool by Downloader.toolVersion.collectAsState()
        val toolStatus by Downloader.status.collectAsState()
        Text(
            toolStatus ?: tool?.let { "YouTube-Downloader (yt-dlp $it) ist bereit ✓" } ?: "YouTube-Downloader wird vorbereitet …",
            color = MaterialTheme.colorScheme.onSurfaceVariant, textAlign = TextAlign.Center, fontSize = 13.sp,
        )
        if (tool == null) TextButton(onClick = { Downloader.prepare(ctx, update = true) }) { Text("Erneut versuchen") }
        message?.let {
            Spacer(Modifier.height(12.dp))
            Text(it, color = Color(0xFFFF9696), textAlign = TextAlign.Center, fontSize = 13.sp)
        }
    }
}

/** Cloud-App öffnen (zum Anmelden) oder, falls sie fehlt, ihre Play-Store-Seite. */
private fun openCloudApp(ctx: android.content.Context, pkg: String, installed: Boolean) {
    val intent = if (installed) ctx.packageManager.getLaunchIntentForPackage(pkg) else null
    try {
        ctx.startActivity(intent ?: Intent(Intent.ACTION_VIEW, Uri.parse("market://details?id=$pkg")))
    } catch (e: Exception) {
        runCatching { ctx.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse("https://play.google.com/store/apps/details?id=$pkg"))) }
    }
}