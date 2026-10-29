param(
    [ValidateRange(1,64)][int]$Jobs = 4,
    [switch]$SkipBootstrap,
    [switch]$SkipGuiTests,
    [string]$Output = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'windows-common.ps1')
$Root = Split-Path -Parent $PSScriptRoot
if (-not $Output) { $Output = Join-Path $Root 'dist' }
$Output = [System.IO.Path]::GetFullPath($Output)
$Validation = Join-Path $Output 'validation/windows'
New-Item -ItemType Directory -Path $Validation -Force | Out-Null
$ReportPath = Join-Path $Output 'windows-build-report.json'
$Report = [ordered]@{
    schema = 1; application = 'STPViewer'; version = '1.0.0'; status = 'running';
    started_utc = [DateTime]::UtcNow.ToString('o'); finished_utc = $null;
    github_actions = [ordered]@{ repository=$env:GITHUB_REPOSITORY; commit=$env:GITHUB_SHA; run_id=$env:GITHUB_RUN_ID };
    environment = [ordered]@{ os = [Environment]::OSVersion.VersionString; process_architecture = $env:PROCESSOR_ARCHITECTURE; native_architecture = $env:PROCESSOR_ARCHITEW6432; powershell_version = $PSVersionTable.PSVersion.ToString(); build_target = 'x64'; gpu_test_scope = 'local interactive session only' };
    dependencies = $null; checks = @(); package = $null; error = $null;
    acceptance_limits = @('This build is not validation on every supported Windows version.', 'Relocation on the build host is not an independent clean-machine test.', 'A 2000-instance fixture is not a 200 MB engineering-assembly acceptance test.', 'PMI support and split-export limitations remain as recorded in VALIDATION.md.')
}
function Save-Report { Write-StpJson $ReportPath $Report }
function Add-Check {
    param([string]$Name, [string]$Status, $Evidence)
    $Report.checks += [ordered]@{ name = $Name; status = $Status; evidence = $Evidence }
    Save-Report
}
Save-Report
$OriginalPath = $env:PATH
$OriginalLocation = (Get-Location).Path
$SavedOverrides = @{}
foreach ($Name in @('QT_QPA_PLATFORM_PLUGIN_PATH', 'CSF_ShadersDirectory')) { $SavedOverrides[$Name] = [Environment]::GetEnvironmentVariable($Name, 'Process') }
try {
    Set-Location -LiteralPath $Root
    & (Join-Path $PSScriptRoot 'validate-windows-scripts.ps1') -NoNative
    Add-Check 'powershell-script-parser' 'passed' @{ powershell=$PSVersionTable.PSVersion.ToString() }
    $Report.environment = Get-StpMachineEnvironment
    if ($Report.environment.native_cpu_architecture -eq 'ARM64') { $Report.acceptance_limits += 'This is x64-target validation on Windows ARM64 under emulation, not native x64 hardware validation.' }
    Initialize-StpMsvc
    $Lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'dependencies.lock.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $Report.dependencies = $Lock
    $Report.environment['msvc_tools_version'] = $env:VCToolsVersion
    $Report.environment['windows_sdk_version'] = $env:WindowsSDKVersion
    Add-Check 'msvc-x64-and-sdk' 'passed' @{ target_architecture = $env:VSCMD_ARG_TGT_ARCH; cl = (Get-Command cl.exe).Source }
    if (-not $SkipBootstrap) {
        & (Join-Path $PSScriptRoot 'bootstrap-windows.ps1') -Jobs $Jobs -LogDirectory (Join-Path $Validation 'bootstrap')
        Add-Check 'pinned-dependency-bootstrap' 'passed' @{ hashes = '.deps/downloads/SHA256SUMS.json' }
    } else { Add-Check 'pinned-dependency-bootstrap' 'skipped' 'Explicit -SkipBootstrap; existing pinned dependencies required.' }
    & (Join-Path $PSScriptRoot 'validate-windows-scripts.ps1') -LogDirectory (Join-Path $Validation 'script-tests')
    Add-Check 'native-argument-quoting-and-json' 'passed' @{ report='validation/windows/script-tests/script-test-report.json' }
    $Tools = Join-Path $Root '.tools/venv/Scripts'
    $Cmake = Join-Path $Tools 'cmake.exe'
    $Ctest = Join-Path $Tools 'ctest.exe'
    $Ninja = Join-Path $Tools 'ninja.exe'
    foreach ($Required in @($Cmake, $Ctest, $Ninja, (Join-Path $Root '.deps/paths.cmake'))) {
        if (-not (Test-Path -LiteralPath $Required)) { throw "Required generated build input is missing: $Required" }
    }
    $env:PATH = "$Tools;$env:PATH"
    foreach ($Tool in @(@{ name='cmake'; file=$Cmake }, @{ name='ninja'; file=$Ninja })) {
        Invoke-StpNative $Tool.file @('--version') (Join-Path $Validation $Tool.name) -TimeoutSeconds 30 | Out-Null
    }
    $Build = Join-Path $Root 'build-windows'
    Invoke-StpNative $Cmake @('-C', (Join-Path $Root '.deps/paths.cmake'), '-S', $Root, '-B', $Build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DSTP_BUILD_TESTS=ON') (Join-Path $Validation 'configure') | Out-Null
    Add-Check 'configure' 'passed' @{ generator='Ninja'; configuration='Release' }
    $Targets = @('STPViewer', 'stp_core_tests', 'stp_measurement_tests', 'stp_pmi_tests', 'stp_job_queue_tests', 'stp_placement_tests', 'stp_transform_panel_tests')
    if (-not $SkipGuiTests) { $Targets += @('stp_editing_integration_tests', 'stp_viewer_tests', 'stp_workspace_tests', 'stp_appearance_tests', 'stp_layout_tests', 'stp_measurement_integration_tests') }
    Invoke-StpNative $Cmake (@('--build', $Build, '--target') + $Targets + @('--parallel', [string]$Jobs)) (Join-Path $Validation 'build') -TimeoutSeconds 14400 | Out-Null
    Add-Check 'application-and-regression-build' 'passed' @{ targets=$Targets }
    $Qt = Join-Path $Root ".deps/qt/$($Lock.qt.version)/msvc2022_64"
    $Prefix = Join-Path $Root '.deps/install'
    $env:PATH = "$(Join-Path $Prefix 'bin');$(Join-Path $Qt 'bin');$env:PATH"
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $Qt 'plugins/platforms'
    $env:CSF_ShadersDirectory = Join-Path $Prefix 'share/opencascade/resources/Shaders'
    Invoke-StpNative $Ctest @('--test-dir', $Build, '--output-on-failure', '--output-junit', (Join-Path $Validation 'ctest.xml')) (Join-Path $Validation 'ctest') | Out-Null
    Add-Check 'core-and-pmi-ctest' 'passed' @{ junit='validation/windows/ctest.xml'; logs='validation/windows/ctest.*.log' }
    if (-not $SkipGuiTests) {
        Invoke-StpNative (Join-Path $Build 'stp_editing_integration_tests.exe') @((Join-Path $Root 'samples/colored-assembly.step'), (Join-Path $Validation 'editing')) (Join-Path $Validation 'editing-tests') -TimeoutSeconds 300 | Out-Null
        Add-Check 'interactive-editing-regression' 'passed' @{ logs='validation/windows/editing-tests.*.log'; evidence='validation/windows/editing' }
        Invoke-StpNative (Join-Path $Build 'stp_viewer_tests.exe') @((Join-Path $Root 'samples/colored-assembly.step')) (Join-Path $Validation 'viewer-tests') -TimeoutSeconds 300 | Out-Null
        Add-Check 'interactive-opengl-regression' 'passed' @{ logs='validation/windows/viewer-tests.*.log' }
        Invoke-StpNative (Join-Path $Build 'stp_measurement_integration_tests.exe') @((Join-Path $Validation 'measurement')) (Join-Path $Validation 'measurement-tests') -TimeoutSeconds 600 | Out-Null
        Add-Check 'interactive-measurement-regression' 'passed' @{ evidence='validation/windows/measurement' }
        foreach ($Test in @('workspace', 'appearance', 'layout')) {
            $TestOutput = Join-Path $Validation $Test
            New-Item -ItemType Directory -Path $TestOutput -Force | Out-Null
            $TestInput = if ($Test -eq 'layout') { Join-Path $Root 'samples/colored-assembly.step' } else { $Root }
            Invoke-StpNative (Join-Path $Build ("stp_{0}_tests.exe" -f $Test)) @($TestOutput, $TestInput) (Join-Path $Validation ("{0}-tests" -f $Test)) -TimeoutSeconds 600 | Out-Null
            Add-Check ("interactive-{0}-regression" -f $Test) 'passed' @{ evidence=("validation/windows/{0}" -f $Test) }
        }
    } else { Add-Check 'interactive-opengl-regression' 'skipped' 'Explicit -SkipGuiTests. Packaged smoke tests still run; physical desktop interaction remains unverified.' }
    & (Join-Path $PSScriptRoot 'package-windows.ps1') -Executable (Join-Path $Build 'STPViewer.exe') -Output $Output -DependencyPrefix $Prefix
    & (Join-Path $PSScriptRoot 'package-windows-single.ps1') -Directory (Join-Path $Output 'STPViewer-1.0.0-windows-x64') -Archive (Join-Path $Output 'STPViewer-1.0.0-windows-x64.zip') -Output (Join-Path $Output 'STPViewer-1.0.0-windows-x64-portable.exe')
    Add-Check 'single-exe-extraction-and-measurement' 'passed' @{ report='validation/windows/single-exe/single-exe-report.json' }
    $PackageReport = Join-Path $Output 'windows-bundle-validation.json'
    $Report.package = Get-Content -LiteralPath $PackageReport -Raw -Encoding UTF8 | ConvertFrom-Json
    Add-Check 'portable-zip-and-relocated-smokes' 'passed' @{ report='windows-bundle-validation.json'; sha256=$Report.package.sha256 }
    foreach ($Evidence in @('SHA256SUMS.json', 'DEPENDENCY_VERSIONS.json')) {
        $EvidenceSource = if ($Evidence -eq 'SHA256SUMS.json') { Join-Path $Root '.deps/downloads/SHA256SUMS.json' } else { Join-Path $Prefix 'share/stp-licenses/DEPENDENCY_VERSIONS.json' }
        if (Test-Path -LiteralPath $EvidenceSource) { Copy-Item -LiteralPath $EvidenceSource -Destination (Join-Path $Validation $Evidence) -Force }
    }
    $Report.status = 'passed'
} catch {
    $Report.status = 'failed'
    $Report.error = $_.Exception.Message
    [System.IO.File]::WriteAllText((Join-Path $Validation 'failure.txt'), ($_ | Out-String), (New-Object System.Text.UTF8Encoding($false)))
    Write-Host ("Build failed. Return {0} and validation/windows logs for diagnosis." -f $ReportPath) -ForegroundColor Red
    throw
} finally {
    $Report.finished_utc = [DateTime]::UtcNow.ToString('o')
    Save-Report
    $env:PATH = $OriginalPath
    foreach ($Name in $SavedOverrides.Keys) { [Environment]::SetEnvironmentVariable($Name, $SavedOverrides[$Name], 'Process') }
    Set-Location -LiteralPath $OriginalLocation
}
Write-Host ("Completed. Portable ZIP and machine-readable report are in {0}" -f $Output) -ForegroundColor Green
