# Shared Windows helpers. Keep this file ASCII for Windows PowerShell 5.1.
Set-StrictMode -Version 2.0

function Get-StpMachineEnvironment {
    $System = Get-CimInstance Win32_OperatingSystem
    $Cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $ArchitectureNames = @{ 0='x86'; 5='ARM'; 9='x64'; 12='ARM64' }
    $CpuArchitecture = if ($ArchitectureNames.ContainsKey([int]$Cpu.Architecture)) { $ArchitectureNames[[int]$Cpu.Architecture] } else { [string]$Cpu.Architecture }
    return [ordered]@{
        os_name=$System.Caption; os_version=$System.Version; os_build=$System.BuildNumber;
        native_cpu_architecture=$CpuArchitecture; cpu_name=$Cpu.Name; memory_bytes=([int64]$System.TotalVisibleMemorySize * 1024);
        process_architecture=$env:PROCESSOR_ARCHITECTURE; emulation_environment=$env:PROCESSOR_ARCHITEW6432;
        powershell_version=$PSVersionTable.PSVersion.ToString(); build_target='x64'; gpu_test_scope='local interactive session only'
    }
}

function Write-StpJson {
    param([Parameter(Mandatory=$true)][string]$Path, [Parameter(Mandatory=$true)]$Value)
    $Parent = Split-Path -Parent $Path
    if ($Parent) { New-Item -ItemType Directory -Path $Parent -Force | Out-Null }
    [System.IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 20) + "`n", (New-Object System.Text.UTF8Encoding($false)))
}

function ConvertTo-StpNativeArgument {
    param([AllowEmptyString()][string]$Value)
    # Windows CommandLineToArgvW / MSVC quoting, including a trailing backslash.
    if ($Value -and $Value -notmatch '[\s"]') { return $Value }
    $Value = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $Value = [regex]::Replace($Value, '(\\+)$', '$1$1')
    return '"' + $Value + '"'
}

function Invoke-StpNative {
    param(
        [Parameter(Mandatory=$true, Position=0)][string]$FilePath,
        [Parameter(Position=1)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Arguments = @(),
        [Parameter(Mandatory=$true, Position=2)][string]$LogPrefix,
        [string]$WorkingDirectory = (Get-Location).Path,
        [int]$TimeoutSeconds = 7200
    )
    New-Item -ItemType Directory -Path (Split-Path -Parent $LogPrefix) -Force | Out-Null
    $Quoted = ($Arguments | ForEach-Object { ConvertTo-StpNativeArgument ([string]$_) }) -join ' '
    Write-Host ("Running: {0} {1}" -f (Split-Path $FilePath -Leaf), $Quoted)
    Write-Host ("Logs: {0}.stdout.log / .stderr.log" -f $LogPrefix)
    $Start = [DateTime]::UtcNow
    # Own the Process that performs Start(), including its original handle.
    # Drain both pipes asynchronously to files; waiting on a full pipe deadlocks.
    $StartInfo = New-Object System.Diagnostics.ProcessStartInfo
    $StartInfo.FileName = $FilePath
    $StartInfo.Arguments = $Quoted
    $StartInfo.WorkingDirectory = $WorkingDirectory
    $StartInfo.UseShellExecute = $false
    $StartInfo.CreateNoWindow = $true
    $StartInfo.RedirectStandardOutput = $true
    $StartInfo.RedirectStandardError = $true
    $Process = New-Object System.Diagnostics.Process
    $Process.StartInfo = $StartInfo
    $StdoutFile = $null
    $StderrFile = $null
    try {
        $StdoutFile = [System.IO.File]::Open($LogPrefix + '.stdout.log', [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
        $StderrFile = [System.IO.File]::Open($LogPrefix + '.stderr.log', [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
        if (-not $Process.Start()) { throw "Starting process failed: $FilePath" }
        $null = $Process.Handle
        $StdoutCopy = $Process.StandardOutput.BaseStream.CopyToAsync($StdoutFile)
        $StderrCopy = $Process.StandardError.BaseStream.CopyToAsync($StderrFile)
        if (-not $Process.WaitForExit($TimeoutSeconds * 1000)) {
            $Process.Kill()
            $Process.WaitForExit(10000) | Out-Null
            throw "Process timed out after $TimeoutSeconds seconds. See $LogPrefix.*.log"
        }
        $Process.WaitForExit()
        if (-not $StdoutCopy.Wait(30000) -or -not $StderrCopy.Wait(30000)) { throw "Process exited but output pipes did not close. See $LogPrefix.*.log" }
        $StdoutFile.Flush()
        $StderrFile.Flush()
        $ExitCode = [int]$Process.ExitCode
    } finally {
        if ($StdoutFile) { $StdoutFile.Dispose() }
        if ($StderrFile) { $StderrFile.Dispose() }
        $Process.Dispose()
    }
    $Result = [ordered]@{ exit_code = $ExitCode; duration_ms = [math]::Round(([DateTime]::UtcNow - $Start).TotalMilliseconds); stdout_log = $LogPrefix + '.stdout.log'; stderr_log = $LogPrefix + '.stderr.log' }
    if ($ExitCode -ne 0) {
        Get-Content -LiteralPath ($LogPrefix + '.stderr.log') -Tail 30 -ErrorAction SilentlyContinue | Write-Host
        Get-Content -LiteralPath ($LogPrefix + '.stdout.log') -Tail 30 -ErrorAction SilentlyContinue | Write-Host
        throw "Process failed with exit code $ExitCode. See $LogPrefix.*.log"
    }
    return $Result
}

function Initialize-StpMsvc {
    if ($env:OS -ne 'Windows_NT' -or -not [Environment]::Is64BitOperatingSystem -or -not [Environment]::Is64BitProcess) {
        throw 'Use 64-bit Windows PowerShell 5.1 or PowerShell 7 on Windows x64.'
    }
    if ((Get-Command cl.exe -ErrorAction SilentlyContinue) -and $env:VSCMD_ARG_TGT_ARCH -eq 'x64' -and $env:VSCMD_VER -match '^17\.' -and $env:WindowsSdkDir) { return }
    $VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $VsWhere)) { throw 'Visual Studio 2022 Build Tools not found. Install the Desktop development with C++ workload (x64/x86 MSVC and Windows SDK), then retry.' }
    $VsRoot = & $VsWhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $VsRoot) { throw 'VS2022 C++ x64/x86 build tools are missing. Add the Desktop development with C++ workload in Visual Studio Installer.' }
    $DeveloperCommand = Join-Path ([string]($VsRoot | Select-Object -First 1)) 'Common7/Tools/VsDevCmd.bat'
    if (-not (Test-Path -LiteralPath $DeveloperCommand)) { throw 'VS2022 VsDevCmd.bat is missing.' }
    $CompilerHost = 'x64'
    $DefaultVersion = Join-Path ([string]($VsRoot | Select-Object -First 1)) 'VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt'
    if (Test-Path -LiteralPath $DefaultVersion) {
        $InstalledVersion = (Get-Content -LiteralPath $DefaultVersion -Raw).Trim()
        $ArmHostCompiler = Join-Path ([string]($VsRoot | Select-Object -First 1)) "VC/Tools/MSVC/$InstalledVersion/bin/HostARM64/x64/cl.exe"
        if ((Test-Path -LiteralPath $ArmHostCompiler) -and (Get-StpMachineEnvironment).native_cpu_architecture -eq 'ARM64') { $CompilerHost = 'arm64' }
    }
    # Read only the initialized environment; never print the full environment.
    $Command = '""' + $DeveloperCommand + '" -no_logo -arch=x64 -host_arch=' + $CompilerHost + ' && set"'
    # Pass cmd's raw command line directly. Windows PowerShell 5.1 re-quotes
    # nested quotes in a native '& cmd.exe ...' call, breaking Program Files.
    # Capture the complete environment only in memory, never in output/logs.
    $StartInfo = New-Object System.Diagnostics.ProcessStartInfo
    $StartInfo.FileName = $env:ComSpec
    $StartInfo.Arguments = '/d /s /c ' + $Command
    $StartInfo.UseShellExecute = $false
    $StartInfo.CreateNoWindow = $true
    $StartInfo.RedirectStandardOutput = $true
    $StartInfo.RedirectStandardError = $true
    $Process = New-Object System.Diagnostics.Process
    $Process.StartInfo = $StartInfo
    try {
        if (-not $Process.Start()) { throw 'Starting VS2022 environment initialization failed.' }
        $null = $Process.Handle
        $StdoutRead = $Process.StandardOutput.ReadToEndAsync()
        $StderrRead = $Process.StandardError.ReadToEndAsync()
        if (-not $Process.WaitForExit(60000)) {
            $Process.Kill()
            throw 'VS2022 environment initialization timed out after 60 seconds.'
        }
        $Process.WaitForExit()
        if (-not $StdoutRead.Wait(10000) -or -not $StderrRead.Wait(10000)) { throw 'VS2022 environment output did not close after initialization.' }
        $Stdout = $StdoutRead.GetAwaiter().GetResult()
        $Stderr = $StderrRead.GetAwaiter().GetResult()
        $CommandExit = [int]$Process.ExitCode
    } finally { $Process.Dispose() }
    $EnvironmentLines = $Stdout -split "`r?`n"
    if ($CommandExit -ne 0) {
        $SafeErrors = @(($EnvironmentLines + ($Stderr -split "`r?`n")) | Where-Object { $_ -and $_ -notmatch '=' } | Select-Object -First 8) -join ' '
        if ($SafeErrors.Length -gt 2000) { $SafeErrors = $SafeErrors.Substring(0,2000) }
        throw "Initializing VS2022 x64 environment failed (exit $CommandExit). $SafeErrors"
    }
    foreach ($Line in $EnvironmentLines) {
        if ([string]$Line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue) -or $env:VSCMD_ARG_TGT_ARCH -ne 'x64' -or -not $env:WindowsSdkDir) { throw 'MSVC x64 or the Windows SDK is unavailable after VsDevCmd.' }
}

function Find-StpPython {
    param([string]$LogDirectory)
    $Candidates = @()
    $ProjectPython = Join-Path (Split-Path -Parent $PSScriptRoot) '.tools/python312/python.exe'
    if (Test-Path -LiteralPath $ProjectPython) { $Candidates += @{ path = $ProjectPython; arguments = @() } }
    $Launcher = Get-Command py.exe -ErrorAction SilentlyContinue
    if ($Launcher) {
        foreach ($Version in @('-3.12', '-3.13', '-3.14', '-3')) { $Candidates += @{ path = $Launcher.Source; arguments = @($Version) } }
    }
    foreach ($Name in @('python.exe', 'python3.exe')) {
        $Command = Get-Command $Name -ErrorAction SilentlyContinue
        if ($Command -and $Command.Source -notmatch '\\WindowsApps\\') { $Candidates += @{ path = $Command.Source; arguments = @() } }
    }
    $Index = 0
    foreach ($Candidate in $Candidates) {
        $Index++
        $Prefix = Join-Path $LogDirectory "python-probe-$Index"
        try {
            $Probe = @($Candidate.arguments) + @('-X', 'utf8', '-c', 'import sys,struct; assert sys.version_info >= (3,12) and struct.calcsize("P")==8; print(sys.executable)')
            Invoke-StpNative $Candidate.path $Probe $Prefix -TimeoutSeconds 15 | Out-Null
            $Python = (Get-Content -LiteralPath ($Prefix + '.stdout.log') -Raw -Encoding UTF8).Trim()
            if (Test-Path -LiteralPath $Python) { return $Python }
        } catch { Write-Host "Skipping unavailable Python candidate $Index." }
    }
    throw '64-bit Python 3.12 or newer not found. Install Python 3.12 x64 from python.org with the launcher (py), then retry. A Microsoft Store alias does not count.'
}
