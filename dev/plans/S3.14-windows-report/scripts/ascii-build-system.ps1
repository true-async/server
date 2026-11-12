# Drop non-ASCII bytes from the PHP_BUILD_SYSTEM line of config.w32.h (a localized
# WMI OS caption in the ANSI code page breaks cl /utf-8 under /WX with C4828).
param([string]$path)
$ansi = [Text.Encoding]::GetEncoding(1251)
$lines = $ansi.GetString([IO.File]::ReadAllBytes($path)) -split "`n"
$lines = $lines | ForEach-Object {
    if ($_ -like '#define PHP_BUILD_SYSTEM *') { ($_ -replace '[^\x00-\x7F]', '') -replace '" +', '"' } else { $_ }
}
[IO.File]::WriteAllText($path, ($lines -join "`n"), [Text.Encoding]::ASCII)
Select-String -Path $path -Pattern 'PHP_BUILD_SYSTEM' | ForEach-Object Line
