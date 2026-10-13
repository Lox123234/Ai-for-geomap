<#
.SYNOPSIS
    Generates a synthetic survey flight for manual and acceptance tests (Windows PowerShell 5.1+).
.DESCRIPTION
    Rows x Cols JPEGs (default 10 x 13 = 130 photos, 4000x3000, ~1.6 MB each) on a GPS grid near
    43.23 N 76.93 E with EXIF make/model/time, plus overview.png, ortho-preview.tif and a corrupt
    DJI_BROKEN.JPG. Synthetic images only: no personal photos and nothing to commit to Git.
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\scripts\generate-test-flight.ps1 -OutDir "$env:TEMP\GeoMapTestFlight"
#>
param([Parameter(Mandatory=$true)][string]$OutDir, [int]$Rows = 10, [int]$Cols = 13, [int]$Width = 4000, [int]$Height = 3000)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
if (-not ('Grain' -as [type])) {
Add-Type -ReferencedAssemblies System.Drawing @'
using System; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
public static class Grain {
  // Adds sensor-like noise so JPEG sizes resemble real 12 MP aerial photos (~4-6 MB).
  public static void Apply(Bitmap bmp, int seed, int amplitude) {
    var rect = new Rectangle(0, 0, bmp.Width, bmp.Height);
    var data = bmp.LockBits(rect, ImageLockMode.ReadWrite, PixelFormat.Format32bppArgb);
    int n = data.Stride * data.Height; var buf = new byte[n]; Marshal.Copy(data.Scan0, buf, 0, n);
    var rnd = new Random(seed); var noise = new byte[n]; rnd.NextBytes(noise);
    for (int i = 0; i < n; i++) { if ((i & 3) == 3) continue; int v = buf[i] + (noise[i] % (2 * amplitude + 1)) - amplitude; buf[i] = (byte)(v < 0 ? 0 : v > 255 ? 255 : v); }
    Marshal.Copy(buf, 0, data.Scan0, n); bmp.UnlockBits(data);
  }
}
'@
}

function Dms([double]$v) { $d = [math]::Floor($v); $m = [math]::Floor(($v - $d) * 60); $s = (($v - $d) * 60 - $m) * 60; @($d, $m, $s) }
function New-ExifTiff([double[]]$lat, [double[]]$lon, [string]$make, [string]$model, [string]$date) {
    $ms = New-Object IO.MemoryStream; $w = New-Object IO.BinaryWriter($ms)
    $makeB = [Text.Encoding]::ASCII.GetBytes($make + "`0"); $modelB = [Text.Encoding]::ASCII.GetBytes($model + "`0")
    $dateB = [Text.Encoding]::ASCII.GetBytes($date + "`0")
    $mainOff = 8; $exifOff = $mainOff + 2 + 4*12 + 4; $gpsOff = $exifOff + 2 + 12 + 4; $data = $gpsOff + 2 + 4*12 + 4
    $latOff = $data; $lonOff = $latOff + 24; $makeOff = $lonOff + 24; $modelOff = $makeOff + $makeB.Length; $dateOff = $modelOff + $modelB.Length
    function E([int]$tag, [int]$type, [int]$count, [int]$value) { $w.Write([uint16]$tag); $w.Write([uint16]$type); $w.Write([uint32]$count); $w.Write([uint32]$value) }
    function Inline([byte[]]$b, [int]$offset) { if ($b.Length -le 4) { $v = 0; for ($i = 0; $i -lt $b.Length; $i++) { $v += [int]$b[$i] -shl (8 * $i) }; $v } else { $offset } }
    $w.Write([byte[]](0x49,0x49)); $w.Write([uint16]42); $w.Write([uint32]$mainOff)
    $w.Write([uint16]4); E 0x010F 2 $makeB.Length (Inline $makeB $makeOff); E 0x0110 2 $modelB.Length (Inline $modelB $modelOff); E 0x8769 4 1 $exifOff; E 0x8825 4 1 $gpsOff; $w.Write([uint32]0)
    $w.Write([uint16]1); E 0x9003 2 $dateB.Length $dateOff; $w.Write([uint32]0)
    $w.Write([uint16]4); E 1 2 2 ([int][char]'N'); E 2 5 3 $latOff; E 3 2 2 ([int][char]'E'); E 4 5 3 $lonOff; $w.Write([uint32]0)
    foreach ($v in $lat + $lon) { $w.Write([uint32][math]::Round($v * 1000)); $w.Write([uint32]1000) }
    $w.Write($makeB); $w.Write($modelB); $w.Write($dateB); $w.Flush()
    return $ms.ToArray()
}
$codec = [Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
$params = New-Object Drawing.Imaging.EncoderParameters(1)
$params.Param[0] = New-Object Drawing.Imaging.EncoderParameter([Drawing.Imaging.Encoder]::Quality, [long]85)
$rand = New-Object Random 42
$bmp = New-Object Drawing.Bitmap($Width, $Height); $g = [Drawing.Graphics]::FromImage($bmp)
$font = New-Object Drawing.Font('Segoe UI', 120, [Drawing.FontStyle]::Bold)
$n = 0; $start = Get-Date -Year 2026 -Month 9 -Day 20 -Hour 10 -Minute 0 -Second 0
for ($r = 0; $r -lt $Rows; $r++) {
    for ($c = 0; $c -lt $Cols; $c++) {
        $n++
        $lat = 43.2300 + $r * 0.0004; $lon = 76.9300 + $c * 0.0005
        $g.Clear([Drawing.Color]::FromArgb(70 + ($r * 13) % 80, 90 + ($c * 11) % 90, 60))
        for ($i = 0; $i -lt 220; $i++) {
            $brush = New-Object Drawing.SolidBrush([Drawing.Color]::FromArgb($rand.Next(40, 200), $rand.Next(40, 200), $rand.Next(40, 160)))
            $g.FillRectangle($brush, $rand.Next(0, $Width), $rand.Next(0, $Height), $rand.Next(40, 600), $rand.Next(40, 400)); $brush.Dispose()
        }
        $g.DrawString(('DJI_{0:D4}  N{1:F4} E{2:F4}' -f $n, $lat, $lon), $font, [Drawing.Brushes]::White, 80, 80)
        [Grain]::Apply($bmp, $n, 10)
        $ms = New-Object IO.MemoryStream; $bmp.Save($ms, $codec, $params); $bytes = $ms.ToArray()
        $tiff = New-ExifTiff (Dms $lat) (Dms $lon) 'DJI' 'FC3411' ($start.AddSeconds($n * 2).ToString('yyyy:MM:dd HH:mm:ss'))
        $exif = [byte[]](0x45,0x78,0x69,0x66,0,0) + $tiff; $len = $exif.Length + 2
        $out = New-Object IO.MemoryStream
        $out.Write($bytes, 0, 2); $out.Write([byte[]](0xFF, 0xE1, [byte]($len -shr 8), [byte]($len -band 0xFF)), 0, 4); $out.Write($exif, 0, $exif.Length); $out.Write($bytes, 2, $bytes.Length - 2)
        [IO.File]::WriteAllBytes((Join-Path $OutDir ('DJI_{0:D4}.JPG' -f $n)), $out.ToArray())
    }
}
$g.Dispose(); $bmp.Dispose()
$small = New-Object Drawing.Bitmap(1200, 800); $gs = [Drawing.Graphics]::FromImage($small); $gs.Clear([Drawing.Color]::DarkSlateBlue); $gs.Dispose()
$small.Save((Join-Path $OutDir 'overview.png'), [Drawing.Imaging.ImageFormat]::Png); $small.Save((Join-Path $OutDir 'ortho-preview.tif'), [Drawing.Imaging.ImageFormat]::Tiff); $small.Dispose()
[IO.File]::WriteAllBytes((Join-Path $OutDir 'DJI_BROKEN.JPG'), [byte[]](0xFF,0xD8,0xFF,0xE1) + [byte[]](1..500 | ForEach-Object { [byte](($_ * 37) % 256) }))
$files = Get-ChildItem $OutDir -File
"{0} files, {1:N1} MB" -f $files.Count, (($files | Measure-Object Length -Sum).Sum / 1MB)
