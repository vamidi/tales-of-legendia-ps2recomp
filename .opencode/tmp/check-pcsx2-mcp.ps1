# Verifies two things after wiring the PCSX2 MCP server:
#  1. the global opencode config is still valid JSONC (comments + trailing commas) and has the pcsx2 entry
#  2. the Node bridge actually speaks MCP on stdio and advertises its tools
param(
    [string]$ConfigPath = "$env:USERPROFILE\.config\opencode\opencode.jsonc",
    [string]$ServerPath = 'C:\PCSX2-MCP-v1.0.0\pcsx2-mcp-server\dist\index.js'
)

$ErrorActionPreference = 'Stop'

function Remove-JsoncNoise([string]$src) {
    $sb = [System.Text.StringBuilder]::new()
    $i = 0
    $n = $src.Length
    $quote = [char]34
    $backslash = [char]92
    $lf = [char]10
    while ($i -lt $n) {
        $c = $src[$i]
        if ($c -eq $quote) {
            [void]$sb.Append($c); $i++
            while ($i -lt $n) {
                $d = $src[$i]
                [void]$sb.Append($d); $i++
                if ($d -eq $backslash -and $i -lt $n) { [void]$sb.Append($src[$i]); $i++; continue }
                if ($d -eq $quote) { break }
            }
            continue
        }
        if ($c -eq '/' -and $i + 1 -lt $n -and $src[$i + 1] -eq '/') {
            while ($i -lt $n -and $src[$i] -ne $lf) { $i++ }
            continue
        }
        if ($c -eq '/' -and $i + 1 -lt $n -and $src[$i + 1] -eq '*') {
            $i += 2
            while ($i + 1 -lt $n -and -not ($src[$i] -eq '*' -and $src[$i + 1] -eq '/')) { $i++ }
            $i += 2
            continue
        }
        [void]$sb.Append($c)
        $i++
    }
    return ($sb.ToString() -replace ',(\s*[}\]])', '$1')
}

'=== 1. config ==='
$clean = Remove-JsoncNoise (Get-Content -Raw -LiteralPath $ConfigPath)
try {
    $cfg = $clean | ConvertFrom-Json
} catch {
    "JSONC INVALID: $($_.Exception.Message)"
    exit 1
}
"JSONC OK - top-level keys: " + (($cfg.PSObject.Properties.Name) -join ', ')
$servers = $cfg.mcp.servers.PSObject.Properties.Name
"mcp.servers: " + ($servers -join ', ')
if (-not ($servers -contains 'pcsx2')) { 'pcsx2 ENTRY MISSING'; exit 1 }
"pcsx2 command: " + ($cfg.mcp.servers.pcsx2.command -join ' ')
"pcsx2 disabled: $($cfg.mcp.servers.pcsx2.disabled)  codemode: $($cfg.mcp.servers.pcsx2.codemode)"

'=== 2. bridge handshake (stdio MCP) ==='
if (-not (Test-Path -LiteralPath $ServerPath)) { "MISSING: $ServerPath"; exit 1 }
$init = @{ jsonrpc = '2.0'; id = 1; method = 'initialize'; params = @{
        protocolVersion = '2025-06-18'; capabilities = @{}
        clientInfo = @{ name = 'opencode-smoke'; version = '0.0.0' } } } | ConvertTo-Json -Depth 8 -Compress
$ready = @{ jsonrpc = '2.0'; method = 'notifications/initialized' } | ConvertTo-Json -Compress
$list = @{ jsonrpc = '2.0'; id = 2; method = 'tools/list'; params = @{} } | ConvertTo-Json -Compress

$job = Start-Job -ScriptBlock {
    param($server, $lines)
    $lines | & node $server 2>$null
} -ArgumentList $ServerPath, @($init, $ready, $list)
if (-not (Wait-Job $job -Timeout 45)) { Stop-Job $job; 'BRIDGE TIMEOUT (no answer in 45s)'; exit 1 }
$raw = Receive-Job $job
Remove-Job $job -Force

$tools = @()
foreach ($line in $raw) {
    if ($line -match '"tools"') {
        try {
            $msg = $line | ConvertFrom-Json
            if ($msg.result -and $msg.result.tools) { $tools = $msg.result.tools }
        } catch { }
    }
}
if ($tools.Count -eq 0) {
    'NO TOOLS RETURNED. Raw output:'
    $raw | Select-Object -First 5
    exit 1
}
"bridge OK - $($tools.Count) tools:"
($tools.name | Sort-Object) -join ', '
