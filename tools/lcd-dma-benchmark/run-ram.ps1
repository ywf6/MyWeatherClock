param(
    [Parameter(Mandatory = $true)][string]$OpenOcdRoot,
    [switch]$SlowSpi
)

$ErrorActionPreference = 'Stop'
$benchVariant = if ($SlowSpi) { 'build-slow' } else { 'build' }
$image = Join-Path $PSScriptRoot "$benchVariant\lcd-benchmark-ram.bin"
if (-not (Test-Path -LiteralPath $image -PathType Leaf)) { throw 'Build the selected RAM image with build.ps1 first' }

$binary = [IO.File]::ReadAllBytes($image)
$base = 0x20000000
$limit = 0x20020000
$debugWorkArea = 0x2001f000
if ($binary.Length -lt 8 -or $binary.Length -gt ($debugWorkArea - $base)) {
    throw 'Invalid RAM image size'
}
$stack = [BitConverter]::ToUInt32($binary, 0)
$reset = [BitConverter]::ToUInt32($binary, 4)
$entry = $reset -band 0xfffffffeL
if ($stack -le $base -or $stack -gt $limit -or ($stack -band 7) -ne 0 -or
    ($reset -band 1) -eq 0 -or $entry -lt ($base + 8) -or $entry -ge ($base + $binary.Length)) {
    throw 'Image vector table does not fit STM32F407 SRAM'
}

$imagePath = $image.Replace('\', '/')
if ($imagePath -match '[{}]') { throw 'Use a path without braces for OpenOCD' }
$startup = 'init; reset halt; load_image {{{0}}} 0x{1:x} bin; verify_image {{{0}}} 0x{1:x} bin; reg msp 0x{2:x}; reg xpsr 0x01000000; resume 0x{3:x}; shutdown' -f $imagePath, $base, $stack, $entry
$ocd = Join-Path $OpenOcdRoot 'bin/openocd.exe'
$scripts = Join-Path $OpenOcdRoot 'openocd/scripts'
if (-not (Test-Path -LiteralPath $scripts -PathType Container)) {
    $scripts = Join-Path $OpenOcdRoot 'share/openocd/scripts'
}
if (-not (Test-Path -LiteralPath $ocd -PathType Leaf) -or -not (Test-Path -LiteralPath $scripts -PathType Container)) {
    throw 'OpenOCD installation must contain bin/openocd.exe and its scripts directory'
}
# OpenOCD's checksum routine runs in SRAM. Keep it clear of the image and vectors.
$workArea = 'stm32f4x.cpu configure -work-area-phys 0x{0:x} -work-area-size 0x1000' -f $debugWorkArea
& $ocd -s $scripts -f interface/stlink.cfg -f target/stm32f4x.cfg -c 'adapter speed 1000' -c $workArea -c $startup
if ($LASTEXITCODE -ne 0) { throw "OpenOCD failed ($LASTEXITCODE)" }

