param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [string]$Output = '',
    [string]$DependencyPrefix = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'windows-common.ps1')
$Root = Split-Path -Parent $PSScriptRoot
if (-not $Output) { $Output = Join-Path $Root 'dist' }
if (-not $DependencyPrefix) { $DependencyPrefix = Join-Path $Root '.deps/install' }
$Output = [System.IO.Path]::GetFullPath($Output)
$PackageReportPath = Join-Path $Output 'windows-bundle-validation.json'
Write-StpJson $PackageReportPath @{ schema=1; status='running'; started_utc=[DateTime]::UtcNow.ToString('o'); architecture='x64' }
try {
$MachineEnvironment = Get-StpMachineEnvironment
Initialize-StpMsvc
$Exe = Get-Item -LiteralPath $Executable
$Lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'dependencies.lock.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$Qt = Join-Path $Root ".deps/qt/$($Lock.qt.version)/msvc2022_64"
$Validation = Join-Path $Output 'validation/windows/package'
New-Item -ItemType Directory -Path $Validation -Force | Out-Null
$Stage = Join-Path $Output 'STPViewer-1.0.0-windows-x64'
if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }
New-Item $Stage -ItemType Directory -Force | Out-Null
Copy-Item -LiteralPath $Exe.FullName -Destination $Stage
Get-ChildItem (Join-Path $DependencyPrefix 'bin') -Filter '*.dll' | Copy-Item -Destination $Stage
Invoke-StpNative (Join-Path $Qt 'bin/windeployqt.exe') @('--release', '--no-translations', '--no-compiler-runtime', (Join-Path $Stage $Exe.Name)) (Join-Path $Validation 'windeployqt') | Out-Null
if (-not (Test-Path -LiteralPath (Join-Path $Stage 'platforms/qwindows.dll'))) { throw 'The Windows Qt platform plugin is missing from the deployment.' }
# Copy only the official, redistributable release runtime, never the compiler's
# arbitrary shared/debug DLLs. No installer or admin privilege is needed.
if (-not $env:VCToolsRedistDir) { throw 'VCToolsRedistDir is missing. Initialize the VS2022 x64 developer environment.' }
$Crt = Get-ChildItem (Join-Path $env:VCToolsRedistDir 'x64') -Directory -Filter 'Microsoft.VC*.CRT' | Select-Object -First 1
if (-not $Crt) { throw 'Official x64 Visual C++ Redistributable CRT directory is missing.' }
Get-ChildItem $Crt.FullName -Filter '*.dll' | Copy-Item -Destination $Stage
New-Item (Join-Path $Stage 'resources') -ItemType Directory -Force | Out-Null
Copy-Item (Join-Path $DependencyPrefix 'share/opencascade/resources') (Join-Path $Stage 'resources/opencascade') -Recurse
$LicenseSource = Join-Path $DependencyPrefix 'share/stp-licenses'
foreach ($Required in @('occt-LICENSE_LGPL_21.txt', 'occt-OCCT_LGPL_EXCEPTION.txt', 'libxlsxwriter-License.txt', 'freetype-FTL.TXT', 'zlib-LICENSE', 'Qt/LGPL-3.0-only.txt', 'DEPENDENCY_VERSIONS.json')) {
    if (-not (Test-Path -LiteralPath (Join-Path $LicenseSource $Required))) { throw "Required dependency license is missing: $Required" }
}
Copy-Item -LiteralPath $LicenseSource -Destination (Join-Path $Stage 'licenses') -Recurse
$RuntimeNotice = @'
Microsoft Visual C++ release runtime (x64)
Source: the installed Visual Studio 2022 official VCToolsRedistDir/x64/Microsoft.VC*.CRT directory.
This runtime is redistributed under the applicable Microsoft Visual Studio license.
https://learn.microsoft.com/cpp/windows/redistributing-visual-cpp-files
Debug runtime and compiler binaries are not included.
'@
[System.IO.File]::WriteAllText((Join-Path $Stage 'licenses/Microsoft-VC-Runtime.txt'), $RuntimeNotice, (New-Object System.Text.UTF8Encoding($false)))
foreach ($Name in @('LICENSE.txt', 'THIRD_PARTY_NOTICES.md', 'STP_README.md', 'VALIDATION.md')) {
    $Path = Join-Path $Root $Name
    if (-not (Test-Path $Path)) { throw "Required distribution document is missing: $Name" }
    Copy-Item $Path $Stage
}
New-Item (Join-Path $Stage 'doc') -ItemType Directory -Force | Out-Null
foreach ($Name in @('stp_mac_measurement.md','stp_mac_part_grab.md','stp_windows_measurement.md','placement-editing.md')) { Copy-Item (Join-Path $Root "doc/$Name") (Join-Path $Stage 'doc') }
New-Item (Join-Path $Stage 'samples') -ItemType Directory -Force | Out-Null
Copy-Item (Join-Path $Root 'tests/inputs/cube.step') (Join-Path $Stage 'samples/cube.step')
if (Test-Path (Join-Path $Root 'samples')) {
    Copy-Item (Join-Path $Root 'samples/*') (Join-Path $Stage 'samples') -Recurse -Force
}
if (Test-Path (Join-Path $Root 'resources/fonts')) {
    Copy-Item (Join-Path $Root 'resources/fonts') (Join-Path $Stage 'resources/fonts') -Recurse
}
$QtTranslations = Join-Path $Qt 'translations'
if (Test-Path $QtTranslations) {
    New-Item (Join-Path $Stage 'translations') -ItemType Directory -Force | Out-Null
    Get-ChildItem $QtTranslations -Filter '*_zh_CN.qm' | Copy-Item -Destination (Join-Path $Stage 'translations')
}
New-Item (Join-Path $Stage 'translations') -ItemType Directory -Force | Out-Null
Get-ChildItem -LiteralPath (Join-Path $Root 'resources/translations') | Copy-Item -Destination (Join-Path $Stage 'translations') -Recurse -Force
if (-not (Test-Path -LiteralPath (Join-Path $Stage 'translations/qtbase_zh_CN.qm'))) { throw 'Required bundled Qt Chinese translation is missing.' }
"[Paths]`nPrefix = .`nPlugins = .`nTranslations = translations`n" | Set-Content (Join-Path $Stage 'qt.conf') -Encoding ascii
$Archive = Join-Path $Output 'STPViewer-1.0.0-windows-x64.zip'
# Audit PE machine type and recursive imported DLL closure before release.
$Dumpbin = (Get-Command dumpbin.exe -ErrorAction Stop).Source
$PeFiles = Get-ChildItem -LiteralPath $Stage -Recurse -File | Where-Object { $_.Extension -in @('.exe', '.dll') }
$PeInventory = @()
foreach ($Binary in $PeFiles) {
    $SafeName = $Binary.FullName.Substring($Stage.Length + 1) -replace '[\\/]', '_'
    $HeaderLog = Join-Path $Validation ($SafeName + '-headers')
    $ImportLog = Join-Path $Validation ($SafeName + '-imports')
    Invoke-StpNative $Dumpbin @('/headers', $Binary.FullName) $HeaderLog -TimeoutSeconds 60 | Out-Null
    $Headers = Get-Content -LiteralPath ($HeaderLog + '.stdout.log') -Raw
    if ($Headers -notmatch '(?im)^\s*8664\s') { throw "A deployed binary is not x64 PE: $SafeName" }
    Invoke-StpNative $Dumpbin @('/dependents', $Binary.FullName) $ImportLog -TimeoutSeconds 60 | Out-Null
    $Imports = @()
    foreach ($Line in (Get-Content -LiteralPath ($ImportLog + '.stdout.log'))) {
        if ($Line -match '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') {
            $Dll = $Matches[1]
            $AtRoot = Join-Path $Stage $Dll
            $Beside = Join-Path $Binary.DirectoryName $Dll
            $Resolution = if ((Test-Path -LiteralPath $AtRoot) -or (Test-Path -LiteralPath $Beside)) { 'bundled' }
                elseif ($Dll -match '^(api-ms-win-|ext-ms-win-)' -or (Test-Path -LiteralPath (Join-Path $env:SystemRoot "System32/$Dll"))) { 'Windows system / API set' }
                else { throw "Unresolved DLL import: $SafeName -> $Dll" }
            if ($Dll -match '^(Qt6|TK|vcruntime|msvcp|concrt)' -and $Resolution -ne 'bundled') { throw "Application runtime unexpectedly depends on a system installation: $Dll" }
            $Imports += @{ name=$Dll; resolution=$Resolution }
        }
    }
    if ($Imports.Count -eq 0) { throw "No imports could be read from $SafeName; inspect dumpbin logs." }
    $PeInventory += @{ path=$Binary.FullName.Substring($Stage.Length + 1); machine='x64 / 0x8664'; sha256=(Get-FileHash -LiteralPath $Binary.FullName -Algorithm SHA256).Hash.ToLowerInvariant(); imports=$Imports }
}
Write-StpJson (Join-Path $Stage 'runtime-inventory.json') ([ordered]@{
    schema=1; application='STPViewer'; architecture='x64'; binaries=$PeInventory;
    dependency_versions=$Lock; crt_source=$Crt.Name;
    scope='PE machine type and imported DLL closure; runtime validation is recorded in the separate bundle report.'
})
$ArchivePython = Find-StpPython $Validation
Invoke-StpNative $ArchivePython @('-X','utf8',(Join-Path $PSScriptRoot 'package_windows_archive.py'),$Stage,$Archive) (Join-Path $Validation 'portable-archive') -TimeoutSeconds 300 | Out-Null
# Test an extracted ZIP in a fresh directory with no developer dependency PATHs.
$SavedPath = $env:PATH
$SavedOverrides = @{}
$RuntimeVariables = Get-ChildItem Env: | Where-Object { $_.Name -match '^(QT_|QML_|CSF_|CASROOT$)' }
foreach ($Variable in $RuntimeVariables) {
    $SavedOverrides[$Variable.Name] = $Variable.Value
    Remove-Item "Env:$($Variable.Name)"
}
$Temporary = Join-Path ([System.IO.Path]::GetTempPath()) ('stp-portable-' + [guid]::NewGuid().ToString('N'))
$SmokeResults = @()
$Cleanup = [ordered]@{ status='pending'; directory=$Temporary; attempts=0; warning=$null }
try {
    Expand-Archive $Archive -DestinationPath $Temporary
    $Relocated = Join-Path $Temporary (Split-Path $Stage -Leaf)
    $env:PATH = "$env:SystemRoot/system32;$env:SystemRoot"
    Invoke-StpNative (Join-Path $Relocated $Exe.Name) @('--version') (Join-Path $Validation 'relocated-version') -WorkingDirectory $Relocated -TimeoutSeconds 45 | Out-Null
    foreach ($SampleName in @('cube.step', 'colored-assembly.step', 'pmi-basic.step', 'nist_ctc_01_asme1_ap242-e1.stp', 'measurement-reference.step', 'measurement-extension.step')) {
        $Sample = Join-Path $Relocated "samples/$SampleName"
        if (-not (Test-Path -LiteralPath $Sample)) { throw "Required portable verification sample is missing: $SampleName" }
        $SmokeName = 'relocated-smoke-' + [System.IO.Path]::GetFileNameWithoutExtension($SampleName)
        $Smoke = Join-Path $Temporary $SmokeName
        try {
            Invoke-StpNative (Join-Path $Relocated $Exe.Name) @('--smoke-test', $Smoke, $Sample) (Join-Path $Validation $SmokeName) -WorkingDirectory $Relocated -TimeoutSeconds 300 | Out-Null
        } finally {
            if (Test-Path -LiteralPath $Smoke) {
                # Copying a directory to an existing destination nests it and
                # leaves previous top-level reports/screenshots untouched.
                $RetainedSmoke = Join-Path $Validation $SmokeName
                if (Test-Path -LiteralPath $RetainedSmoke) { Remove-Item -LiteralPath $RetainedSmoke -Recurse -Force }
                Copy-Item -LiteralPath $Smoke -Destination $RetainedSmoke -Recurse -Force
            }
        }
        $SmokeReport = Get-Content -LiteralPath (Join-Path $Smoke 'smoke-report.json') -Raw -Encoding UTF8 | ConvertFrom-Json
        if (-not $SmokeReport.qtChineseTranslationLoaded -or $SmokeReport.qtCancelText -ne ([string][char]0x53D6 + [string][char]0x6D88)) { throw "Bundled Qt Chinese translation did not load: $SampleName" }
        if ($SmokeReport.error -or -not $SmokeReport.screenshot -or -not $SmokeReport.sectionScreenshot -or $SmokeReport.parts -lt 1 -or -not $SmokeReport.exportedFiles -or -not (Test-Path -LiteralPath (Join-Path $Smoke 'BOM.xlsx'))) { throw "Portable STEP/Excel/screenshot/split verification failed: $SampleName" }
        if ($SampleName -in @('measurement-reference.step','measurement-extension.step')) {
            $ExpectedRecords = if ($SampleName -eq 'measurement-reference.step') { 10 } else { 7 }
            if (-not $SmokeReport.measurementReferenceSmokePassed -or -not $SmokeReport.measurementExtensionSmokePassed -or -not $SmokeReport.measurementSmokePassed -or $SmokeReport.measurementRecordCount -ne $ExpectedRecords) { throw "Portable measurement verification failed: $SampleName" }
        }
        foreach ($SplitFile in $SmokeReport.exportedFiles) { if (-not (Test-Path -LiteralPath $SplitFile)) { throw "Reported split export is missing: $SplitFile" } }
        $SmokeResults += @{ sample=$SampleName; status='passed'; report="validation/windows/package/$SmokeName/smoke-report.json"; parts=$SmokeReport.parts; load_ms=$SmokeReport.loadMs }
    }
} finally {
    $env:PATH = $SavedPath
    foreach ($Name in $SavedOverrides.Keys) { Set-Item "Env:$Name" $SavedOverrides[$Name] }
    # Windows can briefly retain a plugin file after an emulated process exits.
    # Cleanup must neither hide a failed test nor discard successful evidence.
    for ($Attempt=1; $Attempt -le 5; $Attempt++) {
        $Cleanup.attempts=$Attempt
        try {
            if (Test-Path -LiteralPath $Temporary) { Remove-Item -LiteralPath $Temporary -Recurse -Force -ErrorAction Stop }
            $Cleanup.status='completed'; $Cleanup.warning=$null
            break
        } catch {
            $Cleanup.status='retained'; $Cleanup.warning=$_.Exception.Message
            if ($Attempt -lt 5) { Start-Sleep -Milliseconds 500 }
        }
    }
    if ($Cleanup.status -eq 'retained') { Write-Warning "Generated temporary verification directory retained: $Temporary. $($Cleanup.warning)" -WarningAction Continue }
}
$Hash = (Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash.ToLowerInvariant()
"$Hash  $(Split-Path $Archive -Leaf)" | Set-Content "$Archive.sha256" -Encoding ascii
Write-StpJson (Join-Path $Output 'windows-bundle-validation.json') ([ordered]@{
    schema=1; status='passed'; archive=(Split-Path $Archive -Leaf); sha256=$Hash; bytes=(Get-Item -LiteralPath $Archive).Length;
    architecture='x64'; environment=$MachineEnvironment;
    relocated=$true; signed=$false; developer_environment_cleared=$true; pe_inventory=$PeInventory; smoke_tests=$SmokeResults; temporary_cleanup=$Cleanup;
    completed_utc=[DateTime]::UtcNow.ToString('o'); dependency_versions=$Lock; crt_source=$Crt.Name;
    limitations=@('Ad-hoc build-host relocation only; separate clean Windows and minimum Windows version still require validation.', 'ARM64-host x64 emulation, if used, does not prove native x64 hardware or native ARM64 support.')
})
Write-Output $Archive
} catch {
    Write-StpJson $PackageReportPath @{ schema=1; status='failed'; architecture='x64'; completed_utc=[DateTime]::UtcNow.ToString('o'); error=$_.Exception.Message }
    throw
}
