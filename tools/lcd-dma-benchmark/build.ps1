param(
    [string]$SourceRoot = (Join-Path $PSScriptRoot '..\..'),
    [switch]$SlowSpi
)

$ErrorActionPreference = 'Stop'
$stm = Join-Path (Resolve-Path -LiteralPath $SourceRoot).Path 'stm32'
$benchVariant = if ($SlowSpi) { 'build-slow' } else { 'build' }
$output = Join-Path $PSScriptRoot $benchVariant
$compiler = if ($env:ARMCC_BIN) { $env:ARMCC_BIN } else { 'D:\Develop\Keil\core\ARM\ARMCC\bin' }
New-Item -ItemType Directory -Path $output -Force | Out-Null

function Invoke-ArmTool([string]$Name, [string[]]$Arguments) {
    & (Join-Path $compiler $Name) @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Name failed ($LASTEXITCODE)" }
}

function Replace-Once([string]$Content, [string]$Pattern, [string]$Replacement) {
    $regex = [regex]::new($Pattern)
    if ($regex.Matches($Content).Count -ne 1) {
        throw "Expected exactly one source match: $Pattern"
    }
    return $regex.Replace($Content, { param($match) $Replacement }, 1)
}

$latin1 = [Text.Encoding]::GetEncoding(28591)
$newline = [Environment]::NewLine
$systemSource = Join-Path $stm 'firmware/cmsis/device/system_stm32f4xx.c'
$system = [IO.File]::ReadAllText($systemSource, $latin1)
$system = Replace-Once $system '(?m)^/\* #define VECT_TAB_SRAM \*/' '#define VECT_TAB_SRAM'
[IO.File]::WriteAllText((Join-Path $output 'system_stm32f4xx.c'), $system, $latin1)

$configSource = Join-Path $stm 'third_lib/freertos/portable/FreeRTOSConfig.h'
$config = [IO.File]::ReadAllText($configSource, $latin1)
$config = Replace-Once $config '(?m)^(#define configTOTAL_HEAP_SIZE)[^\r\n]+' '#define configTOTAL_HEAP_SIZE (12 * 1024)'
$config = @'
void lcd_bench_task_switched(const void *task);
#define traceTASK_SWITCHED_IN() lcd_bench_task_switched(pxCurrentTCB)
'@ + $newline + $config
[IO.File]::WriteAllText((Join-Path $output 'FreeRTOSConfig.h'), $config, $latin1)

$lcdSource = Join-Path $stm 'driver/st7789/st7789.c'
$lcd = [IO.File]::ReadAllText($lcdSource, $latin1)
$lcd = @'
#include <stdbool.h>
bool lcd_bench_is_polling(void);
void lcd_bench_dma_irq(bool entering);
'@ + $newline + $lcd
# Both modes share the real driver's address-window and chip-select paths.
$pollingBranch = @'
    if (lcd_bench_is_polling())
    {
        const uint16_t *pixels = (const uint16_t *)data;
        const uint16_t fill = *pixels;
        while (length > 0)
        {
            while ((SPI2->SR & SPI_SR_TXE) == 0) {}
            SPI2->DR = singlecolor ? fill : *pixels++;
            --length;
        }
    }
    else do
'@
$lcd = Replace-Once $lcd '(?m)^    do(?=\r?$)' $pollingBranch
$traceEntry = 'BaseType_t pxHigherPriorityTaskWoken = pdFALSE;' + $newline + '        lcd_bench_dma_irq(true);'
$lcd = Replace-Once $lcd 'BaseType_t pxHigherPriorityTaskWoken;' $traceEntry
$clearPending = 'DMA_ClearITPendingBit(DMA1_Stream4, DMA_IT_TCIF4);'
$traceExit = $clearPending + $newline + '        lcd_bench_dma_irq(false);'
$lcd = Replace-Once $lcd ([regex]::Escape($clearPending)) $traceExit
if ($SlowSpi) {
    $lcd = Replace-Once $lcd 'SPI_BaudRatePrescaler_4;' 'SPI_BaudRatePrescaler_8;'
}
[IO.File]::WriteAllText((Join-Path $output 'st7789.c'), $lcd, $latin1)

$includes = @($output) + @(
    'firmware/cmsis/core', 'firmware/cmsis/device', 'firmware/driver/inc',
    'app', 'app/font', 'app/image', 'driver/st7789', 'driver/tim_delay',
    'third_lib/freertos/include', 'third_lib/freertos/portable'
) | ForEach-Object {
    if ([IO.Path]::IsPathRooted($_)) { '-I' + $_ } else { '-I' + (Join-Path $stm $_) }
}
$compileFlags = @('--cpu=Cortex-M4.fp', '--c99', '--gnu', '--library_type=microlib',
    '--split_sections', '-O1', '-g', '-D__MICROLIB', '-DSTM32F40_41xxx',
    '-DUSE_STDPERIPH_DRIVER', '-DHSE_VALUE=8000000') + $includes

$sources = @(
    (Join-Path $PSScriptRoot 'benchmark.c'),
    (Join-Path $output 'system_stm32f4xx.c'),
    (Join-Path $output 'st7789.c')
) + @(
    'driver/tim_delay/tim_delay.c', 'firmware/driver/src/misc.c',
    'firmware/driver/src/stm32f4xx_dma.c', 'firmware/driver/src/stm32f4xx_gpio.c',
    'firmware/driver/src/stm32f4xx_rcc.c', 'firmware/driver/src/stm32f4xx_spi.c',
    'firmware/driver/src/stm32f4xx_tim.c', 'third_lib/freertos/list.c',
    'third_lib/freertos/queue.c', 'third_lib/freertos/tasks.c',
    'third_lib/freertos/timers.c', 'third_lib/freertos/portable/port.c',
    'third_lib/freertos/portable/heap_4.c'
) | ForEach-Object {
    if ([IO.Path]::IsPathRooted($_)) { $_ } else { Join-Path $stm $_ }
}

$startupObject = Join-Path $output 'startup.o'
$startup = Join-Path $stm 'firmware/cmsis/device/startup_stm32f40_41xxx.s'
Invoke-ArmTool 'armasm.exe' @('--cpu=Cortex-M4.fp', '--pd', '__MICROLIB SETA 1', $startup, '-o', $startupObject)
$objects = @($startupObject)
foreach ($source in $sources) {
    $obj = Join-Path $output ([IO.Path]::GetFileNameWithoutExtension($source) + '.o')
    Invoke-ArmTool 'armcc.exe' ($compileFlags + @('-c', $source, '-o', $obj))
    $objects += $obj
}

$scatter = Join-Path $output 'benchmark-ram.sct'
@'
LR_RAM 0x20000000 0x00020000
{
    ER_RAM 0x20000000
    {
        startup.o (RESET, +First)
        *(InRoot$$Sections)
        .ANY (+RO)
    }
    RW_RAM +0 ALIGN 8
    {
        .ANY (+RW +ZI)
    }
}
'@ | Set-Content -LiteralPath $scatter -Encoding ascii

$axf = Join-Path $output 'lcd-benchmark-ram.axf'
Invoke-ArmTool 'armlink.exe' (@('--cpu=Cortex-M4.fp', '--library_type=microlib',
    '--scatter', $scatter, '--map', '--info=sizes',
    '--list', (Join-Path $output 'lcd-benchmark-ram.map'), '--output', $axf) + $objects)
$binary = Join-Path $output 'lcd-benchmark-ram.bin'
Invoke-ArmTool 'fromelf.exe' @('--bin', '--output', $binary, $axf)
Write-Output "RAM image: $binary"

