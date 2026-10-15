<#
.SYNOPSIS
    Draws the GeoMap AI icon and writes app/resources/geomap.ico (16-128 px as 32-bit DIBs, 256 px as PNG),
    app/resources/geomap-256.png and app/resources/geomap-1024.png.
    The icon: an isometric block of terrain (a 3D map: height-coloured, lit mesh with layered
    sides) under a location pin, framed by the corner brackets of an AI detection box, on a deep
    blue rounded square. Run after changing the drawing; the results are kept in the repository.
#>
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent $PSScriptRoot
$outDir = Join-Path $root 'app\resources'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

function C([int]$a, [int]$r, [int]$g, [int]$b) { [System.Drawing.Color]::FromArgb($a, $r, $g, $b) }
function P([double]$x, [double]$y) { [System.Drawing.PointF]::new([single]$x, [single]$y) }
function Mix($c1, $c2, [double]$t) {
    $t = [Math]::Max(0.0, [Math]::Min(1.0, $t))
    C 255 ([int]($c1.R + ($c2.R - $c1.R) * $t)) ([int]($c1.G + ($c2.G - $c1.G) * $t)) ([int]($c1.B + ($c2.B - $c1.B) * $t))
}
function Shade($c, [double]$k) {
    C $c.A ([int][Math]::Min(255, $c.R * $k)) ([int][Math]::Min(255, $c.G * $k)) ([int][Math]::Min(255, $c.B * $k))
}
# Terrain: a main peak, a shoulder and a low ridge, on a gentle base.
function Height([double]$x, [double]$y) {
    $h = 0.95 * [Math]::Exp(-((($x - 0.18) * ($x - 0.18)) + (($y + 0.22) * ($y + 0.22))) / 0.16)
    $h += 0.55 * [Math]::Exp(-((($x + 0.5) * ($x + 0.5)) + (($y - 0.25) * ($y - 0.25))) / 0.10)
    $h += 0.30 * [Math]::Exp(-((($x - 0.55) * ($x - 0.55)) + (($y - 0.55) * ($y - 0.55))) / 0.08)
    $h += 0.05 * [Math]::Sin(3.1 * $x + 1.3) * [Math]::Cos(2.7 * $y)
    return [Math]::Max(0.0, $h) + 0.06
}
# Elevation ramp: deep teal, green, gold, rust, snow.
$ramp = @((C 255 20 128 160), (C 255 40 186 128), (C 255 222 214 88), (C 255 240 142 62), (C 255 255 250 238))
function RampColor([double]$t) {
    $t = [Math]::Max(0.0, [Math]::Min(0.999, [double]$t)) * ($ramp.Count - 1)
    $i = [int][Math]::Floor($t)
    Mix $ramp[$i] $ramp[$i + 1] ($t - $i)
}

function Draw-Icon([int]$S, [int]$target) {
    $bmp = New-Object System.Drawing.Bitmap $S, $S, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)
    $detail = $target -ge 48          # grid lines, brackets, strata only where they stay readable

    # Background: rounded square, deep blue gradient, a glow behind the terrain, a thin rim.
    $m = 0.035 * $S; $w = $S - 2 * $m; $r = 0.42 * $w
    $bg = New-Object System.Drawing.Drawing2D.GraphicsPath
    $bg.AddArc($m, $m, $r, $r, 180, 90); $bg.AddArc($m + $w - $r, $m, $r, $r, 270, 90)
    $bg.AddArc($m + $w - $r, $m + $w - $r, $r, $r, 0, 90); $bg.AddArc($m, $m + $w - $r, $r, $r, 90, 90)
    $bg.CloseFigure()
    $fill = New-Object System.Drawing.Drawing2D.LinearGradientBrush (P 0 0), (P $S $S), (C 255 24 48 92), (C 255 8 16 34)
    $g.FillPath($fill, $bg)
    $g.SetClip($bg)
    $glow = New-Object System.Drawing.Drawing2D.GraphicsPath
    $glow.AddEllipse(0.08 * $S, 0.12 * $S, 0.84 * $S, 0.80 * $S)
    $gb = New-Object System.Drawing.Drawing2D.PathGradientBrush $glow
    $gb.CenterColor = C 150 60 140 255
    $gb.SurroundColors = [System.Drawing.Color[]]@((C 0 60 140 255))
    $g.FillPath($gb, $glow)

    # Isometric terrain block: x to the lower right, y to the lower left, heights up.
    $cx = 0.5 * $S; $cy = 0.56 * $S
    $a = 0.19 * $S; $b = 0.095 * $S; $hs = 0.24 * $S; $depth = 0.09 * $S
    $proj = { param($x, $y, $h) P ($cx + ($x - $y) * $a) ($cy + ($x + $y) * $b - $h * $hs) }
    $n = if ($target -ge 128) { 28 } elseif ($target -ge 48) { 20 } else { 12 }
    $hmax = 1.2
    $H = New-Object 'double[,]' ($n + 1), ($n + 1)
    for ($i = 0; $i -le $n; $i++) { for ($j = 0; $j -le $n; $j++) { $H[$i, $j] = Height (-1 + 2 * $i / $n) (-1 + 2 * $j / $n) } }

    # Side walls (near edges y = 1 and x = 1), layered like a cut through the ground.
    $left = New-Object 'System.Collections.Generic.List[System.Drawing.PointF]'
    for ($i = 0; $i -le $n; $i++) { $left.Add((& $proj (-1 + 2 * $i / $n) 1 $H[$i, $n])) }
    $left.Add((P ($cx + 0 * $a) ($cy + 2 * $b + $depth))); $left.Add((P ($cx - 2 * $a) ($cy + 0 * $b + $depth)))
    $right = New-Object 'System.Collections.Generic.List[System.Drawing.PointF]'
    for ($j = $n; $j -ge 0; $j--) { $right.Add((& $proj 1 (-1 + 2 * $j / $n) $H[$n, $j])) }
    $right.Add((P ($cx + 2 * $a) ($cy + $depth))); $right.Add((P ($cx) ($cy + 2 * $b + $depth)))
    $lb = New-Object System.Drawing.Drawing2D.LinearGradientBrush (P 0 ($cy - 0.1 * $S)), (P 0 ($cy + 2 * $b + $depth)), (C 255 46 112 170), (C 255 18 52 96)
    $rb = New-Object System.Drawing.Drawing2D.LinearGradientBrush (P 0 ($cy - 0.1 * $S)), (P 0 ($cy + 2 * $b + $depth)), (C 255 30 82 136), (C 255 12 36 70)
    $g.FillPolygon($lb, $left.ToArray())
    $g.FillPolygon($rb, $right.ToArray())
    if ($detail) {
        $strata = New-Object System.Drawing.Pen (C 70 170 215 255), ([single](0.006 * $S))
        foreach ($k in 1, 2) {
            $off = $depth * $k / 3.0
            $g.DrawLine($strata, (P ($cx - 2 * $a) ($cy + $off)), (P $cx ($cy + 2 * $b + $off)))
            $g.DrawLine($strata, (P $cx ($cy + 2 * $b + $off)), (P ($cx + 2 * $a) ($cy + $off)))
        }
        $strata.Dispose()
    }

    # Top surface: lit, height-coloured quads, far ones first.
    $light = @(-0.15, 0.62, 0.77); $ll = [Math]::Sqrt(0.15 * 0.15 + 0.62 * 0.62 + 0.77 * 0.77)   # sun in front, a little left
    $grid = New-Object System.Drawing.Pen (C 46 255 255 255), ([single](0.0028 * $S))
    for ($d = 0; $d -le 2 * ($n - 1); $d++) {
        for ($i = [int][Math]::Max(0, $d - ($n - 1)); $i -le [Math]::Min($n - 1, $d); $i++) {
            $j = $d - $i
            $x0 = -1 + 2 * $i / $n; $x1 = -1 + 2 * ($i + 1) / $n; $y0 = -1 + 2 * $j / $n; $y1 = -1 + 2 * ($j + 1) / $n
            $h00 = $H[$i, $j]; $h10 = $H[($i + 1), $j]; $h01 = $H[$i, ($j + 1)]; $h11 = $H[($i + 1), ($j + 1)]
            $hm = ($h00 + $h10 + $h01 + $h11) / 4
            # Normal from the cell's slopes (heights scaled as drawn).
            $sx = (($h10 + $h11) - ($h00 + $h01)) / 2 * 1.6 / (2.0 / $n)
            $sy = (($h01 + $h11) - ($h00 + $h10)) / 2 * 1.6 / (2.0 / $n)
            $nl = [Math]::Sqrt($sx * $sx + $sy * $sy + 1)
            $dot = (-$sx * $light[0] - $sy * $light[1] + $light[2]) / ($nl * $ll)
            $k = 0.72 + 0.42 * [Math]::Max(0.0, $dot)
            $col = Shade (RampColor ([Math]::Pow([Math]::Max(0.0, ($hm - 0.06) / ($hmax - 0.2)), 0.7))) $k
            $pts = [System.Drawing.PointF[]]@((& $proj $x0 $y0 $h00), (& $proj $x1 $y0 $h10), (& $proj $x1 $y1 $h11), (& $proj $x0 $y1 $h01))
            $br = New-Object System.Drawing.SolidBrush $col
            $g.FillPolygon($br, $pts)
            $br.Dispose()
            if ($detail) { $g.DrawPolygon($grid, $pts) } else { $edge = New-Object System.Drawing.Pen $col, ([single](0.004 * $S)); $g.DrawPolygon($edge, $pts); $edge.Dispose() }
        }
    }
    $grid.Dispose()
    # Rim of the block
    $rim = New-Object System.Drawing.Pen (C 120 200 235 255), ([single](0.006 * $S))
    $g.DrawLine($rim, (P ($cx - 2 * $a) ($cy + $depth)), (P $cx ($cy + 2 * $b + $depth)))
    $g.DrawLine($rim, (P $cx ($cy + 2 * $b + $depth)), (P ($cx + 2 * $a) ($cy + $depth)))
    $rim.Dispose()

    # Location pin over the main peak, with its shadow on the slope.
    $peak = & $proj 0.18 -0.22 (Height 0.18 -0.22)
    $ps = if ($detail) { 0.085 * $S } else { 0.12 * $S }
    $shadow = New-Object System.Drawing.SolidBrush (C 90 0 0 0)
    $g.FillEllipse($shadow, $peak.X - 0.45 * $ps, $peak.Y - 0.14 * $ps, 0.9 * $ps, 0.28 * $ps)
    $tipY = $peak.Y - 0.05 * $ps
    $headC = P $peak.X ($tipY - 1.55 * $ps)
    $pin = New-Object System.Drawing.Drawing2D.GraphicsPath
    $pin.AddArc($headC.X - $ps, $headC.Y - $ps, 2 * $ps, 2 * $ps, 150, 240)
    $pin.AddLine((P ($headC.X + $ps * [Math]::Cos(30 * [Math]::PI / 180)) ($headC.Y + $ps * [Math]::Sin(30 * [Math]::PI / 180))), (P $peak.X $tipY))
    $pin.CloseFigure()
    $pinGlow = New-Object System.Drawing.Drawing2D.GraphicsPath
    $pinGlow.AddEllipse($headC.X - 2.1 * $ps, $headC.Y - 2.1 * $ps, 4.2 * $ps, 4.2 * $ps)
    $pg = New-Object System.Drawing.Drawing2D.PathGradientBrush $pinGlow
    $pg.CenterColor = C 110 255 150 60
    $pg.SurroundColors = [System.Drawing.Color[]]@((C 0 255 150 60))
    $g.FillPath($pg, $pinGlow)
    $pinFill = New-Object System.Drawing.Drawing2D.LinearGradientBrush (P 0 ($headC.Y - $ps)), (P 0 $tipY), (C 255 255 196 84), (C 255 240 92 36)
    $g.FillPath($pinFill, $pin)
    $outline = New-Object System.Drawing.Pen (C 160 120 40 10), ([single](0.006 * $S))
    $g.DrawPath($outline, $pin)
    $dotB = New-Object System.Drawing.SolidBrush (C 255 255 255 255)
    $g.FillEllipse($dotB, $headC.X - 0.42 * $ps, $headC.Y - 0.42 * $ps, 0.84 * $ps, 0.84 * $ps)

    # AI detection brackets around the block.
    if ($detail) {
        $bp = New-Object System.Drawing.Pen (C 255 80 230 255), ([single](0.022 * $S))
        $bp.StartCap = [System.Drawing.Drawing2D.LineCap]::Round; $bp.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
        $x0 = 0.10 * $S; $x1 = 0.90 * $S; $y0 = 0.10 * $S; $y1 = 0.90 * $S; $L = 0.12 * $S
        foreach ($c in @(@($x0, $y0, 1, 1), @($x1, $y0, -1, 1), @($x0, $y1, 1, -1), @($x1, $y1, -1, -1))) {
            $g.DrawLine($bp, (P $c[0] $c[1]), (P ($c[0] + $c[2] * $L) $c[1]))
            $g.DrawLine($bp, (P $c[0] $c[1]), (P $c[0] ($c[1] + $c[3] * $L)))
        }
        $bp.Dispose()
    }
    $g.ResetClip()
    $rimPen = New-Object System.Drawing.Pen (C 70 150 200 255), ([single](0.008 * $S))
    $g.DrawPath($rimPen, $bg)
    $g.Dispose()
    return $bmp
}

function Resize($bmp, [int]$size) {
    $small = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $gs = [System.Drawing.Graphics]::FromImage($small)
    $gs.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $gs.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $gs.DrawImage($bmp, 0, 0, $size, $size)
    $gs.Dispose()
    return $small
}

$sizes = 16, 24, 32, 48, 64, 128, 256
$images = @()
foreach ($size in $sizes) {
    $big = Draw-Icon ([Math]::Max(512, $size * 4)) $size
    $bmp = Resize $big $size
    $big.Dispose()
    if ($size -eq 256) {
        # 256 px as PNG (Vista+), the others as classic 32-bit DIBs that every icon reader handles.
        $ms = New-Object System.IO.MemoryStream
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Save((Join-Path $outDir 'geomap-256.png'), [System.Drawing.Imaging.ImageFormat]::Png)
        $data = $ms.ToArray()
    } else {
        $ms = New-Object System.IO.MemoryStream
        $w = New-Object System.IO.BinaryWriter $ms
        $w.Write([uint32]40); $w.Write([int32]$size); $w.Write([int32](2 * $size)); $w.Write([uint16]1); $w.Write([uint16]32)
        $w.Write([uint32]0); $w.Write([uint32]0); $w.Write([int32]0); $w.Write([int32]0); $w.Write([uint32]0); $w.Write([uint32]0)
        for ($yy = $size - 1; $yy -ge 0; $yy--) {        # bottom-up BGRA (straight alpha)
            for ($xx = 0; $xx -lt $size; $xx++) {
                $px = $bmp.GetPixel($xx, $yy)
                $w.Write([byte]$px.B); $w.Write([byte]$px.G); $w.Write([byte]$px.R); $w.Write([byte]$px.A)
            }
        }
        $maskRow = [int]([Math]::Ceiling($size / 32.0) * 4)
        $w.Write((New-Object byte[] ($maskRow * $size)))   # AND mask unused with alpha
        $w.Flush()
        $data = $ms.ToArray()
    }
    $bmp.Dispose()
    $images += , @($size, $data)
}
$large = Draw-Icon 2048 1024
$l = Resize $large 1024
$l.Save((Join-Path $outDir 'geomap-1024.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$l.Dispose(); $large.Dispose()

# ICO: ICONDIR, one ICONDIRENTRY per size, then the images.
$out = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $out
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($img in $images) {
    $size = $img[0]; $data = $img[1]
    $dim = if ($size -ge 256) { 0 } else { $size }
    $bw.Write([byte]$dim); $bw.Write([byte]$dim); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32); $bw.Write([uint32]$data.Length); $bw.Write([uint32]$offset)
    $offset += $data.Length
}
foreach ($img in $images) { $bw.Write([byte[]]$img[1]) }
$bw.Flush()
[IO.File]::WriteAllBytes((Join-Path $outDir 'geomap.ico'), $out.ToArray())
Write-Host "Written: geomap.ico, geomap-256.png, geomap-1024.png in $outDir"
