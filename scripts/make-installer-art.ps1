<#
.SYNOPSIS
    Draws the pictures of the installer wizard from the application icon
    (app/resources/geomap-1024.png, see make-icon.ps1): installer/welcome.bmp (164x314, the
    welcome and finish pages) and installer/header.bmp (150x57, the other pages).
#>
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent $PSScriptRoot
$icon = [System.Drawing.Image]::FromFile((Join-Path $root 'app\resources\geomap-1024.png'))
$outDir = Join-Path $root 'installer'

function New-Canvas([int]$w, [int]$h) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    return $bmp, $g
}

# Welcome / finish page: deep blue, the icon, the name and what the program does.
$bmp, $g = New-Canvas 164 314
$bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.Point]::new(0, 0)), ([System.Drawing.Point]::new(0, 314)),
    ([System.Drawing.Color]::FromArgb(255, 20, 42, 82)), ([System.Drawing.Color]::FromArgb(255, 7, 14, 30))
$g.FillRectangle($bg, 0, 0, 164, 314)
# Faint contour lines at the bottom
$pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(40, 120, 190, 255)), 1.2
for ($k = 0; $k -lt 7; $k++) {
    $y = 236 + $k * 11
    $g.DrawBezier($pen, -10, $y + 8, 40, $y - 14, 110, $y + 22, 175, $y - 6)
}
$g.DrawImage($icon, 17, 34, 130, 130)
$white = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::White)
$soft = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 150, 200, 255))
$center = New-Object System.Drawing.StringFormat
$center.Alignment = [System.Drawing.StringAlignment]::Center
$g.DrawString('GeoMap AI', (New-Object System.Drawing.Font 'Segoe UI Semibold', 15), $white, [System.Drawing.RectangleF]::new(0, 176, 164, 30), $center)
$g.DrawString("Фотограмметрия`nГИС · 3D · AI", (New-Object System.Drawing.Font 'Segoe UI', 9), $soft, [System.Drawing.RectangleF]::new(0, 206, 164, 40), $center)
$g.Dispose()
$bmp.Save((Join-Path $outDir 'welcome.bmp'), [System.Drawing.Imaging.ImageFormat]::Bmp)
$bmp.Dispose()

# Header of the other pages: white like the page header, the icon on the right.
$bmp, $g = New-Canvas 150 57
$g.Clear([System.Drawing.Color]::White)
$g.DrawImage($icon, 96, 4, 49, 49)
$g.Dispose()
$bmp.Save((Join-Path $outDir 'header.bmp'), [System.Drawing.Imaging.ImageFormat]::Bmp)
$bmp.Dispose()
$icon.Dispose()
Write-Host "Written: installer\welcome.bmp, installer\header.bmp"
