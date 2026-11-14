param([string]$OutputDirectory = 'page-flow-build', [switch]$Ota)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$mdk = Join-Path $root 'mdk'
$out = Join-Path $PSScriptRoot $OutputDirectory
$bin = if ($env:ARMCC_BIN) { $env:ARMCC_BIN } else { 'D:\Develop\Keil\core\ARM\ARMCC\bin' }
New-Item -ItemType Directory -Path $out -Force | Out-Null
[xml]$project = Get-Content -LiteralPath (Join-Path $mdk 'stm32f407.uvprojx') -Raw
$target = $project.Project.Targets.Target
$controls = $target.TargetOption.TargetArmAds.Cads.VariousControls
$includeFlags = @($controls.IncludePath.Split(';') | ForEach-Object {
    '-I' + [IO.Path]::GetFullPath((Join-Path $mdk $_))
})
$defineFlags = @($controls.Define.Split(',') | ForEach-Object { '-D' + $_ })
if ($Ota) { $defineFlags += '-DWC_OTA_APP' }

function Invoke-ArmTool([string]$name, [string[]]$toolArgs) {
    & (Join-Path $bin $name) @toolArgs
    if ($LASTEXITCODE -ne 0) { throw "$name failed (exit $LASTEXITCODE)" }
}

$objects = @()
foreach ($group in $target.Groups.Group) {
    foreach ($file in $group.Files.File) {
        if ($file.FileType -notin @('1', '2')) { continue }
        $source = [IO.Path]::GetFullPath((Join-Path $mdk $file.FilePath))
        $obj = Join-Path $out ([IO.Path]::GetFileNameWithoutExtension($source) + '.o')
        if ($Ota -and $file.FileName -eq 'system_stm32f4xx.c') {
            $system = [Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes($source))
            $system = $system.Replace('#define VECT_TAB_OFFSET  0x00', '#define VECT_TAB_OFFSET  0x10000')
            if (!$system.Contains('#define VECT_TAB_OFFSET  0x10000')) { throw 'Could not relocate VTOR' }
            $source = Join-Path $out 'system_stm32f4xx.c'
            [IO.File]::WriteAllBytes($source, [Text.Encoding]::GetEncoding(28591).GetBytes($system))
        }
        if ($file.FileType -eq '1') {
            Invoke-ArmTool 'armcc.exe' (@('--cpu=Cortex-M4.fp', '--c99', '--gnu', '--library_type=microlib',
                '--split_sections', '-O1', '-g', '-D__MICROLIB') + $defineFlags + $includeFlags +
                @('-c', $source, '-o', $obj))
        } else {
            Invoke-ArmTool 'armasm.exe' @('--cpu=Cortex-M4.fp', '--pd', '__MICROLIB SETA 1', $source, '-o', $obj)
        }
        $objects += $obj
    }
}
$scatter = Join-Path $out 'stm32f407_flash.sct'
$layout = @'
LR_FLASH 0x08000000 0x00080000
{
    ER_FLASH 0x08000000 0x00080000
    {
        startup_stm32f40_41xxx.o (RESET, +First)
        *(InRoot$$Sections)
        .ANY (+RO)
    }
    RW_RAM 0x20000000 0x00020000
    {
        .ANY (+RW +ZI)
    }
}
'@
if ($Ota) {
    $layout = $layout.Replace('0x08000000 0x00080000', '0x08010000 0x00070000').Replace('0x00020000', '0x0001FFF8')
}
$layout | Set-Content -LiteralPath $scatter -Encoding ascii
$axf = Join-Path $out 'weatherclock-page-flow.axf'
Invoke-ArmTool 'armlink.exe' (@('--cpu=Cortex-M4.fp', '--library_type=microlib',
    '--scatter', $scatter, '--map', '--info=sizes', '--list', (Join-Path $out 'weatherclock-page-flow.map'),
    '--output', $axf) + $objects)
Invoke-ArmTool 'fromelf.exe' @('--i32combined', '--output', (Join-Path $out 'weatherclock-page-flow.hex'), $axf)
if ($Ota) {
    Invoke-ArmTool 'fromelf.exe' @('--bin', '--output', (Join-Path $out 'weatherclock-page-flow.bin'), $axf)
}
Write-Output "Built $axf"
