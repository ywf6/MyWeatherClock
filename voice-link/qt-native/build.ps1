param([string]$QtRoot = 'D:\Develop\Qt', [string]$CompilerRoot = 'C:\mingw64')
$ErrorActionPreference = 'Stop'
$qt = Join-Path $QtRoot '6.12.0\mingw_64'
$compiler = Join-Path $CompilerRoot 'bin'
$cmake = Join-Path $QtRoot 'Tools\CMake_64\bin\cmake.exe'
$ninja = Join-Path $QtRoot 'Tools\Ninja\ninja.exe'
$env:PATH = "$compiler;$qt\bin;$env:PATH"
& $cmake -S $PSScriptRoot -B (Join-Path $PSScriptRoot 'build-auto') -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_PREFIX_PATH=$qt" '-DCMAKE_BUILD_TYPE=Release' "-DCMAKE_CXX_COMPILER=$compiler\g++.exe"
if ($LASTEXITCODE -ne 0) { throw 'Qt configure failed' }
& $cmake --build (Join-Path $PSScriptRoot 'build-auto') --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Qt build failed' }
$outputDir = Join-Path $PSScriptRoot 'build-auto'
& (Join-Path $qt 'bin\windeployqt.exe') --release --no-translations --no-opengl-sw --no-compiler-runtime (Join-Path $outputDir 'WeatherClockConsole.exe')
if ($LASTEXITCODE -ne 0) { throw 'Qt runtime deployment failed' }
foreach ($dll in @('libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')) {
    Copy-Item -LiteralPath (Join-Path $compiler $dll) -Destination $outputDir -Force
}
Copy-Item -LiteralPath (Join-Path $qt 'plugins\platforms\qoffscreen.dll') -Destination (Join-Path $outputDir 'platforms') -Force
