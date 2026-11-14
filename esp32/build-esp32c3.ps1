param([string]$ArduinoCli = 'arduino-cli')
$ErrorActionPreference = 'Stop'
& $ArduinoCli compile --fqbn 'esp32:esp32:esp32c3:FlashMode=dio,FlashFreq=40,CDCOnBoot=default' --build-path (Join-Path $PSScriptRoot 'build') (Join-Path $PSScriptRoot 'VoiceFirmware')
if ($LASTEXITCODE -ne 0) { throw 'ESP32-C3 build failed' }
