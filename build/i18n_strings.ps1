# =============================================================================
#  i18n_strings.ps1 - The English user interface texts of the source files.
#
#  Use:
#    powershell -ExecutionPolicy Bypass -File build\i18n_strings.ps1            all texts
#    powershell -ExecutionPolicy Bypass -File build\i18n_strings.ps1 -Missing   only the texts
#                                                    that data\lang\tr.xml does not translate
#
#  The script reads the string literals of the user interface files and keeps
#  the literals that look like text for the user (a space, a capital letter or
#  an "&" for a menu key). Configuration keys, names of XML elements and other
#  identifiers are not in the list. Some found texts are not for the user (for
#  example log lines): do not translate them.
# =============================================================================
param(
	[switch]$Missing
)

$root = Split-Path -Parent $PSScriptRoot
$files = @(
	'main_window.cpp', 'world_view.cpp', 'world_properties.cpp', 'property_panel.cpp', 'settings_dialog.cpp',
	'osm_import_dialog.cpp', 'osm_area_map.cpp', 'osm_import.cpp', 'traffic_light_dialog.cpp',
	'traffic_density_widget.cpp', 'chart_dock.cpp', 'chart_widget.cpp', 'stats_collector.cpp',
	'sign_palette.cpp', 'vehicle_types.cpp', 'workspace.cpp', 'sign_catalog.cpp', 'te_file.cpp', 'xml_doc.cpp', 'http_client.cpp', 'demo_world.cpp', 'congestion_dock.cpp', 'splash_screen.cpp', 'sim_congestion.cpp'
)

# Lines that are not user interface text: log calls and file names
$skip_line = 'TE_(TRACE|DEBUG|INFO|WARN|ERROR)\(|logger::instance\(\)\.|setObjectName|value\(QStringLiteral\("|_value\(QStringLiteral\("|set_(value|bool|int|double)\(QStringLiteral\("|attribute|QFont\(|setStyleSheet|\.png|\.svg|\.csv|\.json|property\('

$found = [System.Collections.Generic.List[string]]::new()
$literal = '"((?:[^"\\]|\\.)*)"'
foreach ($name in $files)
{
	$path = Join-Path $root "src\$name"
	if (-not (Test-Path $path)) { continue }
	$text = [IO.File]::ReadAllText($path, [Text.Encoding]::UTF8)
	# Adjacent literals ("abc " "def", also on two lines) are one text for the compiler.
	foreach ($m in [regex]::Matches($text, "(?:$literal\s*)+"))
	{
		$line_start = $text.LastIndexOf("`n", [Math]::Max(0, $m.Index - 1)) + 1
		$line_end = $text.IndexOf("`n", $m.Index)
		if ($line_end -lt 0) { $line_end = $text.Length }
		$line = $text.Substring($line_start, $line_end - $line_start)
		if ($line -match $skip_line -or $line.TrimStart().StartsWith('#include')) { continue }
		$s = (([regex]::Matches($m.Value, $literal) | ForEach-Object { $_.Groups[1].Value }) -join '')
		$s = $s -replace '\\n', "`n" -replace '\\"', '"' -replace '\\\\', '\'
		if ($s.Length -lt 2) { continue }
		if ($s -cmatch '^[a-z0-9_./:\-]+$') { continue }            # identifiers, keys, file names
		if ($s -match '^[%0-9\s.,:;x+\-()]+$') { continue }         # only numbers and placeholders
		if ($s -match '^#[0-9A-Fa-f]{6}$') { continue }             # colors
		if ($s -notmatch '[A-Za-zÇĞİÖŞÜçğıöşü]') { continue }
		if ($s -notmatch '[ &]' -and $s -cnotmatch '^[A-Z]') { continue }
		if (-not $found.Contains($s)) { $found.Add($s) }
	}
}

if ($Missing)
{
	$xml = Join-Path $root 'data\lang\tr.xml'
	$known = @{}
	if (Test-Path $xml)
	{
		[xml]$doc = Get-Content $xml -Encoding UTF8
		foreach ($e in $doc.language.entry) { $known[$e.en] = $true }
	}
	$found = $found | Where-Object { -not $known.ContainsKey($_) }
}
$found | ForEach-Object { $_ -replace "`n", '\n' }
