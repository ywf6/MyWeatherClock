$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$out = Join-Path $PSScriptRoot 'build'
$device = Join-Path $root 'upstream/firmware/cmsis/device'
$cmsis = Join-Path $root 'upstream/firmware/cmsis'
$tools = if ($env:ARMCC_BIN) { $env:ARMCC_BIN } else { 'D:/Develop/Keil/core/ARM/ARMCC/bin' }
New-Item -ItemType Directory -Path $out -Force | Out-Null

function Invoke-ArmTool([string]$name, [string[]]$arguments) {
    & (Join-Path $tools $name) @arguments
    if ($LASTEXITCODE -ne 0) { throw "$name failed ($LASTEXITCODE)" }
}

$startup = Join-Path $out 'startup.o'
$jump = Join-Path $out 'jump.o'
$boot = Join-Path $out 'boot.o'
$axf = Join-Path $out 'boot.axf'
$binary = Join-Path $out 'boot.bin'
Invoke-ArmTool 'armasm.exe' @('--cpu=Cortex-M4.fp', '--pd', '__MICROLIB SETA 1', (Join-Path $device 'startup_stm32f40_41xxx.s'), '-o', $startup)
Invoke-ArmTool 'armasm.exe' @('--cpu=Cortex-M4.fp', (Join-Path $PSScriptRoot 'jump.s'), '-o', $jump)
Invoke-ArmTool 'armcc.exe' @('--cpu=Cortex-M4.fp', '--c99', '--library_type=microlib', '--split_sections', '-O2', '-DSTM32F40_41xxx',
    "-I$device", "-I$cmsis", "-I$cmsis/core", '-c', (Join-Path $PSScriptRoot 'boot.c'), '-o', $boot)
Invoke-ArmTool 'armlink.exe' @('--cpu=Cortex-M4.fp', '--library_type=microlib', '--scatter', (Join-Path $PSScriptRoot 'boot.sct'),
    '--map', '--list', (Join-Path $out 'boot.map'), '--output', $axf, $startup, $jump, $boot)
Invoke-ArmTool 'fromelf.exe' @('--bin', '--output', $binary, $axf)
& (Join-Path $root 'build-page-flow.ps1') -Ota -OutputDirectory 'ota/build/app'
& python -B (Join-Path $PSScriptRoot 'package.py') $out
if ($LASTEXITCODE -ne 0) { throw 'OTA package generation failed' }
