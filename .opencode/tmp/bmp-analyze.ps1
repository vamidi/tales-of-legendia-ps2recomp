# Summarises a 24-bit BMP framebuffer dump: non-black coverage, bounding box, top colours, the rows that
# contain pixels, and a coarse brightness map. Used to tell "nothing drawn" apart from "drawn in black".
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [int]$GridW = 64,
    [int]$GridH = 28,
    [int]$Threshold = 12
)

$bytes = [System.IO.File]::ReadAllBytes($Path)
$off = [BitConverter]::ToInt32($bytes, 10)
$w = [BitConverter]::ToInt32($bytes, 18)
$h = [BitConverter]::ToInt32($bytes, 22)
$bpp = [BitConverter]::ToInt16($bytes, 28)
if ($bpp -ne 24) { "only 24-bit BMP supported (this is $bpp)"; exit 1 }

$rowBytes = [int]([Math]::Floor((($w * $bpp / 8) + 3) / 4) * 4)
$grid = @{}                                   # "gy,gx" -> summed luminance (a hashtable: PS array indexing bit us)
$rows = [int[]]::new($h)
$colours = @{}
$nz = 0
$minX = $w; $maxX = -1; $minY = $h; $maxY = -1

for ($y = 0; $y -lt $h; $y++) {
    $srcY = $h - 1 - $y                      # BMP rows are bottom-up
    $row = $off + $srcY * $rowBytes
    for ($x = 0; $x -lt $w; $x++) {
        $p = $row + $x * $3
        $bl = [int]$bytes[$p]; $g = [int]$bytes[$p + 1]; $r = [int]$bytes[$p + 2]
        $lum = $r + $g + $bl
        $gx = [int]([double]$x * $GridW / $w)
        $gy = [int]([double]$y * $GridH / $h)
        $key = '{0},{1}' -f $gy, $gx
        if ($grid.ContainsKey($key)) { $grid[$key] += $lum } else { $grid[$key] = $lum }
        if ($lum -gt $Threshold) {
            $nz++
            $rows[$y]++
            if ($x -lt $minX) { $minX = $x }
            if ($x -gt $maxX) { $maxX = $x }
            if ($y -lt $minY) { $minY = $y }
            if ($y -gt $maxY) { $maxY = $y }
            $ck = '{0:x2}{1:x2}{2:x2}' -f $r, $g, $bl
            if ($colours.ContainsKey($ck)) { $colours[$ck]++ } else { $colours[$ck] = 1 }
        }
    }
}

"{0}: {1}x{2} nonBlack={3} ({4:n2}%) bbox=({5},{6})-({7},{8})" -f `
    (Split-Path $Path -Leaf), $w, $h, $nz, (100.0 * $nz / ($w * $h)), $minX, $minY, $maxX, $maxY
$top = $colours.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 6
"top colours: " + (($top | ForEach-Object { '#{0}x{1}' -f $_.Key, $_.Value }) -join ' ')

$bands = @()
for ($y = 0; $y -lt $h; $y++) {
    if ($rows[$y] -gt 0) {
        $bands += ('{0}:{1}' -f $y, $rows[$y])
    }
}
"rows with pixels ({0}): {1}" -f $bands.Count, (($bands | Select-Object -First 14) -join ' ')

for ($gy = 0; $gy -lt $GridH; $gy++) {
    $line = ''
    for ($gx = 0; $gx -lt $GridW; $gx++) {
        $key = '{0},{1}' -f $gy, $gx
        $v = if ($grid.ContainsKey($key)) { $grid[$key] } else { 0 }
        $line += $(if ($v -eq 0) { '.' } elseif ($v -lt 300) { '-' } elseif ($v -lt 1200) { '+' } elseif ($v -lt 4000) { '*' } else { '#' })
    }
    $line
}
