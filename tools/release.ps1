# Neue HoverBar-Version bauen und als GitHub-Release veröffentlichen.
# Vorher HB_VERSION in src\update.h erhöhen und committen. Benötigt: Visual Studio Build Tools, git, gh (angemeldet).
param([switch]$SkipBuild, [string]$Exe = 'build\HoverBar.exe')  # -SkipBuild -Exe <Pfad>: schon gebaute Datei nehmen (z. B. während HoverBar läuft)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

$m = Select-String -Path 'src\update.h' -Pattern '#define HB_VERSION L"([0-9.]+)"'
if (-not $m) { throw 'HB_VERSION nicht gefunden' }
$ver = $m.Matches[0].Groups[1].Value
$tag = "v$ver"

if (-not $SkipBuild) {
    Get-Process HoverBar -ErrorAction SilentlyContinue | Stop-Process -Force
    cmd /c build.bat
    if ($LASTEXITCODE -ne 0) { throw 'Build fehlgeschlagen' }
}

$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item $Exe "$dist\HoverBar.exe" -Force
$hash = (Get-FileHash "$dist\HoverBar.exe" -Algorithm SHA256).Hash.ToLower()
"$hash  HoverBar.exe" | Set-Content "$dist\HoverBar.exe.sha256" -Encoding ascii
Write-Host "Version $ver, SHA-256 $hash"

cmd /c tools\build_setup.bat   # dist\setupHoverBar.exe (enthält genau diese HoverBar.exe)
if ($LASTEXITCODE -ne 0) { throw 'Installer-Build fehlgeschlagen' }

gh release create $tag "$dist\setupHoverBar.exe" "$dist\HoverBar.exe" "$dist\HoverBar.exe.sha256" --title "HoverBar $ver" --generate-notes
