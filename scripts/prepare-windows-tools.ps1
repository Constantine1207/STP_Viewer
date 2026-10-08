param(
    [switch]$InstallBuildTools,
    [switch]$InstallPython,
    [string]$OutputDirectory = ''
)
# Download/verify only by default. Installation requires explicit switches.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'windows-common.ps1')
$Root = Split-Path -Parent $PSScriptRoot
if ($env:OS -ne 'Windows_NT' -or -not [Environment]::Is64BitProcess) { throw 'Run in 64-bit Windows PowerShell 5.1 or PowerShell 7.' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $Root '.tools/installers' }
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$ReportPath = Join-Path $Root 'dist/windows-tools-report.json'
$Report = [ordered]@{ schema=1; status='running'; started_utc=[DateTime]::UtcNow.ToString('o'); finished_utc=$null; downloads=@(); installations=@(); error=$null }
Write-StpJson $ReportPath $Report
$OriginalTls = [System.Net.ServicePointManager]::SecurityProtocol
try {
    [System.Net.ServicePointManager]::SecurityProtocol = $OriginalTls -bor [System.Net.SecurityProtocolType]::Tls12
    $Installers = @(
        @{ name='Visual Studio 2022 Build Tools'; file='vs2022_buildtools.exe'; url='https://aka.ms/vs/17/release/vs_buildtools.exe'; publisher='Microsoft Corporation'; sha256='' },
        @{ name='Python 3.12.10 x64'; file='python-3.12.10-amd64.exe'; url='https://www.python.org/ftp/python/3.12.10/python-3.12.10-amd64.exe'; publisher='Python Software Foundation'; sha256='67b5635e80ea51072b87941312d00ec8927c4db9ba18938f7ad2d27b328b95fb' }
    )
    foreach ($Installer in $Installers) {
        $Destination = Join-Path $OutputDirectory $Installer.file
        if (-not (Test-Path -LiteralPath $Destination)) {
            Write-Host ("Downloading official {0}; no installation yet." -f $Installer.name)
            $Partial = $Destination + '.part'
            Invoke-WebRequest -Uri $Installer.url -UseBasicParsing -OutFile $Partial -TimeoutSec 900
            Move-Item -LiteralPath $Partial -Destination $Destination -Force
        }
        $Signature = Get-AuthenticodeSignature -LiteralPath $Destination
        if ($Signature.Status -ne 'Valid' -or -not $Signature.SignerCertificate -or $Signature.SignerCertificate.Subject -notmatch [regex]::Escape($Installer.publisher)) {
            throw "Official installer signature/publisher verification failed for $($Installer.name): $($Signature.Status). The file will not be executed."
        }
        $Hash = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash.ToLowerInvariant()
        # Python hash pinned from its official .exe.sigstore message digest.
        # VS uses the explicit major-17 release channel and publisher signature;
        # its moving-channel bootstrapper hash is recorded for this actual run.
        if ($Installer.sha256 -and $Hash -ne $Installer.sha256) { throw "Installer SHA-256 mismatch for $($Installer.name). The file will not be executed." }
        $Report.downloads += [ordered]@{ name=$Installer.name; file=$Destination; url=$Installer.url; bytes=(Get-Item -LiteralPath $Destination).Length; sha256=$Hash; signature_status=[string]$Signature.Status; publisher=$Signature.SignerCertificate.Subject; certificate_thumbprint=$Signature.SignerCertificate.Thumbprint }
        Write-StpJson $ReportPath $Report
    }
    if ($InstallPython) {
        $PythonTarget = Join-Path $Root '.tools/python312'
        $PythonLog = Join-Path $OutputDirectory 'python-install.log'
        $PythonArguments = @('/quiet', '/log', $PythonLog, 'InstallAllUsers=0', ('TargetDir=' + $PythonTarget), 'Include_launcher=1', 'InstallLauncherAllUsers=0', 'PrependPath=0', 'Include_pip=1', 'Include_test=0', 'Include_doc=0', 'Include_tcltk=0')
        $Result = Invoke-StpNative -FilePath (Join-Path $OutputDirectory 'python-3.12.10-amd64.exe') -Arguments $PythonArguments -LogPrefix (Join-Path $OutputDirectory 'python-install') -TimeoutSeconds 1200
        if (-not (Test-Path -LiteralPath (Join-Path $PythonTarget 'python.exe'))) { throw 'Python installer exited successfully but project Python was not found.' }
        $Report.installations += @{ name='Python'; status='installed'; target=$PythonTarget; result=$Result; scope='current user; system PATH unchanged' }
        Write-StpJson $ReportPath $Report
    }
    if ($InstallBuildTools) {
        $BuildToolsArguments = @('--passive', '--wait', '--norestart', '--add', 'Microsoft.VisualStudio.Workload.VCTools', '--add', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '--add', 'Microsoft.VisualStudio.Component.Windows11SDK.26100')
        try {
            $Result = Invoke-StpNative -FilePath (Join-Path $OutputDirectory 'vs2022_buildtools.exe') -Arguments $BuildToolsArguments -LogPrefix (Join-Path $OutputDirectory 'vs2022-install') -TimeoutSeconds 14400
            $Report.installations += @{ name='Visual Studio 2022 Build Tools'; status='installed'; result=$Result; scope='system developer tools; installer elevation may be required' }
        } catch {
            if ($_.Exception.Message -match 'exit code 3010\.') {
                $Report.installations += @{ name='Visual Studio 2022 Build Tools'; status='installed-reboot-required'; exit_code=3010 }
            } else { throw }
        }
        Write-StpJson $ReportPath $Report
    }
    $Report.status = if ($InstallBuildTools -or $InstallPython) { 'installation-completed' } else { 'downloaded-and-verified-only' }
} catch {
    $Report.status='failed'
    $Report.error=$_.Exception.Message
    throw
} finally {
    [System.Net.ServicePointManager]::SecurityProtocol = $OriginalTls
    $Report.finished_utc=[DateTime]::UtcNow.ToString('o')
    Write-StpJson $ReportPath $Report
}
Write-Host ("Result: {0}; report: {1}" -f $Report.status, $ReportPath)
