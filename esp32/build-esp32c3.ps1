$ErrorActionPreference = 'Stop'
if (-not $env:IDF_PATH) { throw 'Activate the ESP-IDF 5.5 environment before building.' }
$idf = Join-Path $env:IDF_PATH 'tools/idf.py'
python $idf -C $PSScriptRoot build
if ($LASTEXITCODE -ne 0) { throw 'ESP-IDF build failed' }
