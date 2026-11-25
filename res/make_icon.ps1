# Erzeugt res\HoverBar.ico (16-256 px) und res\logo_preview.png: Logo "[<]" modern und clean
Add-Type -AssemblyName System.Drawing
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path

function New-Logo([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)
    $s = $size / 256.0

    # Hintergrund: abgerundetes Quadrat mit Verlauf (Blau -> Violett)
    $m = [Math]::Max(0.5, 8 * $s); $w = $size - 2 * $m; $r = 60 * $s
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc($m, $m, 2 * $r, 2 * $r, 180, 90)
    $path.AddArc($m + $w - 2 * $r, $m, 2 * $r, 2 * $r, 270, 90)
    $path.AddArc($m + $w - 2 * $r, $m + $w - 2 * $r, 2 * $r, 2 * $r, 0, 90)
    $path.AddArc($m, $m + $w - 2 * $r, 2 * $r, 2 * $r, 90, 90)
    $path.CloseFigure()
    $p1 = New-Object System.Drawing.PointF 0, 0
    $p2 = New-Object System.Drawing.PointF $size, $size
    $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush $p1, $p2, ([System.Drawing.Color]::FromArgb(255, 72, 132, 255)), ([System.Drawing.Color]::FromArgb(255, 139, 92, 246))
    $g.FillPath($grad, $path)
    # dezenter Glanz oben (Glas)
    $hl = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF 0, $m), (New-Object System.Drawing.PointF 0, ($size + 1)), ([System.Drawing.Color]::FromArgb(60, 255, 255, 255)), ([System.Drawing.Color]::FromArgb(0, 255, 255, 255))
    $g.FillPath($hl, $path)

    # Zeichen "[<]" in Weiß, runde Enden
    $stroke = [Math]::Max(1.7, 15 * $s)
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), $stroke
    $pen.StartCap = 'Round'; $pen.EndCap = 'Round'; $pen.LineJoin = 'Round'
    function P($x, $y) { New-Object System.Drawing.PointF ($x * $s), ($y * $s) }
    $top = 76; $bot = 180; $arm = 22
    # [
    $g.DrawLines($pen, [System.Drawing.PointF[]]@((P (70 + $arm) $top), (P 70 $top), (P 70 $bot), (P (70 + $arm) $bot)))
    # ]
    $g.DrawLines($pen, [System.Drawing.PointF[]]@((P (186 - $arm) $top), (P 186 $top), (P 186 $bot), (P (186 - $arm) $bot)))
    # <  (etwas kräftiger, optisch zentriert)
    $pen2 = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), ([Math]::Max(1.9, 18 * $s))
    $pen2.StartCap = 'Round'; $pen2.EndCap = 'Round'; $pen2.LineJoin = 'Round'
    $g.DrawLines($pen2, [System.Drawing.PointF[]]@((P 146 98), (P 114 128), (P 146 158)))
    $g.Dispose()
    return $bmp
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 96, 128, 256
$pngs = @()
foreach ($sz in $sizes) {
    $b = New-Logo $sz
    $ms = New-Object System.IO.MemoryStream
    if ($sz -ge 256) {
        $b.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)  # 256 px als PNG (üblich)
    } else {
        # kleine Größen als klassisches 32-Bit-Bitmap (BITMAPINFOHEADER + Pixel von unten + leere AND-Maske)
        $w2 = New-Object System.IO.BinaryWriter $ms
        $w2.Write([UInt32]40); $w2.Write([Int32]$sz); $w2.Write([Int32]($sz * 2))
        $w2.Write([UInt16]1); $w2.Write([UInt16]32); $w2.Write([UInt32]0)
        $maskRow = [int]([Math]::Ceiling($sz / 32.0) * 4)
        $w2.Write([UInt32]($sz * $sz * 4 + $maskRow * $sz)); $w2.Write([Int32]0); $w2.Write([Int32]0); $w2.Write([UInt32]0); $w2.Write([UInt32]0)
        for ($y = $sz - 1; $y -ge 0; $y--) {
            for ($x = 0; $x -lt $sz; $x++) {
                $c = $b.GetPixel($x, $y)
                $w2.Write([byte]$c.B); $w2.Write([byte]$c.G); $w2.Write([byte]$c.R); $w2.Write([byte]$c.A)
            }
        }
        $w2.Write((New-Object byte[] ($maskRow * $sz)))
        $w2.Flush()
    }
    $pngs += , $ms.ToArray()
    if ($sz -eq 256) { $b.Save("$dir\logo_preview.png", [System.Drawing.Imaging.ImageFormat]::Png) }
    $b.Dispose()
}

# ICO-Datei: Kopf, Verzeichnis, PNG-Daten
$out = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $out
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $sz = $sizes[$i]; $len = $pngs[$i].Length
    $bw.Write([byte]($(if ($sz -ge 256) { 0 } else { $sz })))
    $bw.Write([byte]($(if ($sz -ge 256) { 0 } else { $sz })))
    $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$len); $bw.Write([UInt32]$offset)
    $offset += $len
}
foreach ($p in $pngs) { $bw.Write($p) }
$bw.Flush()
[System.IO.File]::WriteAllBytes("$dir\HoverBar.ico", $out.ToArray())
"ok: $dir\HoverBar.ico"
