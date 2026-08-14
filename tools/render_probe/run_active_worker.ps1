# Worker for run_active.ps1. It is launched only by that controller through a
# Scheduled Task principal bound to the controller SID and active console user.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$RequestPath
)

$ErrorActionPreference = 'Stop'
$ScriptDirectory = $PSScriptRoot
$ProjectRoot = (Resolve-Path -LiteralPath (Join-Path $ScriptDirectory '..\..')).Path
$ExpectedRunScriptPath = [System.IO.Path]::GetFullPath((Join-Path $ScriptDirectory 'run.ps1'))
$BridgeRoot = [System.IO.Path]::GetFullPath((Join-Path $ProjectRoot 'dump\render-probe\bridge'))
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Initialize-WorkerConsoleInterop {
    if ($null -ne ('BullyRenderProbeActiveWorker.ConsoleNative' -as [type])) {
        return
    }

    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace BullyRenderProbeActiveWorker
{
    public static class ConsoleNative
    {
        private enum WTS_INFO_CLASS
        {
            WTSUserName = 5,
            WTSConnectState = 8
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern UInt32 WTSGetActiveConsoleSessionId();

        [DllImport("wtsapi32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "WTSQuerySessionInformationW")]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool WTSQuerySessionInformation(
            IntPtr hServer,
            Int32 sessionId,
            WTS_INFO_CLASS wtsInfoClass,
            out IntPtr ppBuffer,
            out Int32 pBytesReturned);

        [DllImport("wtsapi32.dll")]
        private static extern void WTSFreeMemory(IntPtr pMemory);

        public static string GetSessionUserName(Int32 sessionId)
        {
            IntPtr buffer;
            Int32 bytesReturned;
            if (!WTSQuerySessionInformation(IntPtr.Zero, sessionId, WTS_INFO_CLASS.WTSUserName, out buffer, out bytesReturned))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "WTSQuerySessionInformation(WTSUserName) failed.");
            }
            try
            {
                return Marshal.PtrToStringUni(buffer) ?? String.Empty;
            }
            finally
            {
                if (buffer != IntPtr.Zero) WTSFreeMemory(buffer);
            }
        }

        public static Int32 GetSessionConnectState(Int32 sessionId)
        {
            IntPtr buffer;
            Int32 bytesReturned;
            if (!WTSQuerySessionInformation(IntPtr.Zero, sessionId, WTS_INFO_CLASS.WTSConnectState, out buffer, out bytesReturned))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "WTSQuerySessionInformation(WTSConnectState) failed.");
            }
            try
            {
                if (bytesReturned < sizeof(Int32))
                {
                    throw new InvalidOperationException("WTSQuerySessionInformation(WTSConnectState) returned an invalid buffer.");
                }
                return Marshal.ReadInt32(buffer);
            }
            finally
            {
                if (buffer != IntPtr.Zero) WTSFreeMemory(buffer);
            }
        }
    }
}
'@ -ErrorAction Stop
}

function Get-CurrentSid {
    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    if (($null -eq $identity) -or ($null -eq $identity.User)) {
        throw 'The scheduled-task worker could not resolve its SID.'
    }
    return $identity.User.Value
}

function Get-ActiveConsoleSessionId {
    Initialize-WorkerConsoleInterop
    $sessionId = [uint32][BullyRenderProbeActiveWorker.ConsoleNative]::WTSGetActiveConsoleSessionId()
    if ($sessionId -eq [uint32]::MaxValue) {
        throw 'The scheduled-task worker found no active physical console session (0xFFFFFFFF).'
    }
    if ($sessionId -eq 0) {
        throw 'The scheduled-task worker resolved active console session 0 and will not continue.'
    }
    return [int]$sessionId
}

function Get-ActiveConsoleUserRecord {
    param(
        [Parameter(Mandatory = $true)]
        [int]$SessionId
    )

    Initialize-WorkerConsoleInterop
    $connectState = [int][BullyRenderProbeActiveWorker.ConsoleNative]::GetSessionConnectState($SessionId)
    if ($connectState -ne 0) {
        throw ('The scheduled-task worker found active console session {0} in non-active WTS state {1}.' -f $SessionId, $connectState)
    }

    $userName = [BullyRenderProbeActiveWorker.ConsoleNative]::GetSessionUserName($SessionId)
    if ([string]::IsNullOrWhiteSpace($userName)) {
        throw ('The scheduled-task worker found no active user name for console session {0}.' -f $SessionId)
    }

    return [pscustomobject][ordered]@{
        sessionId    = $SessionId
        userName     = $userName
        connectState = $connectState
        stateName    = 'WTSActive'
    }
}

function ConvertTo-PowerShellSingleQuotedLiteral {
    param([Parameter(Mandatory = $true)][string]$Value)
    return "'{0}'" -f $Value.Replace("'", "''")
}

function ConvertTo-EncodedPowerShellCommand {
    param([Parameter(Mandatory = $true)][string]$Command)
    return [Convert]::ToBase64String([System.Text.Encoding]::Unicode.GetBytes($Command))
}

function Write-AtomicJson {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object]$Object,
        [int]$Depth = 12
    )

    $temporaryPath = '{0}.{1}.{2}.tmp' -f $Path, $PID, [guid]::NewGuid().ToString('N')
    try {
        $json = $Object | ConvertTo-Json -Depth $Depth
        [System.IO.File]::WriteAllText($temporaryPath, $json, $Utf8NoBom)
        Move-Item -LiteralPath $temporaryPath -Destination $Path -Force
    }
    finally {
        if (Test-Path -LiteralPath $temporaryPath -PathType Leaf) {
            Remove-Item -LiteralPath $temporaryPath -Force -ErrorAction SilentlyContinue
        }
    }
}

function Write-TextFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [AllowNull()][string]$Text
    )
    [System.IO.File]::WriteAllText($Path, [string]$Text, $Utf8NoBom)
}

function Get-NormalizedDirectoryPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path).TrimEnd([char[]]@('\', '/'))
}

function Get-SafeArtifactDirectoryFromRequestPath {
    param([Parameter(Mandatory = $true)][string]$ResolvedRequestPath)

    if (-not [string]::Equals([System.IO.Path]::GetFileName($ResolvedRequestPath), 'request.json', [System.StringComparison]::OrdinalIgnoreCase)) {
        return $null
    }

    $candidateDirectory = Get-NormalizedDirectoryPath -Path ([System.IO.Path]::GetDirectoryName($ResolvedRequestPath))
    $normalizedBridgeRoot = (Get-NormalizedDirectoryPath -Path $BridgeRoot) + [System.IO.Path]::DirectorySeparatorChar
    if (-not $candidateDirectory.StartsWith($normalizedBridgeRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $null
    }

    $candidateGuid = Split-Path -Path $candidateDirectory -Leaf
    try {
        [void][guid]::Parse($candidateGuid)
    }
    catch {
        return $null
    }

    return $candidateDirectory
}

function Assert-Request {
    param(
        [Parameter(Mandatory = $true)][object]$Request,
        [Parameter(Mandatory = $true)][string]$ResolvedRequestPath
    )

    foreach ($property in @('bridgeGuid', 'taskName', 'expectedSessionId', 'expectedSid', 'activeConsoleUser', 'activeConsoleConnectState', 'runScriptPath', 'artifactDirectory', 'invocationMode', 'parameters')) {
        if ($null -eq $Request.$property -or [string]::IsNullOrWhiteSpace([string]$Request.$property)) {
            throw ('Bridge request is missing required property {0}.' -f $property)
        }
    }

    $guid = [guid]::Parse([string]$Request.bridgeGuid).ToString('N')
    $taskName = [string]$Request.taskName
    if ($taskName -notmatch ('^BullyRenderProbeActive-\d+-{0}$' -f [regex]::Escape($guid))) {
        throw ('Bridge request task name is not a recognized per-invocation task name: {0}.' -f $taskName)
    }

    $artifactDirectory = Get-NormalizedDirectoryPath -Path ([string]$Request.artifactDirectory)
    $requestDirectory = Get-NormalizedDirectoryPath -Path ([System.IO.Path]::GetDirectoryName($ResolvedRequestPath))
    if (-not [string]::Equals($artifactDirectory, $requestDirectory, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw 'Bridge request artifactDirectory must be the request.json directory.'
    }
    $normalizedBridgeRoot = (Get-NormalizedDirectoryPath -Path $BridgeRoot) + [System.IO.Path]::DirectorySeparatorChar
    if (-not $artifactDirectory.StartsWith($normalizedBridgeRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw ('Bridge request artifactDirectory is outside the bridge artifact root: {0}.' -f $artifactDirectory)
    }
    if (-not [string]::Equals((Split-Path -Leaf $artifactDirectory), $guid, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw 'Bridge request GUID does not match its artifact directory name.'
    }

    $requestedRunScript = [System.IO.Path]::GetFullPath([string]$Request.runScriptPath)
    if (-not [string]::Equals($requestedRunScript, $ExpectedRunScriptPath, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw ('Bridge request attempted to invoke an unexpected script: {0}.' -f $requestedRunScript)
    }
    if (-not (Test-Path -LiteralPath $requestedRunScript -PathType Leaf)) {
        throw ('Existing run.ps1 was not found: {0}.' -f $requestedRunScript)
    }

    $mode = [string]$Request.invocationMode
    if ($mode -notin @('validate-display-only', 'validate-only', 'validate-history', 'runtime')) {
        throw ('Bridge request has unsupported invocationMode {0}.' -f $mode)
    }
    if ([int]$Request.expectedSessionId -le 0) {
        throw 'Bridge request expectedSessionId must be a non-session-0 value.'
    }
    if ([int]$Request.activeConsoleConnectState -ne 0) {
        throw ('Bridge request activeConsoleConnectState must be WTSActive (0), not {0}.' -f $Request.activeConsoleConnectState)
    }
    if ([string]::IsNullOrWhiteSpace([string]$Request.activeConsoleUser)) {
        throw 'Bridge request activeConsoleUser must identify the active console user.'
    }
    if ([string]$Request.expectedSid -notmatch '^S-\d+(?:-\d+)+$') {
        throw ('Bridge request expectedSid is not a SID: {0}.' -f $Request.expectedSid)
    }

    return [pscustomobject][ordered]@{
        bridgeGuid        = $guid
        taskName          = $taskName
        artifactDirectory = $artifactDirectory
        runScriptPath     = $requestedRunScript
        invocationMode    = $mode
    }
}

function New-RunPs1EncodedCommand {
    param(
        [Parameter(Mandatory = $true)][string]$ResolvedRequestPath,
        [Parameter(Mandatory = $true)][ValidateSet('preflight', 'validate-display-only', 'validate-only', 'validate-history', 'runtime')][string]$Mode
    )

    $requestPathLiteral = ConvertTo-PowerShellSingleQuotedLiteral -Value $ResolvedRequestPath
    $modeLiteral = ConvertTo-PowerShellSingleQuotedLiteral -Value $Mode
    $command = @"
`$ErrorActionPreference = 'Stop'
`$ProgressPreference = 'SilentlyContinue'
`$request = Get-Content -LiteralPath $requestPathLiteral -Raw | ConvertFrom-Json
`$runParameters = @{
    Backend = [string]`$request.parameters.backend
    On12Device = [string]`$request.parameters.on12Device
    ForceSwapEffect = [string]`$request.parameters.forceSwapEffect
    ForcePresentInterval = [string]`$request.parameters.forcePresentInterval
    DurationSeconds = [int]`$request.parameters.durationSeconds
    CaptureAtSeconds = @(`$request.parameters.captureAtSeconds | ForEach-Object { [string]`$_ })
}
if ([bool]`$request.parameters.noInstall) { `$runParameters.NoInstall = `$true }
if ([bool]`$request.parameters.forceInstall) { `$runParameters.ForceInstall = `$true }
if ([bool]`$request.parameters.allowVirtualDisplay) { `$runParameters.AllowVirtualDisplay = `$true }
switch ($modeLiteral) {
    'preflight' { `$runParameters.ValidateDisplayOnly = `$true }
    'validate-display-only' { `$runParameters.ValidateDisplayOnly = `$true }
    'validate-only' { `$runParameters.ValidateOnly = `$true }
    'validate-history' { `$runParameters.ValidateHistory = `$true }
    'runtime' { }
    default { throw ('Unsupported worker invocation mode: ' + $modeLiteral) }
}
& ([string]`$request.runScriptPath) @runParameters
exit `$LASTEXITCODE
"@

    return ConvertTo-EncodedPowerShellCommand -Command $command
}

function Invoke-ExistingRunPs1 {
    param(
        [Parameter(Mandatory = $true)][string]$ResolvedRequestPath,
        [Parameter(Mandatory = $true)][ValidateSet('preflight', 'validate-display-only', 'validate-only', 'validate-history', 'runtime')][string]$Mode,
        [Parameter(Mandatory = $true)][string]$StdoutPath,
        [Parameter(Mandatory = $true)][string]$StderrPath
    )

    $powershellExe = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
    if (-not (Test-Path -LiteralPath $powershellExe -PathType Leaf)) {
        throw ('Windows PowerShell executable was not found at {0}.' -f $powershellExe)
    }

    $process = New-Object System.Diagnostics.Process
    $stdout = ''
    $stderr = ''
    try {
        $process.StartInfo.FileName = $powershellExe
        $process.StartInfo.Arguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand {0}' -f (New-RunPs1EncodedCommand -ResolvedRequestPath $ResolvedRequestPath -Mode $Mode)
        $process.StartInfo.WorkingDirectory = $ScriptDirectory
        $process.StartInfo.UseShellExecute = $false
        $process.StartInfo.CreateNoWindow = $true
        $process.StartInfo.RedirectStandardOutput = $true
        $process.StartInfo.RedirectStandardError = $true
        if (-not $process.Start()) {
            throw ('Could not start existing run.ps1 for mode {0}.' -f $Mode)
        }

        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        $stdout = $stdoutTask.Result
        $stderr = $stderrTask.Result
        $exitCode = [int]$process.ExitCode
    }
    finally {
        Write-TextFile -Path $StdoutPath -Text $stdout
        Write-TextFile -Path $StderrPath -Text $stderr
        if ($null -ne $process) {
            $process.Dispose()
        }
    }

    return [pscustomobject][ordered]@{
        mode       = $Mode
        exitCode   = $exitCode
        stdoutPath = $StdoutPath
        stderrPath = $StderrPath
    }
}

function Merge-OutputFiles {
    param(
        [AllowEmptyCollection()][string[]]$SourcePaths,
        [Parameter(Mandatory = $true)][string]$DestinationPath
    )

    $paths = @($SourcePaths | Where-Object {
        -not [string]::IsNullOrWhiteSpace([string]$_) -and (Test-Path -LiteralPath $_ -PathType Leaf)
    })
    if ($paths.Count -eq 0) {
        Write-TextFile -Path $DestinationPath -Text ''
        return
    }
    if ($paths.Count -eq 1) {
        [System.IO.File]::Copy($paths[0], $DestinationPath, $true)
        return
    }

    $builder = New-Object System.Text.StringBuilder
    foreach ($path in $paths) {
        [void]$builder.AppendFormat('=== {0} ==={1}', (Split-Path -Path $path -Leaf), [Environment]::NewLine)
        [void]$builder.Append([System.IO.File]::ReadAllText($path))
        if (-not $builder.ToString().EndsWith([Environment]::NewLine)) {
            [void]$builder.AppendLine()
        }
    }
    Write-TextFile -Path $DestinationPath -Text $builder.ToString()
}

function Get-ReportPathFromOutput {
    param([Parameter(Mandatory = $true)][string]$StdoutPath)

    if (-not (Test-Path -LiteralPath $StdoutPath -PathType Leaf)) {
        return $null
    }

    $matches = [regex]::Matches([System.IO.File]::ReadAllText($StdoutPath), '(?im)\bReport:\s*(?<path>[^\r\n]+)\s*$')
    if ($matches.Count -eq 0) {
        return $null
    }

    $candidate = $matches[$matches.Count - 1].Groups['path'].Value.Trim()
    if ($candidate -notmatch '(?i)report\.json$') {
        return $null
    }
    return $candidate
}

$request = $null
$requestInfo = $null
$artifactDirectory = $null
$resultPath = $null
$combinedStdoutPath = $null
$combinedStderrPath = $null
$runs = New-Object System.Collections.ArrayList
$result = [ordered]@{
    schemaVersion           = 1
    bridgeGuid              = $null
    taskName                = $null
    expectedSessionId       = $null
    actualSessionId         = $null
    activeConsoleSessionId  = $null
    activeConsoleUser       = $null
    activeConsoleConnectState = $null
    expectedSid             = $null
    actualSid               = $null
    userInteractive         = [bool][Environment]::UserInteractive
    invocationMode          = $null
    childExitCode           = 1
    childStdoutPath         = $null
    childStderrPath         = $null
    preflightStdoutPath     = $null
    preflightStderrPath     = $null
    preflightExitCode       = $null
    runStdoutPath           = $null
    runStderrPath           = $null
    reportPath              = $null
    requestPath             = $RequestPath
    resultPath              = $null
    startedUtc              = (Get-Date).ToUniversalTime().ToString('o')
    finishedUtc             = $null
    status                  = 'not-started'
    error                   = $null
}
$workerExitCode = 1

try {
    $resolvedRequestPath = [System.IO.Path]::GetFullPath($RequestPath)
    if (-not (Test-Path -LiteralPath $resolvedRequestPath -PathType Leaf)) {
        throw ('Bridge request.json was not found: {0}.' -f $resolvedRequestPath)
    }

    # Before parsing untrusted request JSON, derive a result location only when
    # the passed path is already confined to a GUID-named bridge directory.
    $artifactDirectory = Get-SafeArtifactDirectoryFromRequestPath -ResolvedRequestPath $resolvedRequestPath
    if ($null -ne $artifactDirectory) {
        $resultPath = Join-Path $artifactDirectory 'result.json'
        $combinedStdoutPath = Join-Path $artifactDirectory 'child-stdout.txt'
        $combinedStderrPath = Join-Path $artifactDirectory 'child-stderr.txt'
        $result.childStdoutPath = $combinedStdoutPath
        $result.childStderrPath = $combinedStderrPath
        $result.resultPath = $resultPath
        Write-TextFile -Path $combinedStdoutPath -Text ''
        Write-TextFile -Path $combinedStderrPath -Text ''
    }

    $request = Get-Content -LiteralPath $resolvedRequestPath -Raw | ConvertFrom-Json -ErrorAction Stop
    $requestInfo = Assert-Request -Request $request -ResolvedRequestPath $resolvedRequestPath
    if (-not [string]::Equals($artifactDirectory, $requestInfo.artifactDirectory, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw 'Bridge request artifactDirectory did not match the safely derived request.json directory.'
    }
    $result.bridgeGuid = $requestInfo.bridgeGuid
    $result.taskName = $requestInfo.taskName
    $result.expectedSessionId = [int]$request.expectedSessionId
    $result.expectedSid = [string]$request.expectedSid
    $result.invocationMode = $requestInfo.invocationMode
    $result.childStdoutPath = $combinedStdoutPath
    $result.childStderrPath = $combinedStderrPath
    $result.resultPath = $resultPath

    $result.actualSessionId = [int](Get-Process -Id $PID -ErrorAction Stop).SessionId
    $result.actualSid = Get-CurrentSid
    $result.activeConsoleSessionId = Get-ActiveConsoleSessionId
    $activeConsoleUser = Get-ActiveConsoleUserRecord -SessionId $result.activeConsoleSessionId
    $result.activeConsoleUser = $activeConsoleUser.userName
    $result.activeConsoleConnectState = $activeConsoleUser.connectState
    $result.userInteractive = [bool][Environment]::UserInteractive

    if (-not [string]::Equals($result.actualSid, $result.expectedSid, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw ('Scheduled-task worker SID mismatch. Expected {0}; actual {1}. Refusing to target another user.' -f $result.expectedSid, $result.actualSid)
    }
    if ($result.actualSessionId -ne $result.expectedSessionId) {
        throw ('Scheduled-task worker session mismatch. Expected active console session {0}; actual process session {1}.' -f $result.expectedSessionId, $result.actualSessionId)
    }
    if ($result.activeConsoleSessionId -ne $result.expectedSessionId) {
        throw ('The active console changed while the task started. Expected session {0}; current active console session is {1}.' -f $result.expectedSessionId, $result.activeConsoleSessionId)
    }
    if (-not [string]::Equals($result.activeConsoleUser, [string]$request.activeConsoleUser, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw ('The active console user changed while the task started. Expected {0}; current user is {1}.' -f $request.activeConsoleUser, $result.activeConsoleUser)
    }
    if (-not $result.userInteractive) {
        throw 'Scheduled-task worker is not attached to an interactive user desktop. Refusing to invoke run.ps1.'
    }

    if ($requestInfo.invocationMode -eq 'runtime') {
        # This no-mutation call checks the input desktop and real desktop capture
        # before the separate runtime process can stage files or start Bully.exe.
        $result.preflightStdoutPath = Join-Path $artifactDirectory 'preflight-stdout.txt'
        $result.preflightStderrPath = Join-Path $artifactDirectory 'preflight-stderr.txt'
        $preflightRun = Invoke-ExistingRunPs1 `
            -ResolvedRequestPath $resolvedRequestPath `
            -Mode 'preflight' `
            -StdoutPath $result.preflightStdoutPath `
            -StderrPath $result.preflightStderrPath
        [void]$runs.Add($preflightRun)
        $result.preflightExitCode = [int]$preflightRun.exitCode
        if (($preflightRun.exitCode -ne 0) -and (-not [bool]$request.parameters.allowVirtualDisplay)) {
            throw ('Existing run.ps1 display preflight failed with exit code {0}; the runtime probe was not started.' -f $preflightRun.exitCode)
        }

        $result.runStdoutPath = Join-Path $artifactDirectory 'run-stdout.txt'
        $result.runStderrPath = Join-Path $artifactDirectory 'run-stderr.txt'
        $run = Invoke-ExistingRunPs1 `
            -ResolvedRequestPath $resolvedRequestPath `
            -Mode 'runtime' `
            -StdoutPath $result.runStdoutPath `
            -StderrPath $result.runStderrPath
        [void]$runs.Add($run)
        $result.reportPath = Get-ReportPathFromOutput -StdoutPath $run.stdoutPath
        $workerExitCode = [int]$run.exitCode
    }
    else {
        $result.runStdoutPath = Join-Path $artifactDirectory 'run-stdout.txt'
        $result.runStderrPath = Join-Path $artifactDirectory 'run-stderr.txt'
        $run = Invoke-ExistingRunPs1 `
            -ResolvedRequestPath $resolvedRequestPath `
            -Mode $requestInfo.invocationMode `
            -StdoutPath $result.runStdoutPath `
            -StderrPath $result.runStderrPath
        [void]$runs.Add($run)
        $workerExitCode = [int]$run.exitCode
    }

    $result.status = if ($workerExitCode -eq 0) { 'completed' } else { 'child-failed' }
}
catch {
    $result.status = 'bridge-failed'
    $result.error = $_.Exception.Message
    if (-not [string]::IsNullOrWhiteSpace($combinedStderrPath)) {
        [System.IO.File]::AppendAllText($combinedStderrPath, ('run_active_worker.ps1 failed: {0}{1}' -f $result.error, [Environment]::NewLine), $Utf8NoBom)
    }
    $workerExitCode = 1
}
finally {
    $result.childExitCode = [int]$workerExitCode
    $result.finishedUtc = (Get-Date).ToUniversalTime().ToString('o')
    if (-not [string]::IsNullOrWhiteSpace($resultPath)) {
        try {
            Merge-OutputFiles -SourcePaths @($result.preflightStdoutPath, $result.runStdoutPath) -DestinationPath $combinedStdoutPath
            Merge-OutputFiles -SourcePaths @($result.preflightStderrPath, $result.runStderrPath) -DestinationPath $combinedStderrPath
            if (-not [string]::IsNullOrWhiteSpace($result.error)) {
                [System.IO.File]::AppendAllText($combinedStderrPath, ('run_active_worker.ps1 failed: {0}{1}' -f $result.error, [Environment]::NewLine), $Utf8NoBom)
            }
            Write-AtomicJson -Path $resultPath -Object $result -Depth 12
        }
        catch {
            [Console]::Error.WriteLine(('run_active_worker.ps1 could not write result.json: {0}' -f $_.Exception.Message))
            $workerExitCode = 1
        }
    }
    else {
        [Console]::Error.WriteLine(('run_active_worker.ps1 failed before a safe result path was available: {0}' -f $result.error))
    }
}

exit $workerExitCode
