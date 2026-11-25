# Neue HoverMusic-Version bauen und als GitHub-Release (Tag hovermusic-vX.Y.Z) veröffentlichen.
# Vorher versionName/versionCode in HoverMusic\app\build.gradle.kts erhöhen und committen.
# Benötigt: JDK 17, Android SDK (ANDROID_HOME), HoverMusic\keystore.properties, git, gh (angemeldet).
# Das Release wird NICHT als "latest" markiert, damit der Update-Knopf von HoverBar nicht verwechselt wird.
param([switch]$Prerelease)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$proj = Join-Path $root 'HoverMusic'
Set-Location $proj

$m = Select-String -Path 'app\build.gradle.kts' -Pattern 'versionName = "([0-9.]+)"'
if (-not $m) { throw 'versionName nicht gefunden' }
$ver = $m.Matches[0].Groups[1].Value
$tag = "hovermusic-v$ver"
if (-not (Test-Path 'keystore.properties')) { throw 'keystore.properties fehlt (Release muss signiert werden)' }

.\gradlew.bat test assembleRelease --no-daemon
if ($LASTEXITCODE -ne 0) { throw 'Build oder Tests fehlgeschlagen' }

$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
$apk = Join-Path $dist "HoverMusic-$ver.apk"
Copy-Item 'app\build\outputs\apk\release\app-release.apk' $apk -Force
$hash = (Get-FileHash $apk -Algorithm SHA256).Hash.ToLower()
"$hash  HoverMusic-$ver.apk" | Set-Content "$apk.sha256" -Encoding ascii
Write-Host "Version $ver, SHA-256 $hash"

$args = @('release', 'create', $tag, $apk, "$apk.sha256", '--title', "HoverMusic $ver (Android)", '--latest=false', '--generate-notes')
if ($Prerelease) { $args += '--prerelease' }
gh @args
