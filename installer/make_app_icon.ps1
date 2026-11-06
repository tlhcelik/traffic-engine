# ============================================================================
#  make_app_icon.ps1 - Makes the icons of Traffic Engine:
#   icons\traffic_engine.ico   program icon (traffic light on a pastel tile)
#   icons\traffic_engine.png   256 px version for the window and the splash
#   installer\traffic_engine_setup.ico  setup file icon (arrow on a pastel tile)
#
#  The icons have rounded corners and pastel colors. No external program is
#  necessary (Windows PowerShell 5.1 + .NET System.Drawing). Each ICO has
#  four sizes (256/48/32/16) with PNG data (Windows 7 or newer).
#
#  Use:
#    powershell -NoProfile -ExecutionPolicy Bypass -File installer\make_app_icon.ps1
# ============================================================================
Add-Type -AssemblyName System.Drawing

$root = Resolve-Path (Join-Path $PSScriptRoot '..')

function New-RoundedPath([float]$x, [float]$y, [float]$w, [float]$h, [float]$r)
{
	$p = New-Object System.Drawing.Drawing2D.GraphicsPath
	$d = $r * 2
	$p.AddArc($x, $y, $d, $d, 180, 90)
	$p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
	$p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
	$p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
	$p.CloseFigure()
	return $p
}

function C([int]$r, [int]$g, [int]$b, [int]$a = 255) { return [System.Drawing.Color]::FromArgb($a, $r, $g, $b) }

# Draws the program icon at size $s.
function Draw-App([System.Drawing.Graphics]$g, [int]$s)
{
	$bg = New-RoundedPath 0 0 $s $s ([float]($s * 0.22))
	$rect = New-Object System.Drawing.RectangleF(0, 0, $s, $s)
	$grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, (C 184 230 211), (C 167 199 231), 90)
	$g.FillPath($grad, $bg)

	# road at the bottom (light asphalt with a dashed line)
	$road = New-Object System.Drawing.SolidBrush (C 96 108 128)
	$g.FillRectangle($road, [float]0, [float]($s * 0.80), [float]$s, [float]($s * 0.12))
	if ($s -ge 32)
	{
		$dash = New-Object System.Drawing.SolidBrush (C 255 255 255 220)
		for ($x = $s * 0.06; $x -lt $s; $x += $s * 0.22)
		{
			$g.FillRectangle($dash, [float]$x, [float]($s * 0.855), [float]($s * 0.11), [float]([math]::Max(1, $s * 0.018)))
		}
	}

	# traffic light housing
	$hw = $s * 0.36; $hh = $s * 0.62
	$hx = ($s - $hw) / 2; $hy = $s * 0.10
	$housing = New-RoundedPath $hx $hy $hw $hh ([float]($hw * 0.30))
	$g.FillPath((New-Object System.Drawing.SolidBrush (C 61 74 92)), $housing)

	# three lamps (pastel red, yellow, green)
	$lamps = @((C 255 138 128), (C 255 224 130), (C 143 209 158))
	$lr = $hw * 0.30
	for ($i = 0; $i -lt 3; $i++)
	{
		$cx = $s / 2
		$cy = $hy + $hh * (0.19 + 0.31 * $i)
		$g.FillEllipse((New-Object System.Drawing.SolidBrush $lamps[$i]), [float]($cx - $lr), [float]($cy - $lr), [float]($lr * 2), [float]($lr * 2))
		if ($s -ge 32)
		{
			$hl = New-Object System.Drawing.SolidBrush (C 255 255 255 110)
			$g.FillEllipse($hl, [float]($cx - $lr * 0.55), [float]($cy - $lr * 0.65), [float]($lr * 0.7), [float]($lr * 0.5))
		}
	}
}

# Draws the setup icon at size $s.
function Draw-Setup([System.Drawing.Graphics]$g, [int]$s)
{
	$bg = New-RoundedPath 0 0 $s $s ([float]($s * 0.22))
	$g.FillPath((New-Object System.Drawing.SolidBrush (C 157 185 232)), $bg)
	$white = New-Object System.Drawing.SolidBrush (C 255 255 255)
	$cx = $s * 0.5
	$stem = $s * 0.14
	$g.FillRectangle($white, [float]($cx - $stem / 2), [float]($s * 0.18), [float]$stem, [float]($s * 0.32))
	$head = $s * 0.36
	$pts = @(
		(New-Object System.Drawing.PointF([float]($cx - $head / 2), [float]($s * 0.46))),
		(New-Object System.Drawing.PointF([float]($cx + $head / 2), [float]($s * 0.46))),
		(New-Object System.Drawing.PointF([float]$cx, [float]($s * 0.70))))
	$g.FillPolygon($white, $pts)
	$tray = New-RoundedPath ([float]($s * 0.22)) ([float]($s * 0.76)) ([float]($s * 0.56)) ([float]($s * 0.08)) ([float]($s * 0.03))
	$g.FillPath($white, $tray)
}

function New-Png([scriptblock]$draw, [int]$s)
{
	$bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
	$g = [System.Drawing.Graphics]::FromImage($bmp)
	$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
	$g.Clear([System.Drawing.Color]::Transparent)
	& $draw $g $s
	$g.Dispose()
	$ms = New-Object System.IO.MemoryStream
	$bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
	$bmp.Dispose()
	$bytes = $ms.ToArray()
	$ms.Dispose()
	return ,$bytes
}

function Write-Ico([scriptblock]$draw, [string]$out)
{
	$sizes = @(256, 48, 32, 16)
	$pngs = @()
	foreach ($s in $sizes) { $pngs += ,(New-Png $draw $s) }
	$fs = [System.IO.File]::Create($out)
	$bw = New-Object System.IO.BinaryWriter($fs)
	$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
	$offset = 6 + 16 * $sizes.Count
	for ($i = 0; $i -lt $sizes.Count; $i++)
	{
		$dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
		$bw.Write([Byte]$dim); $bw.Write([Byte]$dim); $bw.Write([Byte]0); $bw.Write([Byte]0)
		$bw.Write([UInt16]1); $bw.Write([UInt16]32)
		$bw.Write([UInt32]$pngs[$i].Length); $bw.Write([UInt32]$offset)
		$offset += $pngs[$i].Length
	}
	foreach ($p in $pngs) { $bw.Write($p) }
	$bw.Flush(); $bw.Close(); $fs.Close()
	Write-Host "[OK] $out"
}

$appDraw = { param($g, $s) Draw-App $g $s }
$setupDraw = { param($g, $s) Draw-Setup $g $s }

Write-Ico $appDraw (Join-Path $root 'icons\traffic_engine.ico')
[System.IO.File]::WriteAllBytes((Join-Path $root 'icons\traffic_engine.png'), (New-Png $appDraw 256))
Write-Host "[OK] $(Join-Path $root 'icons\traffic_engine.png')"
Write-Ico $setupDraw (Join-Path $root 'installer\traffic_engine_setup.ico')
