# sim_test_matrix.ps1
# -------------------
# Runs the simulation test without a window (traffic_engine.exe --sim-test) on
# a set of networks and seeds at the same time, and writes a summary for each
# network: completed trips, vehicles removed by the gridlock rule, overlaps and
# vehicles that do not move for a long time.
#
# Use it before and after a change of the traffic rules. The overlaps and the
# long standstills must not increase.
#
#   powershell -ExecutionPolicy Bypass -File C:\...\tests\sim_test_matrix.ps1 -Name before
#   powershell -ExecutionPolicy Bypass -File C:\...\tests\sim_test_matrix.ps1 -Name after -Seconds 3600
#
# Parameters:
#   -Name      name of the run (folder of the reports)
#   -Seconds   simulated time of each test (default 7200 = 2 hours)
#   -Seeds     seeds of the random numbers (default 1, 2, 3)
#   -Extra     more arguments for each test, for example "--sim-gridlock 0"
#   -Parallel  tests at the same time (default 6)
#   -Exe       the program (default bin\Release\traffic_engine.exe)
#   -Out       folder of the reports (default %APPDATA%\traffic_engine\log\sim_matrix\<Name>)

param(
	[string]$Name = 'matrix',
	[int]$Seconds = 7200,
	[int[]]$Seeds = @(1, 2, 3),
	[string]$Extra = '',
	[int]$Parallel = 6,
	[string]$Exe = '',
	[string]$Out = ''
)

$root = Split-Path -Parent $PSScriptRoot
if (-not $Exe) { $Exe = Join-Path $root 'bin\Release\traffic_engine.exe' }
if (-not $Out) { $Out = Join-Path $env:APPDATA "traffic_engine\log\sim_matrix\$Name" }
if (-not (Test-Path $Exe)) { Write-Error "The program is not found: $Exe. Build it first."; exit 2 }
New-Item -ItemType Directory -Force $Out | Out-Null

$osm = Join-Path $PSScriptRoot 'osm'
$nets = @(
	@{ n = 'demo'; a = @() },
	@{ n = 'kizilay'; a = @('--sim-file', "$osm\kizilay.osm") },
	@{ n = 'eskisehir'; a = @('--sim-file', "$osm\eskisehir.osm") },
	@{ n = 'eskisehir70'; a = @('--sim-file', "$osm\eskisehir.osm", '--shot-density', '70') },
	@{ n = 'kizilay200'; a = @('--sim-file', "$osm\kizilay.osm", '--shot-density', '200') }
)

$jobs = @()
foreach ($net in $nets)
{
	foreach ($seed in $Seeds)
	{
		$jobs += @{ args = @('--sim-test', "$Seconds", '--sim-report', "$Out\$($net.n)_s$seed.txt", '--sim-clock', '7:30', '--sim-seed', "$seed") + $net.a }
	}
}

$running = @()
$t0 = Get-Date
foreach ($j in $jobs)
{
	while (($running | Where-Object { -not $_.HasExited }).Count -ge $Parallel) { Start-Sleep -Milliseconds 500 }
	$a = $j.args
	if ($Extra) { $a += ($Extra -split ' ') }
	$running += Start-Process -FilePath $Exe -ArgumentList $a -PassThru -WindowStyle Hidden
}
$running | ForEach-Object { $_.WaitForExit() }
"Reports: $Out"
"Time: {0:N0} s" -f ((Get-Date) - $t0).TotalSeconds

foreach ($net in $nets)
{
	$sum = @{ trips = 0; removed = 0; overlap = 0; overlap_nc = 0; long = 0; vlong = 0 }
	foreach ($seed in $Seeds)
	{
		$f = "$Out\$($net.n)_s$seed.txt"
		if (-not (Test-Path $f)) { "$($net.n) seed $seed : no report"; continue }
		$lines = Get-Content $f -Encoding UTF8
		$res = $lines | Where-Object { $_ -match '^Result' }
		if ($res -match 'completed (\d+), removed \(gridlock\) (\d+)\. Overlaps (\d+)')
		{
			$sum.trips += [int]$Matches[1]
			$sum.removed += [int]$Matches[2]
			$sum.overlap += [int]$Matches[3]
		}
		$sum.overlap_nc += ($lines | Where-Object { $_ -match '\] Overlap ' -and $_ -match 'without a common node' }).Count
		# The longest standstill of each vehicle (a vehicle has many lines).
		$max = @{}
		foreach ($line in ($lines | Where-Object { $_ -match '\] (Stuck|Deadlock):' }))
		{
			if ($line -match 'vehicle (\d+) \(\w+\) does not move for (\d+) s')
			{
				$id = [int]$Matches[1]
				$t = [int]$Matches[2]
				if (-not $max.ContainsKey($id) -or $max[$id] -lt $t) { $max[$id] = $t }
			}
		}
		$sum.long += ($max.Values | Where-Object { $_ -ge 300 }).Count
		$sum.vlong += ($max.Values | Where-Object { $_ -ge 900 }).Count
	}
	"{0,-12} trips {1,6} | removed {2,5} | overlaps {3,5} (no common node {4,5}, other {5,4}) | standstill >= 300 s {6,4} | >= 900 s {7,4}" -f $net.n, $sum.trips, $sum.removed, $sum.overlap, $sum.overlap_nc, ($sum.overlap - $sum.overlap_nc), $sum.long, $sum.vlong
}
