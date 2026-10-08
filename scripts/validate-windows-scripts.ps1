param([switch]$NoNative, [string]$LogDirectory = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'windows-common.ps1')
$Root = Split-Path -Parent $PSScriptRoot
foreach ($Name in @('windows-common.ps1', 'bootstrap-windows.ps1', 'build-windows.ps1', 'package-windows.ps1', 'package-windows-single.ps1', 'prepare-windows-tools.ps1', 'validate-windows-scripts.ps1')) {
    $Tokens = $null
    $ParseErrors = $null
    [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $Name), [ref]$Tokens, [ref]$ParseErrors) | Out-Null
    if ($ParseErrors.Count -ne 0) { throw "PowerShell parse failed for $Name`: $($ParseErrors | Out-String)" }
    if ([System.IO.File]::ReadAllBytes((Join-Path $PSScriptRoot $Name)) | Where-Object { $_ -gt 127 }) { throw "Keep Windows scripts ASCII for PowerShell 5.1: $Name" }
}
if ($NoNative) { Write-Host 'PowerShell parser checks passed.'; return }
if (-not $LogDirectory) { $LogDirectory = Join-Path $Root 'dist/validation/windows/script-tests' }
New-Item -ItemType Directory -Path $LogDirectory -Force | Out-Null
$Python = Join-Path $Root '.tools/venv/Scripts/python.exe'
if (-not (Test-Path -LiteralPath $Python)) { throw 'Run bootstrap-windows.ps1 before native script checks.' }
$Chinese = ([string][char]0x96f6) + [char]0x4ef6
$ArgumentsToTest = @('', 'plain', 'C:\with space\', 'literal"quote', 'backslash\"quote', ($Chinese + ' spaced.step'))
Invoke-StpNative $Python (@('-X', 'utf8', '-c', 'import sys,json; print(json.dumps(sys.argv[1:],ensure_ascii=False))') + $ArgumentsToTest) (Join-Path $LogDirectory 'native-arguments') -TimeoutSeconds 30 | Out-Null
$DecodedArguments = Get-Content -LiteralPath (Join-Path $LogDirectory 'native-arguments.stdout.log') -Raw -Encoding UTF8 | ConvertFrom-Json
# PowerShell 5.1 emits a root JSON array as one pipeline object. Normalize the
# decoded value directly so the test checks six arguments on both 5.1 and 7.
$Actual = @($DecodedArguments)
if ($Actual.Count -ne $ArgumentsToTest.Count) { throw 'Native argument count mismatch.' }
for ($Index = 0; $Index -lt $Actual.Count; $Index++) {
    if ([string]$Actual[$Index] -cne [string]$ArgumentsToTest[$Index]) { throw "Native quoting mismatch at index $Index." }
}
$JsonPath = Join-Path $LogDirectory 'json-roundtrip.json'
Write-StpJson $JsonPath @{ text=$Chinese; value=1.25 }
$Roundtrip = Get-Content -LiteralPath $JsonPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($Roundtrip.text -cne $Chinese -or $Roundtrip.value -ne 1.25) { throw 'UTF-8 JSON roundtrip failed.' }
$CaughtFailure = $false
try { Invoke-StpNative -FilePath $Python -Arguments @('-c', 'import sys; sys.exit(23)') -LogPrefix (Join-Path $LogDirectory 'native-exit-code') -TimeoutSeconds 30 | Out-Null }
catch { if ($_.Exception.Message -notmatch 'exit code 23\.') { throw }; $CaughtFailure = $true }
if (-not $CaughtFailure) { throw 'Nonzero native exit code was incorrectly accepted.' }
Write-StpJson (Join-Path $LogDirectory 'script-test-report.json') @{ status='passed'; powershell=$PSVersionTable.PSVersion.ToString(); parser=$true; native_argument_cases=$Actual.Count; unicode_json=$true; nonzero_exit_code=23; completed_utc=[DateTime]::UtcNow.ToString('o') }
Write-Host 'PowerShell parser, native argument quoting, exit code and UTF-8 JSON checks passed.'
