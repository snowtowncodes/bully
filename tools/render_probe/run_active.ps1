# Requires Windows PowerShell 5.1+ on Windows.
# Runs the existing render harness in the currently active physical console
# session without changing run.ps1's staging, preflight, or cleanup behavior.
[CmdletBinding()]
param(
    [switch]$ValidateDisplayOnly,

    [switch]$ValidateOnly,

    [switch]$ValidateHistory,

    # Runtime probes deliberately require an explicit acknowledgement because
    # they can launch Bully.exe on the currently unlocked console desktop.
    [switch]$AllowActiveDesktopLaunch,

    # Verifies this controller and its worker without creating a task.
    [switch]$ValidateBridgeOnly,

    [ValidateSet('on12', 'native')]
    [string]$Backend = 'on12',

    [ValidateRange(1, 3600)]
    [int]$DurationSeconds = 40,

    [string[]]$CaptureAtSeconds = @('5', '15', '30'),

    [switch]$NoInstall,

    [switch]$ForceInstall,

    [switch]$AllowVirtualDisplay,

    [ValidateSet('internal', 'explicit')]
    [string]$On12Device = 'internal',

    [ValidateSet('none', 'discard', 'flip', 'copy')]
    [string]$ForceSwapEffect = 'none',

    [ValidateSet('none', 'immediate', 'default', 'one')]
    [string]$ForcePresentInterval = 'none'
)

$ErrorActionPreference = 'Stop'

$ScriptDirectory = $PSScriptRoot
$ProjectRoot = (Resolve-Path -LiteralPath (Join-Path $ScriptDirectory '..\..')).Path
$RunScriptPath = Join-Path $ScriptDirectory 'run.ps1'
$WorkerScriptPath = Join-Path $ScriptDirectory 'run_active_worker.ps1'
$TaskPrefix = 'BullyRenderProbeActive-'
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function ConvertTo-NormalizedCaptureTimes {
    [OutputType([int[]])]
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$InputValues
    )

    $normalized = New-Object 'System.Collections.Generic.List[int]'
    foreach ($inputValue in @($InputValues)) {
        if ($null -eq $inputValue) {
            continue
        }

        foreach ($token in ([string]$inputValue -split '[,;\s]+')) {
            if ([string]::IsNullOrWhiteSpace($token)) {
                continue
            }
            if ($token -notmatch '^[+-]?\d+$') {
                throw ("Invalid CaptureAtSeconds token '{0}'. Use non-negative base-10 integers separated by commas, semicolons, or whitespace." -f $token)
            }

            $parsed = 0
            $parsedSuccessfully = [int]::TryParse(
                $token,
                [System.Globalization.NumberStyles]::AllowLeadingSign,
                [System.Globalization.CultureInfo]::InvariantCulture,
                [ref]$parsed)
            if (-not $parsedSuccessfully) {
                throw ("CaptureAtSeconds token '{0}' is outside the Int32 range." -f $token)
            }
            if ($parsed -lt 0) {
                throw ("CaptureAtSeconds token '{0}' must be non-negative." -f $token)
            }

            [void]$normalized.Add($parsed)
        }
    }

    if ($normalized.Count -eq 0) {
        throw 'CaptureAtSeconds must contain at least one non-negative base-10 integer.'
    }

    return [int[]]@($normalized | Sort-Object -Unique)
}

function Initialize-ActiveConsoleInterop {
    if ($null -ne ('BullyRenderProbeActive.ControllerNative' -as [type])) {
        return
    }

    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace BullyRenderProbeActive
{
    public static class ControllerNative
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

function Get-ActiveConsoleSessionId {
    Initialize-ActiveConsoleInterop
    $sessionId = [uint32][BullyRenderProbeActive.ControllerNative]::WTSGetActiveConsoleSessionId()
    if ($sessionId -eq [uint32]::MaxValue) {
        throw 'No active physical console session is available (WTSGetActiveConsoleSessionId returned 0xFFFFFFFF). Unlock and sign in to the intended desktop, then retry.'
    }
    if ($sessionId -eq 0) {
        throw 'The active physical console resolved to session 0. Refusing to schedule into the service/session-0 desktop.'
    }

    return [int]$sessionId
}

function Get-CurrentSid {
    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    if (($null -eq $identity) -or ($null -eq $identity.User)) {
        throw 'Could not resolve the current controller SID. The bridge only targets the SID that invoked it.'
    }
    return $identity.User.Value
}

function Get-ActiveConsoleUserRecord {
    param(
        [Parameter(Mandatory = $true)]
        [int]$SessionId
    )

    Initialize-ActiveConsoleInterop
    $connectState = [int][BullyRenderProbeActive.ControllerNative]::GetSessionConnectState($SessionId)
    if ($connectState -ne 0) {
        throw ('Active console session {0} is not WTSActive (WTS connect state {1}). Unlock and sign in to the intended physical console before retrying.' -f $SessionId, $connectState)
    }

    $userName = [BullyRenderProbeActive.ControllerNative]::GetSessionUserName($SessionId)
    if ([string]::IsNullOrWhiteSpace($userName)) {
        throw ('Active console session {0} has no active user name. Refusing to schedule a task without a confirmed interactive user.' -f $SessionId)
    }

    return [pscustomobject][ordered]@{
        sessionId    = $SessionId
        userName     = $userName
        connectState = $connectState
        stateName    = 'WTSActive'
    }
}

function Get-ExplorerIdentityForSession {
    param(
        [Parameter(Mandatory = $true)]
        [int]$SessionId
    )

    try {
        $explorers = @(Get-CimInstance -ClassName Win32_Process -Filter ("Name = 'explorer.exe' AND SessionId = {0}" -f $SessionId) -ErrorAction Stop)
    }
    catch {
        throw ('Could not query explorer.exe in active console session {0}. {1}' -f $SessionId, $_.Exception.Message)
    }

    if ($explorers.Count -eq 0) {
        throw ('No explorer.exe was found in active console session {0}. Unlock the intended desktop and verify that its shell is running before retrying.' -f $SessionId)
    }

    $identities = New-Object System.Collections.ArrayList
    foreach ($explorer in $explorers) {
        try {
            $owner = Invoke-CimMethod -InputObject $explorer -MethodName GetOwnerSid -ErrorAction Stop
        }
        catch {
            throw ('Could not resolve the owner SID for explorer.exe PID {0} in session {1}. {2}' -f $explorer.ProcessId, $SessionId, $_.Exception.Message)
        }

        if (($null -eq $owner) -or ([uint32]$owner.ReturnValue -ne 0) -or [string]::IsNullOrWhiteSpace([string]$owner.Sid)) {
            $returnValue = if ($null -eq $owner) { 'no result' } else { [string]$owner.ReturnValue }
            throw ('Could not resolve the owner SID for explorer.exe PID {0} in session {1} (GetOwnerSid={2}).' -f $explorer.ProcessId, $SessionId, $returnValue)
        }

        [void]$identities.Add([pscustomobject][ordered]@{
            processId = [int]$explorer.ProcessId
            sessionId = [int]$explorer.SessionId
            sid       = [string]$owner.Sid
        })
    }

    return @($identities)
}

function Assert-ActiveConsoleOwnership {
    param(
        [Parameter(Mandatory = $true)]
        [int]$SessionId,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedSid
    )

    $explorerIdentities = @(Get-ExplorerIdentityForSession -SessionId $SessionId)
    $mismatches = @($explorerIdentities | Where-Object {
        -not [string]::Equals([string]$_.sid, $ExpectedSid, [System.StringComparison]::OrdinalIgnoreCase)
    })
    if ($mismatches.Count -gt 0) {
        $found = @($explorerIdentities | ForEach-Object { 'PID {0}: {1}' -f $_.processId, $_.sid }) -join '; '
        throw ('Active console session {0} has explorer.exe ownership that does not match the controller SID. Expected {1}; found {2}. Refusing to target another user.' -f $SessionId, $ExpectedSid, $found)
    }

    return $explorerIdentities
}

function Test-ControllerDependencies {
    $paths = @($PSCommandPath, $WorkerScriptPath, $RunScriptPath)
    foreach ($path in $paths) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw ('Required bridge dependency is missing: {0}' -f $path)
        }

        $tokens = $null
        $parseErrors = $null
        [void][System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$parseErrors)
        if (@($parseErrors).Count -gt 0) {
            $messages = @($parseErrors | ForEach-Object { $_.Message }) -join '; '
            throw ('PowerShell parser errors in {0}: {1}' -f $path, $messages)
        }
    }

    Initialize-ActiveConsoleInterop
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

function ConvertTo-PowerShellSingleQuotedLiteral {
    param([Parameter(Mandatory = $true)][string]$Value)
    return "'{0}'" -f $Value.Replace("'", "''")
}

function ConvertTo-EncodedPowerShellCommand {
    param([Parameter(Mandatory = $true)][string]$Command)
    return [Convert]::ToBase64String([System.Text.Encoding]::Unicode.GetBytes($Command))
}

function Get-InvocationMode {
    $validationModeCount = [int][bool]$ValidateDisplayOnly + [int][bool]$ValidateOnly + [int][bool]$ValidateHistory
    if ($validationModeCount -gt 1) {
        throw 'Only one of -ValidateDisplayOnly, -ValidateOnly, or -ValidateHistory can be supplied.'
    }
    if ($ValidateDisplayOnly) { return 'validate-display-only' }
    if ($ValidateOnly) { return 'validate-only' }
    if ($ValidateHistory) { return 'validate-history' }
    return 'runtime'
}

function Get-TaskExecutionLimitSeconds {
    param([Parameter(Mandatory = $true)][string]$Mode)

    if ($Mode -ne 'runtime') {
        return 300
    }

    # Includes display preflight plus a bounded allowance for setup and cleanup.
    return [Math]::Min(3900, [Math]::Max(300, $DurationSeconds + 300))
}

$NormalizedCaptureAtSeconds = [int[]](ConvertTo-NormalizedCaptureTimes -InputValues @($CaptureAtSeconds))
$Backend = $Backend.ToLowerInvariant()
$On12Device = $On12Device.ToLowerInvariant()
$ForceSwapEffect = $ForceSwapEffect.ToLowerInvariant()
$ForcePresentInterval = $ForcePresentInterval.ToLowerInvariant()
$InvocationMode = Get-InvocationMode
$BridgeGuid = [guid]::NewGuid().ToString('N')
$TaskName = '{0}{1}-{2}' -f $TaskPrefix, $PID, $BridgeGuid

Test-ControllerDependencies

if ($ValidateBridgeOnly) {
    [pscustomobject][ordered]@{
        mode                    = 'validate-bridge-only'
        taskPrefix              = $TaskPrefix
        taskName                = $TaskName
        taskRegistrationAttempted = $false
        normalizedCaptureAtSeconds = $NormalizedCaptureAtSeconds
        invocationMode          = $InvocationMode
        controllerScript        = $PSCommandPath
        workerScript            = $WorkerScriptPath
        runScript               = $RunScriptPath
        parserDependenciesValid = $true
    } | ConvertTo-Json -Depth 6
    exit 0
}

if (($InvocationMode -eq 'runtime') -and (-not $AllowActiveDesktopLaunch)) {
    throw 'A runtime probe can launch Bully.exe on the active physical console desktop. Re-run with -AllowActiveDesktopLaunch after confirming that desktop is safe to use. Validation modes do not require this switch.'
}

$ControllerSid = Get-CurrentSid
$ActiveSessionId = Get-ActiveConsoleSessionId
$ActiveConsoleUser = Get-ActiveConsoleUserRecord -SessionId $ActiveSessionId
$ExplorerIdentities = @(Assert-ActiveConsoleOwnership -SessionId $ActiveSessionId -ExpectedSid $ControllerSid)
$BridgeDirectory = Join-Path $ProjectRoot (Join-Path 'dump\render-probe\bridge' $BridgeGuid)
$RequestPath = Join-Path $BridgeDirectory 'request.json'
$ResultPath = Join-Path $BridgeDirectory 'result.json'
$ControllerResultPath = Join-Path $BridgeDirectory 'controller.json'
$TaskExecutionLimitSeconds = Get-TaskExecutionLimitSeconds -Mode $InvocationMode
$ParentWaitLimitSeconds = $TaskExecutionLimitSeconds + 60
# The disabled trigger never starts a task. Its end boundary gives Task Scheduler
# a stale-task expiry path if this controller is forcibly interrupted after task
# registration; explicit finally cleanup remains the normal path.
$TaskExpiryAt = (Get-Date).AddSeconds($TaskExecutionLimitSeconds + 3600)
$TaskDeleteExpiredAfter = New-TimeSpan -Hours 1

$taskRegistered = $false
$taskStarted = $false
$taskUnregistered = $false
$cleanupErrors = New-Object System.Collections.ArrayList
$childResult = $null
$controllerError = $null
$parentExitCode = 1

try {
    New-Item -ItemType Directory -Path $BridgeDirectory -Force | Out-Null

    $request = [ordered]@{
        schemaVersion      = 1
        bridgeGuid         = $BridgeGuid
        taskName           = $TaskName
        createdUtc         = (Get-Date).ToUniversalTime().ToString('o')
        expectedSessionId  = $ActiveSessionId
        expectedSid        = $ControllerSid
        activeConsoleUser  = $ActiveConsoleUser.userName
        activeConsoleConnectState = $ActiveConsoleUser.connectState
        runScriptPath      = $RunScriptPath
        artifactDirectory  = $BridgeDirectory
        invocationMode     = $InvocationMode
        parameters         = [ordered]@{
            backend              = $Backend
            durationSeconds      = $DurationSeconds
            captureAtSeconds     = @($NormalizedCaptureAtSeconds | ForEach-Object { [string]$_ })
            noInstall            = [bool]$NoInstall
            forceInstall         = [bool]$ForceInstall
            allowVirtualDisplay  = [bool]$AllowVirtualDisplay
            on12Device           = $On12Device
            forceSwapEffect      = $ForceSwapEffect
            forcePresentInterval = $ForcePresentInterval
        }
    }
    Write-AtomicJson -Path $RequestPath -Object $request -Depth 8

    $powershellExe = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
    if (-not (Test-Path -LiteralPath $powershellExe -PathType Leaf)) {
        throw ('Windows PowerShell executable was not found at {0}.' -f $powershellExe)
    }

    $workerCommand = '$ProgressPreference = ''SilentlyContinue''; & {0} -RequestPath {1}' -f `
        (ConvertTo-PowerShellSingleQuotedLiteral -Value $WorkerScriptPath), `
        (ConvertTo-PowerShellSingleQuotedLiteral -Value $RequestPath)
    $encodedWorkerCommand = ConvertTo-EncodedPowerShellCommand -Command $workerCommand
    $taskAction = New-ScheduledTaskAction -Execute $powershellExe -Argument ('-NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand {0}' -f $encodedWorkerCommand) -WorkingDirectory $ScriptDirectory
    $taskPrincipal = New-ScheduledTaskPrincipal -UserId $ControllerSid -LogonType Interactive -RunLevel Highest
    $expiryTrigger = New-ScheduledTaskTrigger -Once -At $TaskExpiryAt
    $expiryTrigger.Enabled = $false
    $expiryTrigger.EndBoundary = $TaskExpiryAt.AddSeconds(1).ToString('s')
    $taskSettings = New-ScheduledTaskSettingsSet `
        -ExecutionTimeLimit (New-TimeSpan -Seconds $TaskExecutionLimitSeconds) `
        -DeleteExpiredTaskAfter $TaskDeleteExpiredAfter `
        -AllowStartIfOnBatteries `
        -DontStopIfGoingOnBatteries `
        -MultipleInstances IgnoreNew

    Register-ScheduledTask `
        -TaskName $TaskName `
        -Action $taskAction `
        -Principal $taskPrincipal `
        -Settings $taskSettings `
        -Trigger $expiryTrigger `
        -Description ('Bully active-console render-probe bridge {0}; disabled expiry trigger; manual cleanup prefix: {1}' -f $BridgeGuid, $TaskPrefix) `
        -Force | Out-Null
    $taskRegistered = $true

    Start-ScheduledTask -TaskName $TaskName -ErrorAction Stop
    $taskStarted = $true

    $deadline = (Get-Date).AddSeconds($ParentWaitLimitSeconds)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path -LiteralPath $ResultPath -PathType Leaf) {
            try {
                $childResult = Get-Content -LiteralPath $ResultPath -Raw | ConvertFrom-Json -ErrorAction Stop
                break
            }
            catch {
                # The worker publishes with a same-directory rename. A retry is
                # retained for antivirus/file-indexing races after the rename.
            }
        }
        Start-Sleep -Milliseconds 250
    }

    if ($null -eq $childResult) {
        throw ('Timed out after {0} seconds waiting for active-console task {1} to publish {2}.' -f $ParentWaitLimitSeconds, $TaskName, $ResultPath)
    }
    if (($childResult.bridgeGuid -ne $BridgeGuid) -or ($childResult.taskName -ne $TaskName)) {
        throw ('Active-console task {0} published a result that does not match this bridge invocation.' -f $TaskName)
    }
    if ($null -eq $childResult.childExitCode) {
        throw ('Active-console task {0} did not report a child exit code.' -f $TaskName)
    }

    $childStdoutPath = [string]$childResult.childStdoutPath
    $childStderrPath = [string]$childResult.childStderrPath
    if (-not [string]::IsNullOrWhiteSpace($childStdoutPath) -and (Test-Path -LiteralPath $childStdoutPath -PathType Leaf)) {
        $childStdout = [System.IO.File]::ReadAllText($childStdoutPath)
        if (-not [string]::IsNullOrEmpty($childStdout)) {
            [Console]::Out.Write($childStdout)
            if (-not $childStdout.EndsWith([Environment]::NewLine)) {
                [Console]::Out.WriteLine()
            }
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($childStderrPath) -and (Test-Path -LiteralPath $childStderrPath -PathType Leaf)) {
        $childStderr = [System.IO.File]::ReadAllText($childStderrPath)
        if (-not [string]::IsNullOrEmpty($childStderr)) {
            [Console]::Error.Write($childStderr)
            if (-not $childStderr.EndsWith([Environment]::NewLine)) {
                [Console]::Error.WriteLine()
            }
        }
    }

    [pscustomobject][ordered]@{
        mode             = 'active-console-bridge'
        bridgeGuid       = $BridgeGuid
        taskName         = $TaskName
        sessionId        = $ActiveSessionId
        controllerSid    = $ControllerSid
        invocationMode   = $InvocationMode
        reportPath       = $childResult.reportPath
        resultPath       = $ResultPath
        artifactDirectory = $BridgeDirectory
        childExitCode    = [int]$childResult.childExitCode
    } | ConvertTo-Json -Compress | Write-Output

    $parentExitCode = [int]$childResult.childExitCode
}
catch {
    $controllerError = $_.Exception.Message
    [Console]::Error.WriteLine(('run_active.ps1 failed: {0}' -f $controllerError))
    $parentExitCode = 1
}
finally {
    if ($taskRegistered) {
        try {
            $scheduledTask = Get-ScheduledTask -TaskName $TaskName -ErrorAction Stop
            if ($scheduledTask.State -eq 'Running') {
                Stop-ScheduledTask -TaskName $TaskName -ErrorAction Stop
            }
        }
        catch {
            [void]$cleanupErrors.Add(('Could not stop exact task {0}: {1}' -f $TaskName, $_.Exception.Message))
        }

        try {
            Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction Stop
            $taskUnregistered = $true
        }
        catch {
            [void]$cleanupErrors.Add(('Could not unregister exact task {0}: {1}' -f $TaskName, $_.Exception.Message))
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($BridgeDirectory) -and (Test-Path -LiteralPath $BridgeDirectory -PathType Container)) {
        try {
            Write-AtomicJson -Path $ControllerResultPath -Object ([ordered]@{
                schemaVersion       = 1
                bridgeGuid          = $BridgeGuid
                taskName            = $TaskName
                taskPrefix          = $TaskPrefix
                expectedSessionId   = $ActiveSessionId
                controllerSid       = $ControllerSid
                activeConsoleUser   = $ActiveConsoleUser
                explorerIdentities  = @($ExplorerIdentities)
                invocationMode      = $InvocationMode
                requestPath         = $RequestPath
                resultPath          = $ResultPath
                taskRegistered      = $taskRegistered
                taskStarted         = $taskStarted
                taskUnregistered    = $taskUnregistered
                cleanupErrors       = @($cleanupErrors)
                childExitCode       = if ($null -ne $childResult) { $childResult.childExitCode } else { $null }
                controllerExitCode  = $parentExitCode
                error               = $controllerError
                finishedUtc         = (Get-Date).ToUniversalTime().ToString('o')
            }) -Depth 10
        }
        catch {
            [Console]::Error.WriteLine(('run_active.ps1 could not write controller evidence: {0}' -f $_.Exception.Message))
        }
    }
}

exit $parentExitCode
