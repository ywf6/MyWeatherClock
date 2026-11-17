param([string]$IdfRoot,[string]$ToolsRoot)
$ErrorActionPreference='Stop'
$taskRoot=$PSScriptRoot
$taskOldBuild=Join-Path (Split-Path $taskRoot) 'esp32/build'
if (-not $IdfRoot) {
    $taskDescription=Get-Content -LiteralPath (Join-Path $taskOldBuild 'project_description.json') -Raw | ConvertFrom-Json
    $IdfRoot=$taskDescription.idf_path
}
if (-not $ToolsRoot) { $ToolsRoot=Join-Path (Split-Path $IdfRoot) 'esp-idf-5.5.4-tools' }
$taskPython=Join-Path $ToolsRoot 'python_env/idf5.5_py3.13_env/Scripts/python.exe'
$taskCache=Get-Content -LiteralPath (Join-Path $taskOldBuild 'CMakeCache.txt')
$taskNinja=($taskCache | Where-Object {$_ -match '^CMAKE_MAKE_PROGRAM:FILEPATH='}) -replace '^CMAKE_MAKE_PROGRAM:FILEPATH=',''
$taskCompiler=($taskCache | Where-Object {$_ -match '^CMAKE_CXX_COMPILER_AR:FILEPATH='}) -replace '^CMAKE_CXX_COMPILER_AR:FILEPATH=',''
$taskCmake=(Get-ChildItem -LiteralPath (Join-Path $ToolsRoot 'tools/cmake') -Filter cmake.exe -Recurse | Select-Object -First 1).FullName
foreach($taskTool in @($taskPython,$taskNinja,$taskCompiler,$taskCmake)) {if(-not(Test-Path -LiteralPath $taskTool)){throw "Missing tool: $taskTool"}}
$env:IDF_PATH=$IdfRoot
$env:IDF_TOOLS_PATH=$ToolsRoot
$env:IDF_PYTHON_ENV_PATH=Split-Path (Split-Path $taskPython)
$env:PATH=(Split-Path $taskPython)+';'+(Split-Path $taskNinja)+';'+(Split-Path $taskCompiler)+';'+(Split-Path $taskCmake)+';'+$env:PATH
& $taskPython -B (Join-Path $taskRoot 'prepare_config.py')
if($LASTEXITCODE -ne 0){throw 'Local configuration failed'}
& $taskPython (Join-Path $IdfRoot 'tools/idf.py') -C (Join-Path $taskRoot 'firmware') -D "CMAKE_MAKE_PROGRAM=$taskNinja" build
if($LASTEXITCODE -ne 0){throw 'C3 validation firmware build failed'}
