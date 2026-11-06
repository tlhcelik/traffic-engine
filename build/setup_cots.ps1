# ============================================================================
#  setup_cots.ps1 - Collects the third-party components (COTS) in ..\cots
# ----------------------------------------------------------------------------
#  For each component, the script does these steps in sequence:
#   1) It looks in cots\. If the component is there and complete, it uses it.
#   2) It scans this computer (known folders, environment variables, PATH).
#      If it finds the component, it makes a junction (a folder link) in
#      cots\, or it copies the files if they are small.
#   3) If the component is not on this computer, it downloads the component
#      into cots\. It compiles the component with MSVC if necessary.
#
#  Components: Visual Studio (MSVC), CMake, Qt 5, Boost, Xerces-C, 7-Zip,
#              OpenGL (check only), Inno Setup (optional).
#
#  Output:
#   cots\<component>      folders or junctions
#   cots\cots_env.bat     environment for the build scripts (generated)
#   cots\cots_report.txt  result of the last scan (generated)
#
#  Use:
#   build\setup_cots.bat  (double-click)
#   powershell -NoProfile -ExecutionPolicy Bypass -File build\setup_cots.ps1 [-Force] [-Offline]
#     -Force    Scan the computer again and make all links again.
#     -Offline  Do not download. Stop with an error if a component is missing.
# ============================================================================
param(
	[switch]$Force,
	[switch]$Offline
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is much faster without the progress bar
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$TeRoot    = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Cots      = Join-Path $TeRoot 'cots'
$Downloads = Join-Path $Cots '_downloads'
$Work      = Join-Path $Cots '_work'
$Marker    = '.te_cots'
$Report    = New-Object System.Collections.Generic.List[string]

# Versions for the download step (only if the component is not on the computer).
$CMakeDownloadVersion  = '3.30.5'
$BoostDownloadVersion  = '1.83.0'
$XercesDownloadVersion = '3.3.0'
$QtDownloadVersion     = '5.15.2'
$QtDownloadArch        = 'win64_msvc2019_64'

New-Item -ItemType Directory -Force $Cots | Out-Null

# ---------------------------------------------------------------------------
#  Helpers
# ---------------------------------------------------------------------------
function Write-Step([string]$text) { Write-Host "[*] $text" -ForegroundColor Cyan }
function Write-Ok([string]$text)   { Write-Host "[OK] $text" -ForegroundColor Green }
function Write-Note([string]$text) { Write-Host "[WARNING] $text" -ForegroundColor Yellow }

function Add-Report([string]$name, [string]$version, [string]$source, [string]$path)
{
	$Report.Add(('{0,-11} {1,-12} {2,-10} {3}' -f $name, $version, $source, $path))
}

function Test-IsLink([string]$path)
{
	if (-not (Test-Path -LiteralPath $path)) { return $false }
	$item = Get-Item -LiteralPath $path -Force
	return [bool]($item.Attributes -band [IO.FileAttributes]::ReparsePoint)
}

function Get-LinkTarget([string]$path)
{
	$item = Get-Item -LiteralPath $path -Force
	if (-not $item.Target) { return $null }
	$t = [string]($item.Target | Select-Object -First 1)
	return ($t -replace '^\\\\\?\\', '' -replace '^\\\?\?\\', '')
}

# Removes a junction or a directory link. It does not touch the target folder.
function Remove-Link([string]$path)
{
	if (Test-IsLink $path) { cmd /c rmdir "$path" | Out-Null }
}

# Makes cots\<name> a junction to $target. A real folder is not replaced.
function Set-Junction([string]$name, [string]$target)
{
	$link = Join-Path $Cots $name
	$target = (Resolve-Path -LiteralPath $target).Path.TrimEnd('\')
	if (Test-IsLink $link)
	{
		$cur = Get-LinkTarget $link
		if ($cur -and ($cur.TrimEnd('\') -ieq $target)) { return $link }
		Remove-Link $link
	}
	elseif (Test-Path -LiteralPath $link)
	{
		throw "cots\$name is a real folder. The script does not replace it. Remove it, then run the script again."
	}
	New-Item -ItemType Junction -Path $link -Target $target | Out-Null
	return $link
}

# A folder that the script made (copy, download or build) has a marker file.
function Set-Marker([string]$folder, [string]$text)
{
	Set-Content -LiteralPath (Join-Path $folder $Marker) -Value $text -Encoding ASCII
}

function Get-Marker([string]$folder)
{
	$m = Join-Path $folder $Marker
	if (Test-Path -LiteralPath $m) { return (Get-Content -LiteralPath $m -Raw).Trim() }
	return ''
}

# Prepares cots\<name> for new files. Removes a link or an old folder that the
# script made. A real folder without a marker is not touched (error).
function Reset-ComponentFolder([string]$name)
{
	$dir = Join-Path $Cots $name
	if (Test-IsLink $dir) { Remove-Link $dir }
	elseif (Test-Path -LiteralPath $dir)
	{
		if (-not (Test-Path -LiteralPath (Join-Path $dir $Marker)))
		{
			throw "cots\$name is a folder that the script did not make. Remove it, then run the script again."
		}
		Remove-Item -LiteralPath $dir -Recurse -Force
	}
	New-Item -ItemType Directory -Force $dir | Out-Null
	return $dir
}

function Get-Download([string[]]$urls, [string]$fileName)
{
	if ($Offline) { throw "Offline mode: cannot download $fileName." }
	New-Item -ItemType Directory -Force $Downloads | Out-Null
	$out = Join-Path $Downloads $fileName
	if (Test-Path -LiteralPath $out) { Write-Ok "Download is already in cots\_downloads: $fileName"; return $out }
	foreach ($u in $urls)
	{
		try
		{
			Write-Step "Download: $u"
			Invoke-WebRequest -Uri $u -OutFile "$out.part" -UseBasicParsing -TimeoutSec 600
			Move-Item -LiteralPath "$out.part" -Destination $out -Force
			return $out
		}
		catch
		{
			Write-Note "Download failed ($u): $($_.Exception.Message)"
			if (Test-Path -LiteralPath "$out.part") { Remove-Item -LiteralPath "$out.part" -Force }
		}
	}
	throw "Cannot download $fileName."
}

# Compares the SHA-256 of a file with the value in a .sha256 file from the same source.
function Assert-Sha256([string]$file, [string[]]$hashUrls)
{
	foreach ($u in $hashUrls)
	{
		try
		{
			$text = (Invoke-WebRequest -Uri $u -UseBasicParsing -TimeoutSec 60).Content
			if ($text -is [byte[]]) { $text = [Text.Encoding]::ASCII.GetString($text) }
			$expected = ([regex]::Match($text, '[0-9a-fA-F]{64}')).Value.ToLowerInvariant()
			if (-not $expected) { continue }
			$actual = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
			if ($actual -ne $expected)
			{
				Remove-Item -LiteralPath $file -Force
				throw "SHA-256 is not correct for $file. The file is removed."
			}
			Write-Ok "SHA-256 is correct: $(Split-Path $file -Leaf)"
			return
		}
		catch [System.Net.WebException] { Write-Note "Cannot read $u" }
	}
	throw "Cannot get the SHA-256 value for $(Split-Path $file -Leaf)."
}

function Get-ExeVersion([string]$exe)
{
	try
	{
		$vi = (Get-Item -LiteralPath $exe).VersionInfo
		$v = [string]$vi.ProductVersion
		if (-not $v -or $v -eq '0.0.0.0') { $v = [string]$vi.FileVersion }
		if (-not $v -or $v -eq '0.0.0.0') { $v = '-' }
		return $v
	}
	catch { return '' }
}

# A complete component in cots is used again. With -Force, only the junctions
# are scanned again. The folders that the script copied, downloaded or
# compiled stay (a new compile of Xerces-C takes minutes).
function Use-Existing([string]$dir)
{
	if (-not $Force) { return $true }
	return -not (Test-IsLink $dir)
}

function Find-InPath([string]$exeName)
{
	$cmd = Get-Command $exeName -ErrorAction SilentlyContinue | Select-Object -First 1
	if ($cmd) { return $cmd.Source }
	return $null
}

$SevenZipExe = $null   # set by Install-SevenZip, used to extract .7z files

# ---------------------------------------------------------------------------
#  Visual Studio (MSVC) - check only. It is too large to download.
# ---------------------------------------------------------------------------
function Find-VisualStudio
{
	Write-Step 'Visual Studio (MSVC x64)'
	$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
	if (-not (Test-Path $vswhere)) { throw 'Visual Studio is not on this computer. Install Visual Studio 2019 or 2022 with "Desktop development with C++".' }
	$json = & $vswhere -all -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | Out-String
	# PowerShell 5.1 gives a JSON array as one object. ForEach-Object enumerates it.
	$parsed = $json | ConvertFrom-Json
	$list = @($parsed | ForEach-Object { $_ })
	if ($list.Count -eq 0) { throw 'No Visual Studio with the MSVC x64 tools is on this computer.' }
	$best = $list | Sort-Object { [version]$_.installationVersion } -Descending | Select-Object -First 1
	$major = ([version]$best.installationVersion).Major
	$gen = $null
	switch ($major)
	{
		17 { $gen = 'Visual Studio 17 2022' }
		16 { $gen = 'Visual Studio 16 2019' }
		default { throw "Visual Studio $($best.installationVersion) is not supported. Use 2019 or 2022." }
	}

	# MSVC runtime DLLs for the release package
	$crt = ''
	$redistRoot = Join-Path $best.installationPath 'VC\Redist\MSVC'
	if (Test-Path $redistRoot)
	{
		$cands = Get-ChildItem $redistRoot -Directory | Sort-Object Name -Descending
		foreach ($d in $cands)
		{
			$x = Get-ChildItem (Join-Path $d.FullName 'x64') -Directory -Filter 'Microsoft.VC14*.CRT' -ErrorAction SilentlyContinue | Select-Object -First 1
			if ($x -and (Test-Path (Join-Path $x.FullName 'vcruntime140.dll'))) { $crt = $x.FullName; break }
		}
	}
	Write-Ok "$($best.displayName) -> $gen"
	Add-Report 'msvc' $best.installationVersion 'local' $best.installationPath
	return @{ Path = $best.installationPath; Generator = $gen; Crt = $crt }
}

# ---------------------------------------------------------------------------
#  CMake (3.16 or newer)
# ---------------------------------------------------------------------------
function Get-CMakeVersion([string]$exe)
{
	try
	{
		$line = (& $exe --version | Select-Object -First 1)
		$m = [regex]::Match($line, '(\d+\.\d+\.\d+)')
		if ($m.Success) { return [version]$m.Groups[1].Value }
	}
	catch { }
	return $null
}

function Install-CMake($vs)
{
	Write-Step 'CMake (3.16 or newer)'
	$own = Join-Path $Cots 'cmake\bin\cmake.exe'
	if ((Use-Existing (Join-Path $Cots 'cmake')) -and (Test-Path $own))
	{
		$v = Get-CMakeVersion $own
		if ($v -and $v -ge [version]'3.16') { Write-Ok "cots\cmake is ready ($v)"; Add-Report 'cmake' "$v" 'cots' (Join-Path $Cots 'cmake'); return }
	}

	$cands = @()
	$cands += (Join-Path $vs.Path 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')
	$cands += 'C:\Program Files\CMake\bin\cmake.exe'
	$p = Find-InPath 'cmake.exe'; if ($p) { $cands += $p }
	$best = $null; $bestV = $null
	foreach ($c in $cands)
	{
		if (-not (Test-Path $c)) { continue }
		$v = Get-CMakeVersion $c
		if ($v -and $v -ge [version]'3.16' -and (-not $bestV -or $v -gt $bestV)) { $best = $c; $bestV = $v }
	}
	if ($best)
	{
		$root = Split-Path (Split-Path $best -Parent) -Parent
		Set-Junction 'cmake' $root | Out-Null
		Write-Ok "CMake $bestV found: $root"
		Add-Report 'cmake' "$bestV" 'local' $root
		return
	}

	$zip = Get-Download @("https://github.com/Kitware/CMake/releases/download/v$CMakeDownloadVersion/cmake-$CMakeDownloadVersion-windows-x86_64.zip") "cmake-$CMakeDownloadVersion-windows-x86_64.zip"
	$dir = Reset-ComponentFolder 'cmake'
	$tmp = Join-Path $Work 'cmake_unzip'
	if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
	Expand-Archive -LiteralPath $zip -DestinationPath $tmp
	$inner = Get-ChildItem $tmp -Directory | Select-Object -First 1
	Get-ChildItem $inner.FullName | Move-Item -Destination $dir
	Remove-Item $tmp -Recurse -Force
	Set-Marker $dir "download cmake $CMakeDownloadVersion"
	Write-Ok "CMake $CMakeDownloadVersion is in cots\cmake"
	Add-Report 'cmake' $CMakeDownloadVersion 'download' $dir
}

# ---------------------------------------------------------------------------
#  Qt 5 (MSVC, 64-bit, 5.12 or newer)
# ---------------------------------------------------------------------------
function Get-QtVersion([string]$qtDir)
{
	$f = Join-Path $qtDir 'lib\cmake\Qt5Core\Qt5CoreConfigVersion.cmake'
	if (-not (Test-Path $f)) { return $null }
	# Qt 5.12 writes the value without quotes, Qt 5.15 with quotes.
	$m = Select-String -LiteralPath $f -Pattern 'set\(PACKAGE_VERSION "?([0-9.]+)"?\)' | Select-Object -First 1
	if ($m) { return [version]$m.Matches[0].Groups[1].Value }
	return $null
}

function Test-QtDir([string]$qtDir)
{
	if (-not $qtDir -or -not (Test-Path $qtDir)) { return $false }
	foreach ($f in 'bin\qmake.exe', 'bin\windeployqt.exe', 'lib\cmake\Qt5\Qt5Config.cmake', 'lib\cmake\Qt5OpenGL', 'lib\cmake\Qt5Svg', 'lib\cmake\Qt5Network')
	{
		if (-not (Test-Path (Join-Path $qtDir $f))) { return $false }
	}
	return $true
}

function Install-Qt
{
	Write-Step 'Qt 5 (MSVC, 64-bit, 5.12 or newer)'
	$own = Join-Path $Cots 'qt5'
	if ((Use-Existing $own) -and (Test-QtDir $own))
	{
		$v = Get-QtVersion $own
		Write-Ok "cots\qt5 is ready ($v)"; Add-Report 'qt5' "$v" 'cots' $own; return
	}

	$cands = New-Object System.Collections.Generic.List[string]
	foreach ($e in 'Qt5_DIR', 'Qt_DIR')
	{
		$val = [Environment]::GetEnvironmentVariable($e)
		if ($val) { $cands.Add((Join-Path $val '..\..\..')) }
	}
	if ($env:QTDIR) { $cands.Add($env:QTDIR) }
	$qm = Find-InPath 'qmake.exe'; if ($qm) { $cands.Add((Split-Path (Split-Path $qm -Parent) -Parent)) }
	foreach ($drive in 'C:\Qt', 'D:\Qt', 'C:\Qt5')
	{
		if (-not (Test-Path $drive)) { continue }
		Get-ChildItem $drive -Directory -Recurse -Depth 2 -Filter 'msvc*_64' -ErrorAction SilentlyContinue | ForEach-Object { $cands.Add($_.FullName) }
	}

	$best = $null; $bestV = $null
	foreach ($c in $cands)
	{
		$full = Resolve-Path -LiteralPath $c -ErrorAction SilentlyContinue
		if (-not $full) { continue }
		if ((Split-Path $full.Path -Leaf) -notmatch '^msvc\d{4}_64$') { continue }
		if (-not (Test-QtDir $full.Path)) { continue }
		$v = Get-QtVersion $full.Path
		if ($v -and $v.Major -eq 5 -and $v -ge [version]'5.12' -and (-not $bestV -or $v -gt $bestV)) { $best = $full.Path; $bestV = $v }
	}
	if ($best)
	{
		Set-Junction 'qt5' $best | Out-Null
		Write-Ok "Qt $bestV found: $best"
		Add-Report 'qt5' "$bestV" 'local' $best
		return
	}

	# Download with aqtinstall (Python). The Qt online installer needs an account.
	if ($Offline) { throw 'Offline mode: Qt 5 is not on this computer.' }
	$py = Find-InPath 'python.exe'
	if (-not $py) { throw 'Qt 5 is not on this computer and Python is not available for the download. Install Qt 5.12 or newer (MSVC 64-bit).' }
	Write-Step "Download Qt $QtDownloadVersion ($QtDownloadArch) with aqtinstall"
	& $py -m pip install --user --upgrade aqtinstall
	$qtRoot = Join-Path $Cots '_qt'
	& $py -m aqt install-qt windows desktop $QtDownloadVersion $QtDownloadArch --outputdir $qtRoot
	if ($LASTEXITCODE -ne 0) { throw 'aqtinstall failed.' }
	$dir = Get-ChildItem (Join-Path $qtRoot $QtDownloadVersion) -Directory -Filter 'msvc*_64' | Select-Object -First 1
	Set-Marker $qtRoot "download qt $QtDownloadVersion"
	Set-Junction 'qt5' $dir.FullName | Out-Null
	Write-Ok "Qt $QtDownloadVersion is in cots\_qt"
	Add-Report 'qt5' $QtDownloadVersion 'download' $dir.FullName
}

# ---------------------------------------------------------------------------
#  7-Zip (also deployed with the program: it compresses old log files)
# ---------------------------------------------------------------------------
function Install-SevenZip
{
	Write-Step '7-Zip'
	$own = Join-Path $Cots '7zip'
	foreach ($n in '7z.exe', '7za.exe', '7zr.exe')
	{
		$e = Join-Path $own $n
		if ((Use-Existing $own) -and (Test-Path $e)) { $script:SevenZipExe = $e; Write-Ok "cots\7zip is ready ($n)"; Add-Report '7zip' (Get-ExeVersion $e) 'cots' $own; return }
	}

	$cands = @('C:\Program Files\7-Zip\7z.exe', 'C:\Program Files (x86)\7-Zip\7z.exe')
	try
	{
		$reg = (Get-ItemProperty 'HKLM:\SOFTWARE\7-Zip' -ErrorAction Stop).Path
		if ($reg) { $cands = @((Join-Path $reg '7z.exe')) + $cands }
	}
	catch { }
	$p = Find-InPath '7z.exe'; if ($p) { $cands += $p }
	foreach ($c in $cands)
	{
		if (-not (Test-Path $c)) { continue }
		$src = Split-Path $c -Parent
		if (-not (Test-Path (Join-Path $src '7z.dll'))) { continue }
		$dir = Reset-ComponentFolder '7zip'
		foreach ($f in '7z.exe', '7z.dll', 'License.txt')
		{
			$sf = Join-Path $src $f
			if (Test-Path $sf) { Copy-Item -LiteralPath $sf -Destination $dir }
		}
		Set-Marker $dir "copy $src"
		$script:SevenZipExe = Join-Path $dir '7z.exe'
		Write-Ok "7-Zip $(Get-ExeVersion $c) copied from $src"
		Add-Report '7zip' (Get-ExeVersion $c) 'local' $src
		return
	}

	$exe = Get-Download @('https://www.7-zip.org/a/7zr.exe') '7zr.exe'
	$dir = Reset-ComponentFolder '7zip'
	Copy-Item -LiteralPath $exe -Destination $dir
	Set-Marker $dir 'download 7zr.exe'
	$script:SevenZipExe = Join-Path $dir '7zr.exe'
	Write-Ok '7zr.exe is in cots\7zip'
	Add-Report '7zip' (Get-ExeVersion $script:SevenZipExe) 'download' $dir
}

# ---------------------------------------------------------------------------
#  Boost (headers, 1.70 or newer). Used: geometry (R-tree), graph.
# ---------------------------------------------------------------------------
function Get-BoostVersion([string]$incDir)
{
	$f = Join-Path $incDir 'boost\version.hpp'
	if (-not (Test-Path $f)) { return $null }
	$m = Select-String -LiteralPath $f -Pattern '#define BOOST_VERSION (\d+)' | Select-Object -First 1
	if (-not $m) { return $null }
	$n = [int]$m.Matches[0].Groups[1].Value
	return [version]("{0}.{1}.{2}" -f [math]::Floor($n / 100000), ([math]::Floor($n / 100) % 1000), ($n % 100))
}

function Test-BoostDir([string]$incDir)
{
	foreach ($f in 'boost\version.hpp', 'boost\geometry.hpp', 'boost\geometry\index\rtree.hpp', 'boost\graph\adjacency_list.hpp', 'boost\graph\dijkstra_shortest_paths.hpp')
	{
		if (-not (Test-Path (Join-Path $incDir $f))) { return $false }
	}
	return $true
}

function Install-Boost
{
	Write-Step 'Boost (headers, 1.70 or newer)'
	$own = Join-Path $Cots 'boost'
	if ((Use-Existing $own) -and (Test-BoostDir $own)) { $v = Get-BoostVersion $own; Write-Ok "cots\boost is ready ($v)"; Add-Report 'boost' "$v" 'cots' $own; return }

	$roots = New-Object System.Collections.Generic.List[string]
	foreach ($e in 'BOOST_ROOT', 'BOOST_INCLUDEDIR', 'Boost_DIR', 'BOOSTROOT')
	{
		$val = [Environment]::GetEnvironmentVariable($e)
		if ($val) { $roots.Add($val) }
	}
	foreach ($r in 'C:\boost', 'C:\local', 'C:\Program Files\boost', 'D:\boost') { $roots.Add($r) }

	$cands = New-Object System.Collections.Generic.List[string]
	foreach ($r in $roots)
	{
		if (-not (Test-Path $r)) { continue }
		$cands.Add($r)
		$cands.Add((Join-Path $r 'include'))
		Get-ChildItem $r -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -like 'boost*' } | ForEach-Object { $cands.Add($_.FullName) }
		$inc = Join-Path $r 'include'
		if (Test-Path $inc) { Get-ChildItem $inc -Directory -Filter 'boost-*' -ErrorAction SilentlyContinue | ForEach-Object { $cands.Add($_.FullName) } }
	}

	$best = $null; $bestV = $null
	foreach ($c in $cands)
	{
		if (-not (Test-BoostDir $c)) { continue }
		$v = Get-BoostVersion $c
		if ($v -and $v -ge [version]'1.70' -and (-not $bestV -or $v -gt $bestV)) { $best = $c; $bestV = $v }
	}
	if ($best)
	{
		Set-Junction 'boost' $best | Out-Null
		Write-Ok "Boost $bestV found: $best"
		Add-Report 'boost' "$bestV" 'local' $best
		return
	}

	$u = $BoostDownloadVersion -replace '\.', '_'
	$arc = Get-Download @("https://archives.boost.io/release/$BoostDownloadVersion/source/boost_$u.7z") "boost_$u.7z"
	$dir = Reset-ComponentFolder 'boost'
	Write-Step 'Extract the Boost headers'
	& $SevenZipExe x $arc "-o$Work" "boost_$u\boost\*" -y | Out-Null
	if ($LASTEXITCODE -ne 0) { throw 'Cannot extract the Boost archive.' }
	Move-Item -LiteralPath (Join-Path $Work "boost_$u\boost") -Destination $dir
	Remove-Item (Join-Path $Work "boost_$u") -Recurse -Force
	Set-Marker $dir "download boost $BoostDownloadVersion"
	Write-Ok "Boost $BoostDownloadVersion headers are in cots\boost"
	Add-Report 'boost' $BoostDownloadVersion 'download' $dir
}

# ---------------------------------------------------------------------------
#  Xerces-C (3.2 or newer, shared library). Used for all XML files.
# ---------------------------------------------------------------------------
function Get-XercesVersion([string]$root)
{
	$f = Join-Path $root 'include\xercesc\util\XercesVersion.hpp'
	if (-not (Test-Path $f)) { return $null }
	$t = Get-Content -LiteralPath $f -Raw
	$a = [regex]::Match($t, '(?m)^#define XERCES_VERSION_MAJOR (\d+)').Groups[1].Value
	$b = [regex]::Match($t, '(?m)^#define XERCES_VERSION_MINOR (\d+)').Groups[1].Value
	$c = [regex]::Match($t, '(?m)^#define XERCES_VERSION_REVISION (\d+)').Groups[1].Value
	if (-not $a) { return $null }
	return [version]"$a.$b.$c"
}

function Test-XercesDir([string]$root)
{
	if (-not (Test-Path (Join-Path $root 'include\xercesc\util\XercesVersion.hpp'))) { return $false }
	if (-not (Test-Path (Join-Path $root 'lib\xerces-c_3.lib'))) { return $false }
	$dll = Get-ChildItem (Join-Path $root 'bin') -Filter 'xerces-c_3_*.dll' -ErrorAction SilentlyContinue | Select-Object -First 1
	return [bool]$dll
}

function Install-Xerces($vs)
{
	Write-Step 'Xerces-C (3.2 or newer)'
	$own = Join-Path $Cots 'xerces-c'
	if ((Use-Existing $own) -and (Test-XercesDir $own)) { $v = Get-XercesVersion $own; Write-Ok "cots\xerces-c is ready ($v)"; Add-Report 'xerces-c' "$v" 'cots' $own; return }

	# Local installations with the full layout (include + lib + bin)
	$cands = New-Object System.Collections.Generic.List[string]
	foreach ($e in 'XERCESC_ROOT', 'XERCES_ROOT', 'XercesC_ROOT') { $val = [Environment]::GetEnvironmentVariable($e); if ($val) { $cands.Add($val) } }
	foreach ($r in 'C:\Program Files\xerces-c', 'C:\xerces-c', 'C:\OSGeo4W', 'C:\OSGeo4W64', "$env:USERPROFILE\vcpkg\installed\x64-windows", 'C:\vcpkg\installed\x64-windows', 'C:\src\vcpkg\installed\x64-windows') { $cands.Add($r) }
	foreach ($c in $cands)
	{
		if (-not (Test-XercesDir $c)) { continue }
		$v = Get-XercesVersion $c
		if (-not $v -or $v -lt [version]'3.2') { continue }
		$dir = Reset-ComponentFolder 'xerces-c'
		Copy-Item -LiteralPath (Join-Path $c 'include\xercesc') -Destination (Join-Path $dir 'include\xercesc') -Recurse
		New-Item -ItemType Directory -Force (Join-Path $dir 'lib'), (Join-Path $dir 'bin') | Out-Null
		Copy-Item (Join-Path $c 'lib\xerces-c_3*.lib') (Join-Path $dir 'lib')
		Copy-Item (Join-Path $c 'bin\xerces-c_3_*.dll') (Join-Path $dir 'bin')
		# vcpkg keeps the debug files in a "debug" folder
		if (Test-Path (Join-Path $c 'debug\lib')) { Copy-Item (Join-Path $c 'debug\lib\xerces-c_3*D.lib') (Join-Path $dir 'lib') -ErrorAction SilentlyContinue }
		if (Test-Path (Join-Path $c 'debug\bin')) { Copy-Item (Join-Path $c 'debug\bin\xerces-c_3_*D.dll') (Join-Path $dir 'bin') -ErrorAction SilentlyContinue }
		Set-Marker $dir "copy $c"
		Write-Ok "Xerces-C $v copied from $c"
		Add-Report 'xerces-c' "$v" 'local' $c
		return
	}

	# Download the source code and compile it (Release + Debug) with MSVC.
	$name = "xerces-c-$XercesDownloadVersion"
	$zip = Get-Download @("https://dlcdn.apache.org/xerces/c/3/sources/$name.zip", "https://archive.apache.org/dist/xerces/c/3/sources/$name.zip") "$name.zip"
	Assert-Sha256 $zip @("https://downloads.apache.org/xerces/c/3/sources/$name.zip.sha256", "https://archive.apache.org/dist/xerces/c/3/sources/$name.zip.sha256")

	$srcRoot = Join-Path $Work 'xerces_src'
	$bld = Join-Path $Work 'xerces_build'
	foreach ($d in $srcRoot, $bld) { if (Test-Path $d) { Remove-Item $d -Recurse -Force } }
	Write-Step "Extract $name.zip"
	Expand-Archive -LiteralPath $zip -DestinationPath $srcRoot
	$src = Join-Path $srcRoot $name
	$dir = Reset-ComponentFolder 'xerces-c'
	$cmake = Join-Path $Cots 'cmake\bin\cmake.exe'

	Write-Step "Configure Xerces-C ($($vs.Generator))"
	& $cmake -S $src -B $bld -G $vs.Generator -A x64 "-DCMAKE_INSTALL_PREFIX=$dir" -DBUILD_SHARED_LIBS=ON -Dnetwork=OFF -Dtranscoder=windows -Dmessage-loader=inmemory -Dxmlch-type=char16_t
	if ($LASTEXITCODE -ne 0) { throw 'Xerces-C configure failed.' }
	foreach ($cfg in 'Release', 'Debug')
	{
		Write-Step "Compile and install Xerces-C ($cfg). This can take some minutes."
		& $cmake --build $bld --config $cfg --target install --parallel
		if ($LASTEXITCODE -ne 0) { throw "Xerces-C $cfg build failed." }
	}
	Set-Marker $dir "build $name from $(Split-Path $zip -Leaf)"
	Remove-Item $srcRoot, $bld -Recurse -Force
	if (-not (Test-XercesDir $dir)) { throw 'The Xerces-C install is not complete.' }
	Write-Ok "Xerces-C $XercesDownloadVersion is compiled and installed in cots\xerces-c"
	Add-Report 'xerces-c' $XercesDownloadVersion 'build' $dir
}

# ---------------------------------------------------------------------------
#  OpenGL - check only (a part of Windows and of the GPU driver)
# ---------------------------------------------------------------------------
function Test-OpenGL
{
	Write-Step 'OpenGL'
	$dll = Join-Path $env:WINDIR 'System32\opengl32.dll'
	if (-not (Test-Path $dll)) { throw 'opengl32.dll is not on this computer.' }
	$gpus = @()
	try { $gpus = @(Get-CimInstance Win32_VideoController | ForEach-Object { "$($_.Name) (driver $($_.DriverVersion))" }) } catch { }
	foreach ($g in $gpus) { Write-Ok "GPU: $g" }
	Add-Report 'opengl' 'system' 'local' ($gpus -join '; ')
}

# ---------------------------------------------------------------------------
#  Inno Setup (optional, for the setup file of the release package)
# ---------------------------------------------------------------------------
function Install-InnoSetup
{
	Write-Step 'Inno Setup (optional)'
	$cands = @(
		(Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 7'),
		(Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6'),
		'C:\Program Files (x86)\Inno Setup 7',
		'C:\Program Files (x86)\Inno Setup 6',
		'C:\Program Files\Inno Setup 7',
		'C:\Program Files\Inno Setup 6')
	foreach ($c in $cands)
	{
		if (Test-Path (Join-Path $c 'ISCC.exe'))
		{
			Set-Junction 'innosetup' $c | Out-Null
			$v = Get-ExeVersion (Join-Path $c 'ISCC.exe')
			Write-Ok "Inno Setup $v found: $c"
			Add-Report 'innosetup' $v 'local' $c
			return $true
		}
	}
	Write-Note 'Inno Setup is not on this computer. The deploy script does not make a setup file.'
	Add-Report 'innosetup' '-' 'missing' '-'
	return $false
}

# ---------------------------------------------------------------------------
#  Main
# ---------------------------------------------------------------------------
try
{
	Write-Host '============================================================'
	Write-Host ' Traffic Engine - third-party components (cots)'
	Write-Host " Folder: $Cots"
	Write-Host '============================================================'

	$vs = Find-VisualStudio
	Install-CMake $vs
	Install-Qt
	Install-SevenZip
	Install-Boost
	Install-Xerces $vs
	Test-OpenGL
	$hasInno = Install-InnoSetup

	$iscc = ''
	if ($hasInno) { $iscc = '%TE_COTS%\innosetup\ISCC.exe' }
	$envLines = @(
		'@echo off',
		'rem Generated by build\setup_cots.ps1. Do not edit. Run build\setup_cots.bat to make it again.',
		"set `"TE_COTS=$Cots`"",
		'set "TE_CMAKE=%TE_COTS%\cmake\bin\cmake.exe"',
		'set "TE_QT_DIR=%TE_COTS%\qt5"',
		"set `"TE_7Z=$SevenZipExe`"",
		"set `"TE_GENERATOR=$($vs.Generator)`"",
		"set `"TE_VS_DIR=$($vs.Path)`"",
		"set `"TE_VC_CRT=$($vs.Crt)`"",
		"set `"TE_ISCC=$iscc`""
	)
	Set-Content -LiteralPath (Join-Path $Cots 'cots_env.bat') -Value $envLines -Encoding ASCII

	$head = @(
		"Traffic Engine cots report - $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')",
		('{0,-11} {1,-12} {2,-10} {3}' -f 'component', 'version', 'source', 'path'),
		('-' * 78))
	Set-Content -LiteralPath (Join-Path $Cots 'cots_report.txt') -Value ($head + $Report) -Encoding ASCII

	Write-Host ''
	$head + $Report | ForEach-Object { Write-Host $_ }
	Write-Host ''
	Write-Ok 'All components are ready in the cots folder.'
	exit 0
}
catch
{
	Write-Host ''
	Write-Host "[ERROR] $($_.Exception.Message)" -ForegroundColor Red
	exit 1
}
