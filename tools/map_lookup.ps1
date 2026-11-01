<#
.SYNOPSIS
    Look up which function in the recompiler's map covers one or more guest addresses.

.DESCRIPTION
    config/map.csv has the header Name,Start,End,Size, and addresses are written as "0x00100008"
    ([Convert]::ToInt64(string, 16) rejects that prefix, so it is stripped first).

    Rows may nest; by default the innermost covering row is shown, -All shows every match.

    Address parsing: "0x13F660" is hex, "13F660" is hex (a bare token with A-F cannot be decimal),
    "12550240" is decimal.

.EXAMPLE
    .\tools\map_lookup.ps1 0x13F660
    .\tools\map_lookup.ps1 0x109144,0x1450A0,0x165AD0
    .\tools\map_lookup.ps1 13F660 -All

.NOTES
    Script execution may be disabled on this machine; run it as
    powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\map_lookup.ps1 0x13F660
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]] $Address,

    [string] $Map,

    [switch] $All
)

$ErrorActionPreference = 'Stop'

# Under -File a comma-joined argument arrives as a single string, so split it here as well.
$targets = @($Address | ForEach-Object { $_ -split ',' } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
if ($targets.Count -eq 0) {
    Write-Error 'give at least one address, e.g. .\tools\map_lookup.ps1 0x13F660'
    exit 2
}

if (-not $Map) {
    $Map = Join-Path (Split-Path -Parent $PSScriptRoot) 'config\map.csv'
}
if (-not (Test-Path -LiteralPath $Map)) {
    Write-Error "map not found: $Map"
    exit 2
}

function ConvertFrom-MapAddress([string] $text) {
    $trimmed = $text.Trim()
    if ($trimmed.StartsWith('0x') -or $trimmed.StartsWith('0X')) {
        $trimmed = $trimmed.Substring(2)
    }
    return [Convert]::ToInt64($trimmed, 16)
}

function ConvertFrom-UserAddress([string] $text) {
    $trimmed = $text.Trim()
    if ($trimmed -match '^0[xX]') {
        return [Convert]::ToInt64($trimmed.Substring(2), 16)
    }
    if ($trimmed -match '[A-Fa-f]') {
        return [Convert]::ToInt64($trimmed, 16)
    }
    return [Convert]::ToInt64($trimmed, 10)
}

$entries = @(
    Import-Csv -LiteralPath $Map | ForEach-Object {
        [pscustomobject] @{
            Name  = $_.Name
            Start = ConvertFrom-MapAddress $_.Start
            End   = ConvertFrom-MapAddress $_.End
            Size  = $_.Size
        }
    }
) | Sort-Object Start

foreach ($token in $targets) {
    $value = ConvertFrom-UserAddress $token
    $hits = @($entries | Where-Object { $_.Start -le $value -and $value -lt $_.End })

    if ($hits.Count -eq 0) {
        '0x{0:X8} -> no row covers this address (hole in the map)' -f $value
        continue
    }

    if (-not $All -and $hits.Count -gt 1) {
        # Innermost = highest start.
        $hits = @($hits | Sort-Object Start -Descending | Select-Object -First 1)
    }

    foreach ($hit in $hits) {
        $offset = $value - $hit.Start
        $kind = if ($offset -eq 0) { 'start' } else { 'interior +0x{0:X}' -f $offset }
        '0x{0:X8} -> {1} [{2}..{3}) size={4} ({5})' -f $value, $hit.Name, $hit.Start, $hit.End, $hit.Size, $kind
    }
}
