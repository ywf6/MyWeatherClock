param([ValidateSet('echo','dashscope')][string]$Backend='echo',[string]$BindAddress='0.0.0.0',[int]$Port=8000,[ValidateRange(1,65535)][int]$DashboardPort=8001)
$ErrorActionPreference='Stop'
$taskPython=Join-Path $PSScriptRoot '.venv/Scripts/python.exe'
if(-not(Test-Path -LiteralPath $taskPython)){throw 'Create .venv and install server/requirements.txt first.'}
& $taskPython -B (Join-Path $PSScriptRoot 'server/server.py') --host $BindAddress --port $Port --backend $Backend --dashboard-port $DashboardPort
exit $LASTEXITCODE
