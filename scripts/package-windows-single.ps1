param(
    [Parameter(Mandatory=$true)][string]$Directory,
    [Parameter(Mandatory=$true)][string]$Archive,
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'windows-common.ps1')
$Root = Split-Path -Parent $PSScriptRoot
$Directory = [IO.Path]::GetFullPath($Directory)
$Archive = [IO.Path]::GetFullPath($Archive)
$Output = [IO.Path]::GetFullPath($Output)
$Evidence = Join-Path (Split-Path -Parent $Output) 'validation/windows/single-exe'
New-Item -ItemType Directory -Path $Evidence -Force | Out-Null
$Temporary = Join-Path ([IO.Path]::GetTempPath()) ('stp-single-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $Temporary -Force | Out-Null
$Report = [ordered]@{ status='running'; mode='self-extracting user-cache portable executable'; signed=$false; checks=@(); error=$null }
try {
    $Hash = (Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash.ToLowerInvariant()
    $Manifest = Join-Path $Temporary 'manifest.txt'
    $Lines = @()
    foreach ($File in (Get-ChildItem -LiteralPath $Directory -Recurse -File | Sort-Object FullName)) {
        $Relative = $File.FullName.Substring($Directory.Length + 1).Replace('\','/')
        $Lines += ((Get-FileHash -LiteralPath $File.FullName -Algorithm SHA256).Hash.ToLowerInvariant() + ' ' + $Relative)
    }
    [IO.File]::WriteAllLines($Manifest, $Lines, (New-Object Text.UTF8Encoding($false)))
    $Source = Join-Path $Temporary 'launcher.cs'
    $Template = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'windows_portable_launcher.cs'))
    [IO.File]::WriteAllText($Source, $Template.Replace('@PAYLOAD_HASH@',$Hash), (New-Object Text.UTF8Encoding($false)))
    $Compiler = Join-Path $env:SystemRoot 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
    if (-not (Test-Path -LiteralPath $Compiler)) { throw 'The Windows .NET Framework 4.x C# compiler is missing.' }
    $Generated = Join-Path $Temporary 'STPViewer-Portable.exe'
    Invoke-StpNative $Compiler @('/nologo','/target:winexe','/platform:x64','/optimize+',('/out:'+$Generated),('/win32icon:'+(Join-Path $Root 'images/stpviewer-aeme.ico')),('/resource:'+$Archive+',payload'),('/resource:'+$Manifest+',manifest'),'/reference:System.IO.Compression.dll','/reference:System.IO.Compression.FileSystem.dll','/reference:System.Windows.Forms.dll',$Source) (Join-Path $Evidence 'compile') -TimeoutSeconds 300 | Out-Null
    # Only the wrapper EXE is copied to the launch directory; all dependencies
    # must be extracted from its embedded, checksum-verified snapshot.
    $Launch = Join-Path $Temporary 'single exe relocated'
    New-Item -ItemType Directory -Path $Launch -Force | Out-Null
    $Exe = Join-Path $Launch 'STPViewer-Portable.exe'
    Copy-Item -LiteralPath $Generated -Destination $Exe
    $CacheInfo = Join-Path $Temporary 'cache.txt'
    $ExpectedCache = Join-Path (Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'STPViewer/Portable') $Hash
    if (Test-Path -LiteralPath $ExpectedCache) {
        $PreviousCache = $ExpectedCache + '.validation-previous-' + [guid]::NewGuid().ToString('N')
        Move-Item -LiteralPath $ExpectedCache -Destination $PreviousCache
        $Report['previous_cache_backup'] = $PreviousCache
    }
    if (Test-Path -LiteralPath $ExpectedCache) { throw 'Cold-start verification cache was not cleared.' }
    Invoke-StpNative $Exe @('--portable-info',$CacheInfo) (Join-Path $Evidence 'cold-extract') -WorkingDirectory $Launch -TimeoutSeconds 180 | Out-Null
    $Cache = [IO.File]::ReadAllText($CacheInfo)
    if ([IO.Path]::GetFullPath($Cache) -ne [IO.Path]::GetFullPath($ExpectedCache)) { throw 'Single EXE used an unexpected verification cache.' }
    if (-not (Test-Path -LiteralPath (Join-Path $Cache 'platforms/qwindows.dll'))) { throw 'Single EXE did not extract the platform plugin.' }
    $Report.checks += @{ name='exe-only-cold-start-and-checksum-extraction'; status='passed' }
    Invoke-StpNative $Exe @('--version') (Join-Path $Evidence 'warm-version') -WorkingDirectory $Launch -TimeoutSeconds 60 | Out-Null
    $Report.checks += @{ name='warm-cache-launch'; status='passed' }
    # Deliberately alter an owned generated document, then verify that the
    # launcher repairs its cache and retains the altered directory as backup.
    Add-Content -LiteralPath (Join-Path $Cache 'STP_README.md') -Value 'generated cache recovery test'
    Invoke-StpNative $Exe @('--portable-info',$CacheInfo) (Join-Path $Evidence 'repair') -WorkingDirectory $Launch -TimeoutSeconds 180 | Out-Null
    if ((Get-FileHash (Join-Path $Cache 'STP_README.md')).Hash -ne (Get-FileHash (Join-Path $Directory 'STP_README.md')).Hash) { throw 'Cache recovery failed.' }
    $Report.checks += @{ name='modified-cache-recovery'; status='passed' }
    $UnicodeDirectory = Join-Path $Temporary (([string][char]0x6A21)+[char]0x578B+' with space')
    New-Item -ItemType Directory -Path $UnicodeDirectory -Force | Out-Null
    $Sample = Join-Path $UnicodeDirectory 'measurement-extension.step'
    Copy-Item -LiteralPath (Join-Path $Directory 'samples/measurement-extension.step') -Destination $Sample
    $Smoke = Join-Path $Temporary 'smoke with space'
    $RelativeSample = '..\' + (Split-Path -Leaf $UnicodeDirectory) + '\measurement-extension.step'
    Invoke-StpNative $Exe @('--smoke-test',$Smoke,$RelativeSample) (Join-Path $Evidence 'measurement-smoke') -WorkingDirectory $Launch -TimeoutSeconds 300 | Out-Null
    $Result = Get-Content -LiteralPath (Join-Path $Smoke 'smoke-report.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if (-not $Result.measurementSmokePassed -or -not $Result.measurementExtensionSmokePassed -or $Result.measurementRecordCount -ne 7) { throw 'Single EXE actual measurement smoke failed.' }
    $RetainedSmoke = Join-Path $Evidence 'smoke'
    if (Test-Path -LiteralPath $RetainedSmoke) { Remove-Item -LiteralPath $RetainedSmoke -Recurse -Force }
    Copy-Item -LiteralPath $Smoke -Destination $RetainedSmoke -Recurse -Force
    $Report.checks += @{ name='relative-chinese-spaced-path-and-seven-measurements'; status='passed' }
    Copy-Item -LiteralPath $Generated -Destination $Output -Force
    $Report.status='passed'; $Report.file=Split-Path -Leaf $Output; $Report.bytes=(Get-Item $Output).Length
    $Report.sha256=(Get-FileHash -LiteralPath $Output -Algorithm SHA256).Hash.ToLowerInvariant()
    $Report.embedded_zip_sha256=$Hash; $Report.cache=$Cache
    "$($Report.sha256)  $(Split-Path -Leaf $Output)" | Set-Content ($Output+'.sha256') -Encoding ascii
} catch { $Report.status='failed'; $Report.error=$_.Exception.Message; throw }
finally {
    $Cleanup = [ordered]@{ status='pending'; directory=$Temporary; attempts=0; error=$null }
    for ($Attempt=1; $Attempt -le 3; $Attempt++) {
        $Cleanup.attempts=$Attempt
        try {
            if (Test-Path -LiteralPath $Temporary) { Remove-Item -LiteralPath $Temporary -Recurse -Force -ErrorAction Stop }
            $Cleanup.status='completed'; $Cleanup.error=$null
            break
        } catch {
            $Cleanup.status='retained'; $Cleanup.error=$_.Exception.Message
            if ($Attempt -lt 3) { Start-Sleep -Milliseconds 500 }
        }
    }
    $Report['temporary_cleanup']=$Cleanup
    if ($Cleanup.status -eq 'retained') { Write-Warning "Generated verification directory retained: $Temporary. $($Cleanup.error)" -WarningAction Continue }
    # Cleanup and report-write failures must not replace the original build error.
    try { Write-StpJson (Join-Path $Evidence 'single-exe-report.json') $Report }
    catch { Write-Warning "Could not write single EXE validation report: $($_.Exception.Message)" -WarningAction Continue }
}
Write-Output $Output
