param([ValidateRange(1,64)][int]$Jobs = 4, [switch]$SkipQt, [string]$LogDirectory = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'windows-common.ps1')
$Root = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $Root
if (-not $LogDirectory) { $LogDirectory = Join-Path $Root 'dist/validation/windows/bootstrap' }
New-Item -ItemType Directory -Path $LogDirectory -Force | Out-Null
Initialize-StpMsvc
$Lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'dependencies.lock.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$Python = Join-Path $Root '.tools/venv/Scripts/python.exe'
if (-not (Test-Path -LiteralPath $Python)) {
    $SystemPython = Find-StpPython $LogDirectory
    Invoke-StpNative $SystemPython @('-X', 'utf8', '-m', 'venv', (Join-Path $Root '.tools/venv')) (Join-Path $LogDirectory 'create-venv') | Out-Null
}
Invoke-StpNative $Python @('-X', 'utf8', '-c', 'import sys,struct; assert sys.version_info >= (3,12) and struct.calcsize("P")==8; print(sys.version)') (Join-Path $LogDirectory 'private-python') -TimeoutSeconds 30 | Out-Null
Invoke-StpNative $Python @('-X', 'utf8', '-m', 'pip', 'install', '--disable-pip-version-check', "cmake==$($Lock.tools.cmake)", "ninja==$($Lock.tools.ninja)", "aqtinstall==$($Lock.tools.aqtinstall)") (Join-Path $LogDirectory 'private-tools') | Out-Null
$env:PATH = "$(Join-Path $Root '.tools/venv/Scripts');$env:PATH"
$Extra = @()
if ($SkipQt) { $Extra += '--skip-qt' }
Invoke-StpNative $Python (@('-X', 'utf8', (Join-Path $PSScriptRoot 'bootstrap_deps.py'), '--platform', 'windows', '--jobs', [string]$Jobs) + $Extra) (Join-Path $LogDirectory 'dependencies') -TimeoutSeconds 14400 | Out-Null
