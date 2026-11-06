# capture_media.ps1
# -----------------
# Makes the screenshots and the animation frames of the web site (docs\)
# with the test mode of the program. Then make_site_media.py makes the
# images of docs\assets\img from them.
#
#   powershell -ExecutionPolicy Bypass -File C:\...\tools\site\capture_media.ps1 -Out C:\temp\te_media
#   python tools\site\make_site_media.py C:\temp\te_media docs\assets\img
#
# The map tiles of Kizilay come from the tile cache or from the tile server
# (internet). Each run takes some minutes.

param(
	[Parameter(Mandatory = $true)][string]$Out,
	[string]$Exe = ''
)

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Exe) { $Exe = Join-Path $root 'bin\Release\traffic_engine.exe' }
if (-not (Test-Path $Exe)) { Write-Error "The program is not found: $Exe. Build it first."; exit 2 }
$osm = Join-Path $root 'tests\osm\kizilay.osm'
foreach ($d in @('shots', 'junction', 'map', 'cong'))
{
	New-Item -ItemType Directory -Force (Join-Path $Out $d) | Out-Null
}

$common = @('--new-instance', '--lang', 'en', '--shot-size', '1600x900')
$runs = @(
	# Main image: Kizilay with the map and the traffic density colors
	@('--screenshot', "$Out\shots\hero.png", '--shot-delay', '30', '--shot-speed', '8', '--shot-density', '150', $osm),
	# Animation: the junction with traffic lights of the demo city
	@('--screenshot', "$Out\junction\j.png", '--shot-node', '3', '--shot-zoom', '7', '--shot-delay', '60', '--shot-speed', '3', '--shot-density', '170', '--shot-frames', '130', '--shot-interval', '0.2'),
	# Animation: the main junction of Kizilay on the map
	@('--screenshot', "$Out\map\k.png", '--shot-node', '6', '--shot-zoom', '3.2', '--shot-delay', '40', '--shot-speed', '3', '--shot-density', '150', '--shot-frames', '120', '--shot-interval', '0.25', $osm),
	# Animation and panel: congestion analysis at 200 % density
	@('--screenshot', "$Out\cong\c.png", '--shot-node', '6', '--shot-zoom', '1.3', '--shot-delay', '25', '--shot-speed', '16', '--shot-density', '200', '--shot-select', 'congestion', '--shot-frames', '90', '--shot-interval', '4', $osm)
)
$runs2 = @(
	# Wait reason with the link (English and Turkish)
	@('--screenshot', "$Out\shots\wait_en.png", '--shot-node', '3', '--shot-zoom', '7', '--shot-delay', '30', '--shot-speed', '2', '--shot-density', '170', '--shot-select', 'vehicle'),
	@('--lang', 'tr', '--screenshot', "$Out\shots\wait_tr.png", '--shot-node', '3', '--shot-zoom', '7', '--shot-delay', '30', '--shot-speed', '2', '--shot-density', '170', '--shot-select', 'vehicle'),
	# Dialogs and charts
	@('--screenshot', "$Out\shots\osm.png", '--shot-delay', '12', '--shot-select', 'osm'),
	@('--screenshot', "$Out\shots\program.png", '--shot-delay', '6', '--shot-select', 'program'),
	@('--screenshot', "$Out\shots\charts.png", '--shot-delay', '40', '--shot-speed', '8', '--shot-density', '150', '--shot-select', 'charts', $osm)
)
foreach ($set in @($runs, $runs2))
{
	$procs = @()
	foreach ($r in $set)
	{
		$procs += Start-Process -FilePath $Exe -ArgumentList ($common + $r) -PassThru
	}
	$procs | ForEach-Object { $_.WaitForExit() }
}
& $Exe --splash-shot "$Out\shots\splash.png" | Out-Null
& $Exe --sign-sheet "$Out\shots\signs.png" | Out-Null
"Media: $Out"
