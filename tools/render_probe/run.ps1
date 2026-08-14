# Requires Windows PowerShell 5.1+ or PowerShell 7+ on Windows.
# This launcher intentionally has no side effects until it is invoked.
[CmdletBinding()]
param(
    [ValidateSet('on12', 'native')]
    [string]$Backend = 'on12',

    [ValidateSet('internal', 'explicit')]
    [string]$On12Device = 'internal',

    [ValidateSet('none', 'discard', 'flip', 'copy')]
    [string]$ForceSwapEffect = 'none',

    [ValidateSet('none', 'immediate', 'default', 'one')]
    [string]$ForcePresentInterval = 'none',

    [ValidateRange(1, 3600)]
    [int]$DurationSeconds = 40,

    # Keep this as strings: native powershell.exe -File otherwise coerces
    # "5,15,30" through the current culture before the script can split it.
    [string[]]$CaptureAtSeconds = @('5', '15', '30'),

    [switch]$NoInstall,

    [switch]$ForceInstall,

    # Normalizes parameters and exits before creating a run directory, staging
    # files, querying displays, or launching the game.
    [switch]$ValidateOnly,

    # Exercises only the proxy hash-history collection and exits before any
    # runtime directory, display query, file staging, or game launch.
    [switch]$ValidateHistory,

    # Runs the no-mutation host display/capture preflight and prints JSON. It
    # never stages files, creates a runtime directory, or starts Bully.exe.
    [switch]$ValidateDisplayOnly,

    # Generates and validates the requested renderer INI against a temporary
    # fixture under tools/render_probe/, without touching the game or display.
    [switch]$ValidateIniOnly,

    # Allows a runtime probe to continue after a failed display preflight.
    # Screen captures remain explicitly untrusted in the final report.
    [switch]$AllowVirtualDisplay,

    # Validation-only fixture override, constrained below to tools/render_probe/.
    [string]$HistoryValidationPath,

    [ValidatePattern('^[0-9a-fA-F]{64}$')]
    [string]$HistoryTestHash = 'ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff'
)

$ErrorActionPreference = 'Stop'

# ValidateSet is case-insensitive. Normalize accepted input so reports, staged
# INI files, and validation output use stable values for reproducible A/B runs.
$Backend = $Backend.ToLowerInvariant()
$On12Device = $On12Device.ToLowerInvariant()
$ForceSwapEffect = $ForceSwapEffect.ToLowerInvariant()
$ForcePresentInterval = $ForcePresentInterval.ToLowerInvariant()

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

$RawCaptureAtSeconds = @($CaptureAtSeconds)
$NormalizedCaptureAtSeconds = [int[]](ConvertTo-NormalizedCaptureTimes -InputValues $RawCaptureAtSeconds)

if ($ValidateOnly) {
    [pscustomobject][ordered]@{
        captureAtSeconds      = $NormalizedCaptureAtSeconds
        presentationControls  = [ordered]@{
            backend              = $Backend
            on12Device           = $On12Device
            forceSwapEffect      = $ForceSwapEffect
            forcePresentInterval = $ForcePresentInterval
        }
    } | ConvertTo-Json -Compress
    exit 0
}

if (([int][bool]$ValidateHistory + [int][bool]$ValidateDisplayOnly + [int][bool]$ValidateIniOnly) -gt 1) {
    throw 'Only one of -ValidateHistory, -ValidateDisplayOnly, or -ValidateIniOnly can be used at a time.'
}

if (($ValidateDisplayOnly -or $ValidateIniOnly) -and -not [string]::IsNullOrWhiteSpace($HistoryValidationPath)) {
    throw '-HistoryValidationPath is only available with -ValidateHistory.'
}

# Visible capture-analysis thresholds. Adjust only when a known renderer state
# consistently falls on the wrong side of a classification boundary.
$Thresholds = [ordered]@{
    nearWhiteChannel              = 245
    nearBlackChannel              = 10
    blankPixelRatio               = 0.995
    uniformLuminanceStdDev        = 2.0
    uniformCoarseColorBins        = 2
    lowInformationLuminanceStdDev = 8.0
    lowInformationCoarseColorBins = 8
    coarseColorBinsPerChannel     = 8
}

$KnownFailureSignatures = @(
    [ordered]@{
        application   = 'Bully.exe'
        exceptionCode = '0xc0000005'
        faultOffset   = '0x3487DB'
        description   = 'Known access violation evidence: Bully.exe+0x3487DB.'
    }
)

$ScriptDirectory = $PSScriptRoot
$ProjectRoot = (Resolve-Path -LiteralPath (Join-Path $ScriptDirectory '..\..')).Path
$GameDirectory = Join-Path $ProjectRoot 'Bully Scholarship Edition'
$GameExePath = Join-Path $GameDirectory 'Bully.exe'
$GameD3D9Path = Join-Path $GameDirectory 'd3d9.dll'
$GameIniPath = Join-Path $GameDirectory 'bully_d3d9proxy.ini'
$ProxyLogPath = Join-Path $GameDirectory 'bully_d3d9proxy.log'
$BackbufferBmpPath = Join-Path $GameDirectory 'bully_renderprobe_backbuffer.bmp'
$FrontbufferBmpPath = Join-Path $GameDirectory 'bully_renderprobe_frontbuffer.bmp'
$GeneratedProxyPath = Join-Path $ProjectRoot 'build\proxy\Release\d3d9.dll'
$SourceIniPath = Join-Path $ProjectRoot 'src\proxy\bully_d3d9proxy.ini'
$NativeMethodsSourcePath = Join-Path $ScriptDirectory 'NativeMethods.cs'
$RealHashHistoryPath = Join-Path $ScriptDirectory 'proxy-hash-history.json'
$HashHistoryPath = $RealHashHistoryPath
$TemporaryHistoryValidationPath = $null
$RequestedPresentationControls = [ordered]@{
    backend              = $Backend
    on12Device           = $On12Device
    forceSwapEffect      = $ForceSwapEffect
    forcePresentInterval = $ForcePresentInterval
}
$RequestedRendererIniValues = [ordered]@{
    backend                = $Backend
    on12_device            = $On12Device
    force_swap_effect      = $ForceSwapEffect
    force_present_interval = $ForcePresentInterval
}

if ($ValidateHistory) {
    if ([string]::IsNullOrWhiteSpace($HistoryValidationPath)) {
        # The default validation target is isolated and removed in the
        # validation finally block. It must never append test data to the real
        # project-generated proxy allowlist.
        $TemporaryHistoryValidationPath = Join-Path $ScriptDirectory ('.history-validation-{0}.json' -f [System.Guid]::NewGuid().ToString('N'))
        $HashHistoryPath = $TemporaryHistoryValidationPath
    }
    else {
    $normalizedScriptDirectory = [System.IO.Path]::GetFullPath($ScriptDirectory).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    $candidateHistoryPath = [System.IO.Path]::GetFullPath($HistoryValidationPath)
    if (-not $candidateHistoryPath.StartsWith($normalizedScriptDirectory, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw '-HistoryValidationPath must be inside tools/render_probe/.'
    }
        if ([string]::Equals($candidateHistoryPath, [System.IO.Path]::GetFullPath($RealHashHistoryPath), [System.StringComparison]::OrdinalIgnoreCase)) {
            throw '-HistoryValidationPath must be a fixture, not proxy-hash-history.json.'
        }
    $HashHistoryPath = $candidateHistoryPath
    }
}
elseif (-not [string]::IsNullOrWhiteSpace($HistoryValidationPath)) {
    throw '-HistoryValidationPath is only available with -ValidateHistory.'
}

$RunStart = Get-Date
$On12DeviceRunMetadata = if ($On12Device -eq 'explicit') { 'od-e' } else { 'od-i' }
$PresentationRunMetadata = 'se-{0}_pi-{1}_{2}' -f $ForceSwapEffect, $ForcePresentInterval, $On12DeviceRunMetadata
$RunId = '{0}-pid{1}-{2}-{3}' -f $RunStart.ToString('yyyyMMdd-HHmmss'), $PID, $Backend, $PresentationRunMetadata
$RunDirectory = Join-Path $ProjectRoot (Join-Path 'dump\render-probe' $RunId)
$CaptureDirectory = Join-Path $RunDirectory 'captures'
$ArtifactDirectory = Join-Path $RunDirectory 'artifacts'
$StateDirectory = Join-Path $RunDirectory 'state'
$RunLogPath = Join-Path $RunDirectory 'run.log'
$ReportPath = Join-Path $RunDirectory 'report.json'
$SummaryPath = Join-Path $RunDirectory 'summary.txt'
$WerPath = Join-Path $RunDirectory 'wer-application-errors.json'
$Utf8NoBom = [System.Text.UTF8Encoding]::new($false)

if ((-not $ValidateHistory) -and (-not $ValidateDisplayOnly) -and (-not $ValidateIniOnly)) {
    New-Item -ItemType Directory -Path $CaptureDirectory -Force | Out-Null
    New-Item -ItemType Directory -Path $ArtifactDirectory -Force | Out-Null
    New-Item -ItemType Directory -Path $StateDirectory -Force | Out-Null
}

$script:RunErrors = New-Object System.Collections.ArrayList
$script:RunWarnings = New-Object System.Collections.ArrayList
$script:Activity = New-Object System.Collections.ArrayList
$script:CaptureRecords = New-Object System.Collections.ArrayList
$script:Observations = New-Object System.Collections.ArrayList
$script:ProxyHashHistoryEntries = New-Object 'System.Collections.Generic.List[object]'
$script:WerRecords = @()
$script:BackbufferAnalysis = $null
$script:FrontbufferAnalysis = $null
$script:FatalError = $null
$script:GameProcess = $null
$script:RunStartedProcess = $false
$script:RunEnd = $null
$script:ExitCode = 1
$script:PreflightBlocked = $false
$script:PreflightOverridden = $false

$script:DllInstallState = [ordered]@{
    previousExisted = $false
    previousHash    = $null
    backupPath      = $null
    installAttempted = $false
    installedHash   = $null
    changedByProbe  = $false
    restoreStatus   = 'not-required'
}

$script:IniInstallState = [ordered]@{
    previousExisted = $false
    previousHash    = $null
    backupPath      = $null
    installAttempted = $false
    installedHash   = $null
    changedByProbe  = $false
    restoreStatus   = 'not-required'
}

$script:Installation = [ordered]@{
    attempted                 = $false
    status                    = 'not-started'
    noInstall                 = [bool]$NoInstall
    forceInstall              = [bool]$ForceInstall
    generatedProxyPath        = $GeneratedProxyPath
    generatedProxySha256      = $null
    sourceIniPath             = $SourceIniPath
    sourceIniSha256           = $null
    gameD3D9BeforeSha256      = $null
    gameIniBeforeSha256       = $null
    gameD3D9AfterCleanupSha256 = $null
    gameIniAfterCleanupSha256  = $null
    trustedExistingProxy       = $null
    knownProxyHashCountBefore = 0
    rendererOverrides          = [ordered]@{
        requested       = $RequestedPresentationControls
        requestedIni    = [ordered]@{
            path       = $null
            sha256     = $null
            effective  = $null
            validation = $null
        }
        activeIniStatus    = 'not-applied'
        activeIniEffective = $null
    }
    d3d9                      = $script:DllInstallState
    ini                       = $script:IniInstallState
}

$script:Display = [ordered]@{
    dpiAwareness                 = $null
    allowVirtualDisplay          = [bool]$AllowVirtualDisplay
    preflightOverrideUsed        = $false
    sessionName                  = $null
    remoteSession                = $null
    screenDeviceNames            = @()
    preflight                    = $null
    screenCapturesTrustworthy    = $null
    beforeLaunch                 = $null
    afterTermination             = $null
    restoreDecision              = 'not-evaluated'
    restoreAttempted             = $false
    restoreResult                = $null
    afterCleanup                 = $null
}

$script:ProcessInfo = [ordered]@{
    launched                 = $false
    processId                = $null
    executable               = $GameExePath
    workingDirectory         = $GameDirectory
    launchUtc                = $null
    exitedBeforeTimeout      = $false
    survivedToTimeout        = $false
    terminatedByHarness      = $false
    terminationReason        = $null
    exitUtc                  = $null
    exitCode                 = $null
    faultOffsets             = @()
    knownFailureEvidenceSeen = $false
}

$script:Artifacts = [ordered]@{
    proxyLog              = $null
    proxyLogFull          = $null
    activeIni             = $null
    proxyBackbufferBmp    = $null
    proxyFrontbufferBmp   = $null
    preRun                = $null
    requestedIni          = $null
    originalD3D9Backup    = $null
    originalIniBackup     = $null
    werApplicationErrors  = 'wer-application-errors.json'
}

function Add-RunLog {
    param(
        [Parameter(Mandatory = $true)][string]$Level,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $entry = [ordered]@{
        utc     = (Get-Date).ToUniversalTime().ToString('o')
        level   = $Level
        message = $Message
    }
    [void]$script:Activity.Add([pscustomobject]$entry)
    $line = '{0} [{1}] {2}' -f $entry.utc, $Level.ToUpperInvariant(), $Message
    try {
        [System.IO.File]::AppendAllText($RunLogPath, $line + [Environment]::NewLine, $Utf8NoBom)
    }
    catch {
        # The final report remains the primary record if the text log cannot be written.
    }
    Write-Host $line
}

function Add-RunError {
    param([Parameter(Mandatory = $true)][string]$Message)
    [void]$script:RunErrors.Add($Message)
    Add-RunLog -Level 'error' -Message $Message
}

function Add-RunWarning {
    param([Parameter(Mandatory = $true)][string]$Message)
    [void]$script:RunWarnings.Add($Message)
    Add-RunLog -Level 'warning' -Message $Message
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $null
    }

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-FileSnapshot {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return [ordered]@{
            path         = $Path
            exists       = $false
            length       = $null
            lastWriteUtc = $null
            sha256       = $null
        }
    }

    $item = Get-Item -LiteralPath $Path
    return [ordered]@{
        path         = $Path
        exists       = $true
        length       = [long]$item.Length
        lastWriteUtc = $item.LastWriteTimeUtc.ToString('o')
        sha256       = Get-Sha256 -Path $Path
    }
}

function Test-FileChangedSinceSnapshot {
    param(
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$Before,
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$After
    )

    if ([bool]$Before['exists'] -ne [bool]$After['exists']) {
        return $true
    }
    if (-not [bool]$After['exists']) {
        return $false
    }
    return ($Before['sha256'] -ne $After['sha256']) -or ($Before['length'] -ne $After['length']) -or
        ($Before['lastWriteUtc'] -ne $After['lastWriteUtc'])
}

function Get-FilePrefixSha256 {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][long]$Length
    )

    if ($Length -lt 0) {
        throw 'Prefix length cannot be negative.'
    }
    $stream = $null
    $hashAlgorithm = $null
    try {
        if ($Length -eq 0) {
            $hashAlgorithm = [System.Security.Cryptography.SHA256]::Create()
            $empty = New-Object byte[] 0
            [void]$hashAlgorithm.TransformFinalBlock($empty, 0, 0)
            return ([System.BitConverter]::ToString($hashAlgorithm.Hash) -replace '-', '').ToLowerInvariant()
        }

        $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        if ($stream.Length -lt $Length) {
            return $null
        }
        $hashAlgorithm = [System.Security.Cryptography.SHA256]::Create()
        $buffer = New-Object byte[] 65536
        $remaining = $Length
        while ($remaining -gt 0) {
            $requested = [int][Math]::Min([long]$buffer.Length, $remaining)
            $read = $stream.Read($buffer, 0, $requested)
            if ($read -le 0) {
                return $null
            }
            [void]$hashAlgorithm.TransformBlock($buffer, 0, $read, $buffer, 0)
            $remaining -= $read
        }
        [void]$hashAlgorithm.TransformFinalBlock($buffer, 0, 0)
        return ([System.BitConverter]::ToString($hashAlgorithm.Hash) -replace '-', '').ToLowerInvariant()
    }
    finally {
        if ($null -ne $hashAlgorithm) { $hashAlgorithm.Dispose() }
        if ($null -ne $stream) { $stream.Dispose() }
    }
}

function Copy-FileByteRange {
    param(
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][long]$Offset,
        [Parameter(Mandatory = $true)][long]$Length,
        [Parameter(Mandatory = $true)][string]$DestinationPath
    )

    if ($Offset -lt 0 -or $Length -lt 0) {
        throw 'Byte-range offset and length must be non-negative.'
    }

    $source = $null
    $destination = $null
    try {
        $source = [System.IO.File]::Open($SourcePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        if ($source.Length -lt ($Offset + $Length)) {
            throw ('Source file does not contain requested byte range {0}+{1}.' -f $Offset, $Length)
        }
        $source.Seek($Offset, [System.IO.SeekOrigin]::Begin) | Out-Null
        $destination = [System.IO.File]::Open($DestinationPath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
        $buffer = New-Object byte[] 65536
        $remaining = $Length
        while ($remaining -gt 0) {
            $requested = [int][Math]::Min([long]$buffer.Length, $remaining)
            $read = $source.Read($buffer, 0, $requested)
            if ($read -le 0) {
                throw 'Unexpected end of file while copying byte range.'
            }
            $destination.Write($buffer, 0, $read)
            $remaining -= $read
        }
    }
    finally {
        if ($null -ne $destination) { $destination.Dispose() }
        if ($null -ne $source) { $source.Dispose() }
    }
}

function Get-RectRecord {
    param([Parameter(Mandatory = $true)][object]$Rectangle)

    return [ordered]@{
        left   = [int]$Rectangle.Left
        top    = [int]$Rectangle.Top
        right  = [int]$Rectangle.Right
        bottom = [int]$Rectangle.Bottom
        width  = [int]$Rectangle.Width
        height = [int]$Rectangle.Height
    }
}

function Get-DisplayScreenRecords {
    $records = New-Object System.Collections.ArrayList
    $errorMessage = $null
    try {
        Add-Type -AssemblyName System.Windows.Forms -ErrorAction Stop
        foreach ($screen in @([System.Windows.Forms.Screen]::AllScreens)) {
            [void]$records.Add([pscustomobject][ordered]@{
                deviceName  = [string]$screen.DeviceName
                primary     = [bool]$screen.Primary
                bounds      = Get-RectRecord -Rectangle $screen.Bounds
                workingArea = Get-RectRecord -Rectangle $screen.WorkingArea
            })
        }
    }
    catch {
        $errorMessage = $_.Exception.Message
    }

    return [ordered]@{
        succeeded = ($null -eq $errorMessage)
        screens   = @($records)
        error     = $errorMessage
    }
}

function Get-DisplayMonitorRecords {
    $records = New-Object System.Collections.ArrayList
    $errorMessage = $null
    try {
        foreach ($monitor in @([RenderProbe.NativeMethods]::GetDisplayMonitors())) {
            [void]$records.Add([pscustomobject][ordered]@{
                deviceName  = [string]$monitor.DeviceName
                primary     = [bool]$monitor.IsPrimary
                bounds      = Get-RectRecord -Rectangle $monitor.Bounds
                workingArea = Get-RectRecord -Rectangle $monitor.WorkingArea
            })
        }
    }
    catch {
        $errorMessage = $_.Exception.Message
    }

    return [ordered]@{
        succeeded = ($null -eq $errorMessage)
        monitors  = @($records)
        error     = $errorMessage
    }
}

function Get-DisplayDeviceRecords {
    $records = New-Object System.Collections.ArrayList
    $errorMessage = $null
    try {
        foreach ($device in @([RenderProbe.NativeMethods]::GetDisplayDevices())) {
            [void]$records.Add([pscustomobject][ordered]@{
                deviceName   = [string]$device.DeviceName
                deviceString = [string]$device.DeviceString
                stateFlags   = [int]$device.StateFlags
            })
        }
    }
    catch {
        $errorMessage = $_.Exception.Message
    }

    return [ordered]@{
        succeeded = ($null -eq $errorMessage)
        devices   = @($records)
        error     = $errorMessage
    }
}

function Test-VirtualOrDisconnectedDisplayName {
    param([AllowNull()][string]$Name)

    if ([string]::IsNullOrWhiteSpace($Name)) {
        return $false
    }

    return $Name -match '(?i)(?:windisc|disconnected|virtual|indirect|remote|rdp|mirage|dummy)'
}

function Test-RemoteSessionName {
    param([AllowNull()][string]$SessionName)

    if ([string]::IsNullOrWhiteSpace($SessionName)) {
        return $false
    }

    return $SessionName -match '(?i)^(?:rdp-|ica-|remote|shadow)'
}

function New-DisplayPreflightCheck {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][bool]$Passed,
        [Parameter(Mandatory = $true)][string]$Description,
        [AllowNull()][string]$Error
    )

    return [pscustomobject][ordered]@{
        name        = $Name
        passed      = $Passed
        status      = if ($Passed) { 'passed' } else { 'failed' }
        description = $Description
        error       = $Error
    }
}

function Get-PrimaryDisplaySnapshot {
    $recordedUtc = (Get-Date).ToUniversalTime().ToString('o')
    $screenResult = Get-DisplayScreenRecords
    $monitorResult = Get-DisplayMonitorRecords
    $primaryScreen = @($screenResult.screens | Where-Object { $_.primary } | Select-Object -First 1)
    $primaryMonitor = @($monitorResult.monitors | Where-Object { $_.primary } | Select-Object -First 1)
    $primary = if ($primaryScreen.Count -gt 0) { $primaryScreen[0] } elseif ($primaryMonitor.Count -gt 0) { $primaryMonitor[0] } else { $null }
    $widthMetric = [RenderProbe.NativeMethods]::TryGetSystemMetric(0)
    $heightMetric = [RenderProbe.NativeMethods]::TryGetSystemMetric(1)
    $mode = $null
    $modeError = $null
    if ($null -ne $primary) {
        try {
            $nativeMode = [RenderProbe.NativeMethods]::GetCurrentDisplayMode([string]$primary.deviceName)
            $mode = [ordered]@{
                deviceName         = [string]$nativeMode.DeviceName
                positionX          = [int]$nativeMode.PositionX
                positionY          = [int]$nativeMode.PositionY
                width              = [int]$nativeMode.Width
                height             = [int]$nativeMode.Height
                bitsPerPixel       = [int]$nativeMode.BitsPerPixel
                displayFrequency   = [int]$nativeMode.DisplayFrequency
                displayOrientation = [int]$nativeMode.DisplayOrientation
                displayFlags       = [int]$nativeMode.DisplayFlags
            }
        }
        catch {
            $modeError = $_.Exception.Message
        }
    }

    if ($null -eq $primary) {
        $errors = @($screenResult.error, $monitorResult.error | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        return [ordered]@{
            available           = $false
            deviceName          = $null
            bounds              = $null
            systemPrimaryWidth  = if ($widthMetric.Available) { [int]$widthMetric.Value } else { $null }
            systemPrimaryHeight = if ($heightMetric.Available) { [int]$heightMetric.Value } else { $null }
            screens             = @($screenResult.screens)
            monitors            = @($monitorResult.monitors)
            mode                = $mode
            modeError           = $modeError
            error               = if ($errors.Count -gt 0) { $errors -join '; ' } else { 'No primary display was enumerated.' }
            recordedUtc         = $recordedUtc
        }
    }

    return [ordered]@{
        available           = $true
        deviceName          = $primary.deviceName
        bounds              = $primary.bounds
        systemPrimaryWidth  = if ($widthMetric.Available) { [int]$widthMetric.Value } else { $null }
        systemPrimaryHeight = if ($heightMetric.Available) { [int]$heightMetric.Value } else { $null }
        screens             = @($screenResult.screens)
        monitors            = @($monitorResult.monitors)
        mode                = $mode
        modeError           = $modeError
        error               = $null
        recordedUtc         = $recordedUtc
    }
}

function Test-DisplayGeometryUnchanged {
    param(
        [AllowNull()][object]$Before,
        [AllowNull()][object]$After
    )

    if (($null -eq $Before) -or ($null -eq $After) -or (-not [bool]$Before.available) -or (-not [bool]$After.available) -or
        ($null -eq $Before.bounds) -or ($null -eq $After.bounds)) {
        return $false
    }

    if (-not [string]::Equals([string]$Before.deviceName, [string]$After.deviceName, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $false
    }

    foreach ($property in @('left', 'top', 'right', 'bottom', 'width', 'height')) {
        if ([int]$Before.bounds.$property -ne [int]$After.bounds.$property) {
            return $false
        }
    }

    if (($null -ne $Before.mode) -and ($null -ne $After.mode)) {
        foreach ($property in @('positionX', 'positionY', 'width', 'height', 'bitsPerPixel', 'displayFrequency', 'displayOrientation', 'displayFlags')) {
            if ([int]$Before.mode.$property -ne [int]$After.mode.$property) {
                return $false
            }
        }
    }
    return $true
}

function Record-AfterCleanupDisplayState {
    try {
        $script:Display['afterCleanup'] = Get-PrimaryDisplaySnapshot
    }
    catch {
        Add-RunWarning -Message ('Could not record primary display state after cleanup. {0}' -f $_.Exception.Message)
    }
}

function Invoke-DisplayPreflight {
    $sessionName = [string]$env:SESSIONNAME
    $checks = New-Object System.Collections.ArrayList
    $errors = New-Object System.Collections.ArrayList
    $failedCapabilities = New-Object System.Collections.ArrayList

    $userInteractive = [Environment]::UserInteractive
    $interactiveDesktop = [RenderProbe.NativeMethods]::GetInteractiveDesktopInfo()
    $interactivePassed = $userInteractive -and $interactiveDesktop.Available
    $interactiveError = if (-not $userInteractive) {
        'Environment.UserInteractive is false.'
    }
    elseif (-not $interactiveDesktop.Available) {
        if (-not [string]::IsNullOrWhiteSpace([string]$interactiveDesktop.Error)) { $interactiveDesktop.Error } else { 'OpenInputDesktop failed.' }
    }
    else {
        $null
    }
    [void]$checks.Add((New-DisplayPreflightCheck -Name 'interactive-desktop' -Passed $interactivePassed -Description 'Requires a user-interactive input desktop.' -Error $interactiveError))

    $remoteMetric = [RenderProbe.NativeMethods]::TryGetSystemMetric(4096)
    $remoteByMetric = $remoteMetric.Available -and ($remoteMetric.Value -ne 0)
    $remoteBySessionName = Test-RemoteSessionName -SessionName $sessionName
    $remoteSession = $remoteByMetric -or $remoteBySessionName
    $remoteError = if ($remoteSession) {
        'Remote-session indicator detected.'
    }
    elseif (-not $remoteMetric.Available) {
        'SM_REMOTESESSION was unavailable: {0}' -f $remoteMetric.Error
    }
    else {
        $null
    }
    [void]$checks.Add((New-DisplayPreflightCheck -Name 'remote-session' -Passed (-not $remoteSession) -Description 'Rejects RDP/remote sessions from SM_REMOTESESSION or SESSIONNAME.' -Error $remoteError))

    $screenResult = Get-DisplayScreenRecords
    $monitorResult = Get-DisplayMonitorRecords
    $deviceResult = Get-DisplayDeviceRecords
    $primaryScreen = @($screenResult.screens | Where-Object { $_.primary } | Select-Object -First 1)
    $primaryMonitor = @($monitorResult.monitors | Where-Object { $_.primary } | Select-Object -First 1)
    $primary = if ($primaryScreen.Count -gt 0) { $primaryScreen[0] } elseif ($primaryMonitor.Count -gt 0) { $primaryMonitor[0] } else { $null }
    $rawScreenDeviceNames = @(
        @($screenResult.screens | ForEach-Object { $_.deviceName }) +
        @($monitorResult.monitors | ForEach-Object { $_.deviceName }) +
        @($deviceResult.devices | ForEach-Object { $_.deviceName }))
    $screenDeviceNames = @($rawScreenDeviceNames |
        Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) } |
        Sort-Object -Unique)
    $rawDisplayIdentifiers = @(
        $screenDeviceNames + @($deviceResult.devices | ForEach-Object { $_.deviceString }))
    $displayIdentifiers = @($rawDisplayIdentifiers |
        Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) } |
        Sort-Object -Unique)
    $virtualNames = @($displayIdentifiers | Where-Object { Test-VirtualOrDisconnectedDisplayName -Name ([string]$_) })
    $hasValidBounds = ($null -ne $primary) -and ($null -ne $primary.bounds) -and
        ([int]$primary.bounds.width -gt 0) -and ([int]$primary.bounds.height -gt 0)
    $displayEnumerationErrors = @($screenResult.error, $monitorResult.error, $deviceResult.error | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $displayError = if (-not $hasValidBounds) {
        if ($displayEnumerationErrors.Count -gt 0) { $displayEnumerationErrors -join '; ' } else { 'No primary screen with valid nonzero bounds was found.' }
    }
    elseif ($virtualNames.Count -gt 0) {
        'Virtual or disconnected display indicator(s): {0}' -f ($virtualNames -join ', ')
    }
    else {
        $null
    }
    $displayPassed = $hasValidBounds -and ($virtualNames.Count -eq 0)
    [void]$checks.Add((New-DisplayPreflightCheck -Name 'physical-display' -Passed $displayPassed -Description 'Requires a non-virtual primary display with valid bounds.' -Error $displayError))

    $captureProbe = $null
    if ($hasValidBounds) {
        try {
            $captureProbe = [RenderProbe.NativeMethods]::TestDesktopCapture([int]$primary.bounds.left, [int]$primary.bounds.top)
        }
        catch {
            $captureProbe = [pscustomobject][ordered]@{
                Succeeded     = $false
                InvalidHandle = $false
                ErrorCode     = $null
                Error         = $_.Exception.Message
            }
        }
    }
    else {
        $captureProbe = [pscustomobject][ordered]@{
            Succeeded     = $false
            InvalidHandle = $false
            ErrorCode     = $null
            Error         = 'Skipped because no primary display with valid bounds was available.'
        }
    }
    $captureError = if ($captureProbe.Succeeded) { $null } elseif ($captureProbe.InvalidHandle) {
        'Graphics.CopyFromScreen failed with ERROR_INVALID_HANDLE (6): {0}' -f $captureProbe.Error
    }
    else {
        [string]$captureProbe.Error
    }
    [void]$checks.Add((New-DisplayPreflightCheck -Name 'desktop-capture-1x1' -Passed ([bool]$captureProbe.Succeeded) -Description 'Requires a harmless in-memory 1x1 Graphics.CopyFromScreen capture.' -Error $captureError))

    foreach ($check in @($checks | Where-Object { -not $_.passed })) {
        [void]$failedCapabilities.Add([string]$check.name)
        if (-not [string]::IsNullOrWhiteSpace([string]$check.error)) {
            [void]$errors.Add(('{0}: {1}' -f $check.name, $check.error))
        }
    }

    $preflight = [ordered]@{
        status             = if ($failedCapabilities.Count -eq 0) { 'passed' } else { 'preflight-failed' }
        passed             = ($failedCapabilities.Count -eq 0)
        sessionName        = $sessionName
        userInteractive    = [bool]$userInteractive
        interactiveDesktop = [ordered]@{
            available = [bool]$interactiveDesktop.Available
            name      = $interactiveDesktop.Name
            errorCode = $interactiveDesktop.ErrorCode
            error     = $interactiveDesktop.Error
            nameError = $interactiveDesktop.NameError
        }
        remoteSession      = [ordered]@{
            detected              = [bool]$remoteSession
            bySystemMetric        = [bool]$remoteByMetric
            bySessionName         = [bool]$remoteBySessionName
            systemMetricAvailable = [bool]$remoteMetric.Available
            systemMetricValue     = if ($remoteMetric.Available) { [int]$remoteMetric.Value } else { $null }
            systemMetricError     = $remoteMetric.Error
        }
        screenDeviceNames  = $screenDeviceNames
        displayIdentifiers = $displayIdentifiers
        enumeration        = [ordered]@{
            screens = [ordered]@{ succeeded = [bool]$screenResult.succeeded; error = $screenResult.error }
            monitors = [ordered]@{ succeeded = [bool]$monitorResult.succeeded; error = $monitorResult.error }
            devices = [ordered]@{ succeeded = [bool]$deviceResult.succeeded; error = $deviceResult.error }
        }
        screens            = @($screenResult.screens)
        monitors           = @($monitorResult.monitors)
        displayDevices     = @($deviceResult.devices)
        primaryScreen      = $primary
        virtualIndicators  = $virtualNames
        hostCapture        = [ordered]@{
            succeeded     = [bool]$captureProbe.Succeeded
            invalidHandle = [bool]$captureProbe.InvalidHandle
            errorCode     = $captureProbe.ErrorCode
            error         = $captureProbe.Error
        }
        checks             = @($checks)
        failedCapabilities = @($failedCapabilities)
        errors             = @($errors)
    }

    $script:Display['sessionName'] = $sessionName
    $script:Display['remoteSession'] = [bool]$remoteSession
    $script:Display['screenDeviceNames'] = $screenDeviceNames
    $script:Display['preflight'] = $preflight
    return $preflight
}

function Write-JsonFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object]$Object,
        [int]$Depth = 12
    )

    $json = $Object | ConvertTo-Json -Depth $Depth
    [System.IO.File]::WriteAllText($Path, $json, $Utf8NoBom)
}

function Read-ProxyHashHistory {
    $entries = New-Object 'System.Collections.Generic.List[object]'
    if (-not (Test-Path -LiteralPath $HashHistoryPath -PathType Leaf)) {
        return ,$entries
    }

    try {
        $document = [System.IO.File]::ReadAllText($HashHistoryPath, $Utf8NoBom) | ConvertFrom-Json
        if ($null -ne $document -and $null -ne $document.entries) {
            foreach ($entry in @($document.entries)) {
                if ($null -ne $entry.sha256 -and ([string]$entry.sha256) -match '^[0-9a-fA-F]{64}$') {
                    [void]$entries.Add([pscustomobject]@{
                        sha256       = ([string]$entry.sha256).ToLowerInvariant()
                        firstSeenUtc = [string]$entry.firstSeenUtc
                        lastSeenUtc  = [string]$entry.lastSeenUtc
                        source       = [string]$entry.source
                    })
                }
            }
        }
        return ,$entries
    }
    catch {
        Add-RunWarning -Message ('Could not read proxy hash history; unrecognized existing d3d9.dll files will require -ForceInstall. {0}' -f $_.Exception.Message)
        return ,$entries
    }
}

function Set-ProxyHashHistoryEntries {
    param([AllowNull()][object]$Entries)

    $history = New-Object 'System.Collections.Generic.List[object]'
    if ($null -ne $Entries) {
        $baseObject = $Entries.PSObject.BaseObject
        if (($baseObject -is [System.Collections.IEnumerable]) -and ($baseObject -isnot [string])) {
            foreach ($entry in $baseObject) {
                [void]$history.Add($entry)
            }
        }
        else {
            [void]$history.Add($Entries)
        }
    }
    $script:ProxyHashHistoryEntries = $history
}

function Save-ProxyHashHistory {
    try {
        $document = [ordered]@{
            schemaVersion = 1
            updatedUtc    = (Get-Date).ToUniversalTime().ToString('o')
            entries       = @($script:ProxyHashHistoryEntries | Sort-Object sha256)
        }
        Write-JsonFile -Path $HashHistoryPath -Object $document -Depth 6
    }
    catch {
        Add-RunWarning -Message ('Could not persist the generated proxy hash history. {0}' -f $_.Exception.Message)
    }
}

function Add-GeneratedProxyHashToHistory {
    param([Parameter(Mandatory = $true)][string]$Hash)

    # Re-materialize before every mutation. PowerShell unwraps singleton
    # pipeline output, so this guards callers that supplied zero, one, or many
    # entries as arrays, generic lists, or a scalar PSObject.
    Set-ProxyHashHistoryEntries -Entries $script:ProxyHashHistoryEntries

    $now = (Get-Date).ToUniversalTime().ToString('o')
    $found = $false
    foreach ($entry in $script:ProxyHashHistoryEntries) {
        if ($entry.sha256 -eq $Hash) {
            $entry.lastSeenUtc = $now
            $found = $true
            break
        }
    }

    if (-not $found) {
        [void]$script:ProxyHashHistoryEntries.Add([pscustomobject]@{
            sha256       = $Hash
            firstSeenUtc = $now
            lastSeenUtc  = $now
            source       = 'build/proxy/Release/d3d9.dll'
        })
    }
    Save-ProxyHashHistory
}

if ($ValidateHistory) {
    $validationFailed = $false
    try {
        Set-ProxyHashHistoryEntries -Entries (Read-ProxyHashHistory)
        $countBefore = $script:ProxyHashHistoryEntries.Count
        Add-GeneratedProxyHashToHistory -Hash $HistoryTestHash.ToLowerInvariant()
        [pscustomobject]@{
            historyPath      = $HashHistoryPath
            countBefore      = $countBefore
            countAfter       = $script:ProxyHashHistoryEntries.Count
            appendedHash     = $HistoryTestHash.ToLowerInvariant()
            hashes            = [string[]]@($script:ProxyHashHistoryEntries | ForEach-Object { $_.sha256 })
            collectionType   = $script:ProxyHashHistoryEntries.GetType().FullName
        } | ConvertTo-Json -Compress
    }
    catch {
        $validationFailed = $true
        Write-Error ('Proxy hash-history validation failed: {0}' -f $_.Exception.Message)
    }
    finally {
        if (($null -ne $TemporaryHistoryValidationPath) -and (Test-Path -LiteralPath $TemporaryHistoryValidationPath -PathType Leaf)) {
            Remove-Item -LiteralPath $TemporaryHistoryValidationPath -Force -ErrorAction SilentlyContinue
        }
    }
    exit $(if ($validationFailed) { 1 } else { 0 })
}

function Save-OriginalGameFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$State,
        [Parameter(Mandatory = $true)][string]$BackupName
    )

    $State['previousExisted'] = Test-Path -LiteralPath $Path -PathType Leaf
    if (-not $State['previousExisted']) {
        return
    }

    $State['previousHash'] = Get-Sha256 -Path $Path
    $backupPath = Join-Path $StateDirectory $BackupName
    Copy-Item -LiteralPath $Path -Destination $backupPath -Force
    $backupHash = Get-Sha256 -Path $backupPath
    if ($backupHash -ne $State['previousHash']) {
        throw ('Backup hash mismatch for {0}; refusing to stage a replacement.' -f $Path)
    }
    $State['backupPath'] = $backupPath
}

function Replace-FileFromSource {
    param(
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$DestinationPath,
        [Parameter(Mandatory = $true)][string]$ExpectedHash,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $temporaryPath = Join-Path $GameDirectory ('.render-probe-{0}-{1}.tmp' -f $RunId, $Label)
    $replaceBackupPath = $temporaryPath + '.backup'
    try {
        Copy-Item -LiteralPath $SourcePath -Destination $temporaryPath -Force
        $temporaryHash = Get-Sha256 -Path $temporaryPath
        if ($temporaryHash -ne $ExpectedHash) {
            throw ('Staged {0} hash mismatch.' -f $Label)
        }

        if (Test-Path -LiteralPath $DestinationPath -PathType Leaf) {
            try {
                # This runtime requires a non-empty backup path. Keep it in the
                # destination directory so the replacement remains same-volume.
                [System.IO.File]::Replace($temporaryPath, $DestinationPath, $replaceBackupPath)
            }
            catch {
                Add-RunWarning -Message ('Atomic replacement for {0} was unavailable; using verified Copy-Item fallback. {1}' -f $Label, $_.Exception.Message)
                Copy-Item -LiteralPath $temporaryPath -Destination $DestinationPath -Force
                Remove-Item -LiteralPath $temporaryPath -Force -ErrorAction SilentlyContinue
            }
        }
        else {
            [System.IO.File]::Move($temporaryPath, $DestinationPath)
        }

        $installedHash = Get-Sha256 -Path $DestinationPath
        if ($installedHash -ne $ExpectedHash) {
            throw ('Installed {0} hash mismatch.' -f $Label)
        }
    }
    finally {
        if (Test-Path -LiteralPath $temporaryPath -PathType Leaf) {
            Remove-Item -LiteralPath $temporaryPath -Force -ErrorAction SilentlyContinue
        }
        if (Test-Path -LiteralPath $replaceBackupPath -PathType Leaf) {
            Remove-Item -LiteralPath $replaceBackupPath -Force -ErrorAction SilentlyContinue
        }
    }
}

function Restore-StagedGameFile {
    param(
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$State,
        [Parameter(Mandatory = $true)][string]$DestinationPath,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if (-not $State['installAttempted']) {
        return
    }

    $currentHash = Get-Sha256 -Path $DestinationPath
    if ($currentHash -ne $State['installedHash']) {
        $State['restoreStatus'] = 'skipped-current-file-did-not-match-probe-install'
        Add-RunWarning -Message ('Did not restore {0}: its current hash no longer matches the probe-installed file.' -f $Label)
        return
    }

    try {
        if ($State['previousExisted']) {
            Replace-FileFromSource -SourcePath $State['backupPath'] -DestinationPath $DestinationPath -ExpectedHash $State['previousHash'] -Label ('restore-' + $Label)
            $State['restoreStatus'] = 'restored-original'
        }
        else {
            Remove-Item -LiteralPath $DestinationPath -Force
            $State['restoreStatus'] = 'removed-probe-created-file'
        }
    }
    catch {
        $State['restoreStatus'] = 'restore-failed'
        Add-RunError -Message ('Failed to restore {0}. {1}' -f $Label, $_.Exception.Message)
    }
}

function ConvertTo-UpdatedRendererIniText {
    param(
        [Parameter(Mandatory = $true)][string]$SourceText,
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$RendererValues
    )

    $newline = if ($SourceText -match "`r`n") { "`r`n" } else { "`n" }
    $hasTerminalNewline = $SourceText.EndsWith("`r`n") -or $SourceText.EndsWith("`n")
    $lines = New-Object 'System.Collections.Generic.List[string]'
    foreach ($line in ($SourceText -split "`r?`n", 0, [System.StringSplitOptions]::None)) {
        [void]$lines.Add($line)
    }
    if ($hasTerminalNewline -and ($lines.Count -gt 0) -and ($lines[$lines.Count - 1] -eq '')) {
        $lines.RemoveAt($lines.Count - 1)
    }

    $rendererSectionStart = -1
    $rendererSectionEnd = $lines.Count
    for ($index = 0; $index -lt $lines.Count; $index++) {
        $sectionMatch = [regex]::Match($lines[$index], '^\s*\[([^\]]+)\]\s*(?:[;#].*)?$')
        if (-not $sectionMatch.Success) {
            continue
        }

        if ($rendererSectionStart -ge 0) {
            $rendererSectionEnd = $index
            break
        }
        if ([string]::Equals($sectionMatch.Groups[1].Value.Trim(), 'renderer', [System.StringComparison]::OrdinalIgnoreCase)) {
            $rendererSectionStart = $index
        }
    }

    if ($rendererSectionStart -lt 0) {
        throw 'Source renderer INI lacks a [renderer] section.'
    }

    $managedKeys = @($RendererValues.Keys | ForEach-Object { [string]$_ })
    $updatedLines = New-Object 'System.Collections.Generic.List[string]'
    for ($index = 0; $index -lt $lines.Count; $index++) {
        $line = $lines[$index]
        $insideRenderer = ($index -gt $rendererSectionStart) -and ($index -lt $rendererSectionEnd)
        if ($insideRenderer) {
            $keyMatch = [regex]::Match($line, '^\s*([^=;#\s][^=;#]*)\s*=')
            if ($keyMatch.Success) {
                $keyName = $keyMatch.Groups[1].Value.Trim()
                if ($managedKeys -contains $keyName) {
                    continue
                }
            }
        }
        [void]$updatedLines.Add($line)

        if ($index -eq $rendererSectionStart) {
            foreach ($key in $managedKeys) {
                [void]$updatedLines.Add(('{0}={1}' -f $key, $RendererValues[$key]))
            }
        }
    }

    return (($updatedLines -join $newline) + $newline)
}

function Get-RendererIniEffectiveValues {
    param(
        [Parameter(Mandatory = $true)][string]$IniText,
        [Parameter(Mandatory = $true)][string[]]$Keys
    )

    $effective = [ordered]@{}
    $counts = [ordered]@{}
    foreach ($key in $Keys) {
        $effective[$key] = $null
        $counts[$key] = 0
    }

    $inRenderer = $false
    foreach ($line in ($IniText -split "`r?`n", 0, [System.StringSplitOptions]::None)) {
        $sectionMatch = [regex]::Match($line, '^\s*\[([^\]]+)\]\s*(?:[;#].*)?$')
        if ($sectionMatch.Success) {
            $inRenderer = [string]::Equals($sectionMatch.Groups[1].Value.Trim(), 'renderer', [System.StringComparison]::OrdinalIgnoreCase)
            continue
        }
        if (-not $inRenderer) {
            continue
        }

        $keyMatch = [regex]::Match($line, '^\s*([^=;#\s][^=;#]*)\s*=\s*(.*?)\s*(?:[;#].*)?$')
        if (-not $keyMatch.Success) {
            continue
        }
        $keyName = $keyMatch.Groups[1].Value.Trim()
        foreach ($requestedKey in $Keys) {
            if ([string]::Equals($keyName, $requestedKey, [System.StringComparison]::OrdinalIgnoreCase)) {
                $effective[$requestedKey] = $keyMatch.Groups[2].Value.Trim()
                $counts[$requestedKey] = [int]$counts[$requestedKey] + 1
                break
            }
        }
    }

    return [ordered]@{
        values = $effective
        counts = $counts
    }
}

function Test-RequestedRendererIni {
    param([Parameter(Mandatory = $true)][string]$IniText)

    $parsed = Get-RendererIniEffectiveValues -IniText $IniText -Keys @($RequestedRendererIniValues.Keys)
    $errors = New-Object System.Collections.ArrayList
    foreach ($key in @($RequestedRendererIniValues.Keys)) {
        if ([int]$parsed.counts[$key] -ne 1) {
            [void]$errors.Add(('Expected exactly one [renderer] {0} key; found {1}.' -f $key, $parsed.counts[$key]))
        }
        elseif (-not [string]::Equals([string]$parsed.values[$key], [string]$RequestedRendererIniValues[$key], [System.StringComparison]::Ordinal)) {
            [void]$errors.Add(('Expected [renderer] {0}={1}; found {2}.' -f $key, $RequestedRendererIniValues[$key], $parsed.values[$key]))
        }
    }

    return [ordered]@{
        succeeded = ($errors.Count -eq 0)
        effective = $parsed.values
        counts    = $parsed.counts
        errors    = @($errors)
    }
}

function Get-RequestedIniText {
    if (-not (Test-Path -LiteralPath $SourceIniPath -PathType Leaf)) {
        throw ('Source renderer INI is missing: {0}' -f $SourceIniPath)
    }

    $sourceText = [System.IO.File]::ReadAllText($SourceIniPath, $Utf8NoBom)
    return ConvertTo-UpdatedRendererIniText -SourceText $sourceText -RendererValues $RequestedRendererIniValues
}

function Install-RendererProxy {
    $script:Installation['attempted'] = $true
    $script:Installation['status'] = 'validating'

    if (-not (Test-Path -LiteralPath $GeneratedProxyPath -PathType Leaf)) {
        throw ('Generated proxy is missing: {0}' -f $GeneratedProxyPath)
    }
    if (-not (Test-Path -LiteralPath $GameExePath -PathType Leaf)) {
        throw ('Game executable is missing: {0}' -f $GameExePath)
    }

    $generatedHash = Get-Sha256 -Path $GeneratedProxyPath
    if ($null -eq $generatedHash) {
        throw ('Could not hash generated proxy: {0}' -f $GeneratedProxyPath)
    }
    $script:Installation['generatedProxySha256'] = $generatedHash
    $script:Installation['sourceIniSha256'] = Get-Sha256 -Path $SourceIniPath
    $requestedIniPath = Join-Path $StateDirectory 'requested-bully_d3d9proxy.ini'
    $requestedIniText = Get-RequestedIniText
    $requestedIniValidation = Test-RequestedRendererIni -IniText $requestedIniText
    if (-not $requestedIniValidation.succeeded) {
        throw ('Generated renderer INI validation failed: {0}' -f ($requestedIniValidation.errors -join ' '))
    }
    [System.IO.File]::WriteAllText($requestedIniPath, $requestedIniText, $Utf8NoBom)
    $requestedIniHash = Get-Sha256 -Path $requestedIniPath
    if ($null -eq $requestedIniHash) {
        throw 'Could not hash the requested renderer INI.'
    }
    $script:Artifacts['requestedIni'] = $requestedIniPath
    $script:Installation['rendererOverrides']['requestedIni']['path'] = $requestedIniPath
    $script:Installation['rendererOverrides']['requestedIni']['sha256'] = $requestedIniHash
    $script:Installation['rendererOverrides']['requestedIni']['effective'] = $requestedIniValidation.effective
    $script:Installation['rendererOverrides']['requestedIni']['validation'] = $requestedIniValidation

    Set-ProxyHashHistoryEntries -Entries (Read-ProxyHashHistory)
    $script:Installation['knownProxyHashCountBefore'] = $script:ProxyHashHistoryEntries.Count
    $existingHash = Get-Sha256 -Path $GameD3D9Path
    $script:Installation['gameD3D9BeforeSha256'] = $existingHash
    $script:Installation['gameIniBeforeSha256'] = Get-Sha256 -Path $GameIniPath

    $isKnownGeneratedProxy = ($null -eq $existingHash) -or ($existingHash -eq $generatedHash)
    if (-not $isKnownGeneratedProxy) {
        foreach ($entry in $script:ProxyHashHistoryEntries) {
            if ($entry.sha256 -eq $existingHash) {
                $isKnownGeneratedProxy = $true
                break
            }
        }
    }
    $script:Installation['trustedExistingProxy'] = $isKnownGeneratedProxy

    if (-not $isKnownGeneratedProxy -and -not $ForceInstall) {
        throw ('Refusing to overwrite unrecognized game d3d9.dll (SHA-256 {0}). Re-run with -ForceInstall only after verifying it is safe to replace.' -f $existingHash)
    }
    if (-not $isKnownGeneratedProxy -and $ForceInstall) {
        Add-RunWarning -Message ('-ForceInstall is replacing an unrecognized game d3d9.dll with SHA-256 {0}.' -f $existingHash)
    }

    # Persist the verified build identity before mutation. If PowerShell or the
    # host is interrupted after staging, a later build can still identify this
    # DLL as one previously generated from the project build output.
    Add-GeneratedProxyHashToHistory -Hash $generatedHash
    Save-OriginalGameFile -Path $GameD3D9Path -State $script:DllInstallState -BackupName 'original-d3d9.dll'
    Save-OriginalGameFile -Path $GameIniPath -State $script:IniInstallState -BackupName 'original-bully_d3d9proxy.ini'
    $script:Artifacts['originalD3D9Backup'] = $script:DllInstallState['backupPath']
    $script:Artifacts['originalIniBackup'] = $script:IniInstallState['backupPath']

    $script:DllInstallState['installAttempted'] = $true
    $script:DllInstallState['installedHash'] = $generatedHash
    Replace-FileFromSource -SourcePath $GeneratedProxyPath -DestinationPath $GameD3D9Path -ExpectedHash $generatedHash -Label 'd3d9'
    $script:DllInstallState['changedByProbe'] = $true

    $script:IniInstallState['installAttempted'] = $true
    $script:IniInstallState['installedHash'] = $requestedIniHash
    Replace-FileFromSource -SourcePath $requestedIniPath -DestinationPath $GameIniPath -ExpectedHash $requestedIniHash -Label 'renderer-ini'
    $script:IniInstallState['changedByProbe'] = $true
    $script:Installation['rendererOverrides']['activeIniStatus'] = 'applied-by-probe'
    $script:Installation['rendererOverrides']['activeIniEffective'] = $requestedIniValidation.effective

    $script:Installation['status'] = 'installed'
    Add-RunLog -Level 'info' -Message ('Installed generated proxy and renderer INI with backend={0}; on12_device={1}; force_swap_effect={2}; force_present_interval={3}.' -f `
        $Backend, $On12Device, $ForceSwapEffect, $ForcePresentInterval)
}

if ($ValidateIniOnly) {
    $validationFailed = $false
    $fixturePath = Join-Path $ScriptDirectory ('.ini-validation-{0}.ini' -f [System.Guid]::NewGuid().ToString('N'))
    $validationOutput = $null
    try {
        $requestedIniText = Get-RequestedIniText
        $validation = Test-RequestedRendererIni -IniText $requestedIniText
        [System.IO.File]::WriteAllText($fixturePath, $requestedIniText, $Utf8NoBom)
        $fixtureText = [System.IO.File]::ReadAllText($fixturePath, $Utf8NoBom)
        $fixtureValidation = Test-RequestedRendererIni -IniText $fixtureText
        $validationOutput = [ordered]@{
            mode                 = 'validate-ini-only'
            fixturePath          = $fixturePath
            presentationControls = $RequestedPresentationControls
            validation           = $fixtureValidation
            requestedIni         = $fixtureText
        }
        if ((-not $validation.succeeded) -or (-not $fixtureValidation.succeeded)) {
            $validationFailed = $true
        }
    }
    catch {
        $validationFailed = $true
        [pscustomobject][ordered]@{
            mode                 = 'validate-ini-only'
            presentationControls = $RequestedPresentationControls
            error                = $_.Exception.Message
            errorType            = $_.Exception.GetType().FullName
        } | ConvertTo-Json -Depth 8
    }
    finally {
        if (Test-Path -LiteralPath $fixturePath -PathType Leaf) {
            Remove-Item -LiteralPath $fixturePath -Force -ErrorAction SilentlyContinue
        }
    }
    if ($null -ne $validationOutput) {
        $validationOutput['fixtureRemoved'] = -not (Test-Path -LiteralPath $fixturePath -PathType Leaf)
        if (-not [bool]$validationOutput['fixtureRemoved']) {
            $validationFailed = $true
        }
        $validationOutput | ConvertTo-Json -Depth 10
    }
    exit $(if ($validationFailed) { 1 } else { 0 })
}

function Convert-PixelMetrics {
    param([Parameter(Mandatory = $true)][object]$Metrics)

    return [ordered]@{
        width                     = [int]$Metrics.Width
        height                    = [int]$Metrics.Height
        pixelCount                = [long]$Metrics.PixelCount
        meanRgb                   = [ordered]@{ red = $Metrics.MeanRed; green = $Metrics.MeanGreen; blue = $Metrics.MeanBlue }
        meanLuminance             = $Metrics.MeanLuminance
        minRgb                    = [ordered]@{ red = [int]$Metrics.MinRed; green = [int]$Metrics.MinGreen; blue = [int]$Metrics.MinBlue }
        maxRgb                    = [ordered]@{ red = [int]$Metrics.MaxRed; green = [int]$Metrics.MaxGreen; blue = [int]$Metrics.MaxBlue }
        luminanceVariance         = $Metrics.LuminanceVariance
        luminanceStdDev           = $Metrics.LuminanceStdDev
        coarseColorBinsPerChannel = [int]$Metrics.CoarseColorBinsPerChannel
        coarseColorNonZeroBins    = [int]$Metrics.CoarseColorNonZeroBins
        luminanceHistogram16      = @($Metrics.LuminanceHistogram16)
        nearWhiteRatio            = $Metrics.NearWhiteRatio
        nearBlackRatio            = $Metrics.NearBlackRatio
    }
}

function Get-PixelClassification {
    param([Parameter(Mandatory = $true)][object]$Metrics)

    if ($Metrics.NearWhiteRatio -ge $Thresholds.blankPixelRatio) {
        return [ordered]@{ name = 'blank-white'; reason = 'near-white pixel ratio reached blankPixelRatio' }
    }
    if ($Metrics.NearBlackRatio -ge $Thresholds.blankPixelRatio) {
        return [ordered]@{ name = 'blank-black'; reason = 'near-black pixel ratio reached blankPixelRatio' }
    }
    if (($Metrics.LuminanceStdDev -le $Thresholds.uniformLuminanceStdDev) -and
        ($Metrics.CoarseColorNonZeroBins -le $Thresholds.uniformCoarseColorBins)) {
        return [ordered]@{ name = 'low-information-uniform'; reason = 'very low luminance variation and coarse color diversity' }
    }
    if (($Metrics.LuminanceStdDev -le $Thresholds.lowInformationLuminanceStdDev) -and
        ($Metrics.CoarseColorNonZeroBins -le $Thresholds.lowInformationCoarseColorBins)) {
        return [ordered]@{ name = 'low-information-uniform'; reason = 'low luminance variation and coarse color diversity' }
    }
    return [ordered]@{ name = 'nonblank'; reason = 'image exceeded blank and low-information thresholds' }
}

function Analyze-ImageFile {
    param([Parameter(Mandatory = $true)][string]$Path)

    try {
        $metrics = [RenderProbe.NativeMethods]::AnalyzeImage(
            $Path,
            [int]$Thresholds.coarseColorBinsPerChannel,
            [int]$Thresholds.nearWhiteChannel,
            [int]$Thresholds.nearBlackChannel)
        $classification = Get-PixelClassification -Metrics $metrics
        return [ordered]@{
            succeeded      = $true
            classification = $classification.name
            reason         = $classification.reason
            metrics        = Convert-PixelMetrics -Metrics $metrics
            error          = $null
        }
    }
    catch {
        return [ordered]@{
            succeeded      = $false
            classification = 'analysis-failed'
            reason         = 'image analysis threw an exception'
            metrics        = $null
            error          = $_.Exception.Message
        }
    }
}

function Write-PngWithPrintWindow {
    param(
        [Parameter(Mandatory = $true)][System.IntPtr]$WindowHandle,
        [Parameter(Mandatory = $true)][object]$Rectangle,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $bitmap = $null
    $graphics = $null
    $hdc = [System.IntPtr]::Zero
    $hasHdc = $false
    try {
        if ($Rectangle.Width -le 0 -or $Rectangle.Height -le 0) {
            throw 'Window rectangle has no drawable area.'
        }
        $bitmap = [System.Drawing.Bitmap]::new($Rectangle.Width, $Rectangle.Height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $graphics.Clear([System.Drawing.Color]::Black)
        $hdc = $graphics.GetHdc()
        $hasHdc = $true
        # PW_RENDERFULLCONTENT. This does not activate or foreground the window.
        $printed = [RenderProbe.NativeMethods]::PrintWindow($WindowHandle, $hdc, [uint32]2)
        $graphics.ReleaseHdc($hdc)
        $hasHdc = $false

        if (-not $printed) {
            return [ordered]@{ succeeded = $false; apiReturnedTrue = $false; error = 'PrintWindow returned false.' }
        }

        $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
        return [ordered]@{ succeeded = $true; apiReturnedTrue = $true; error = $null }
    }
    catch {
        return [ordered]@{ succeeded = $false; apiReturnedTrue = $false; error = $_.Exception.Message }
    }
    finally {
        if ($hasHdc -and $null -ne $graphics) {
            try { $graphics.ReleaseHdc($hdc) } catch { }
        }
        if ($null -ne $graphics) { $graphics.Dispose() }
        if ($null -ne $bitmap) { $bitmap.Dispose() }
    }
}

function Write-PngWithScreenCopy {
    param(
        [Parameter(Mandatory = $true)][object]$Rectangle,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $bitmap = $null
    $graphics = $null
    try {
        if ($Rectangle.Width -le 0 -or $Rectangle.Height -le 0) {
            throw 'Capture rectangle has no drawable area.'
        }
        $bitmap = [System.Drawing.Bitmap]::new($Rectangle.Width, $Rectangle.Height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $size = [System.Drawing.Size]::new($Rectangle.Width, $Rectangle.Height)
        $graphics.CopyFromScreen(
            $Rectangle.Left,
            $Rectangle.Top,
            0,
            0,
            $size,
            [System.Drawing.CopyPixelOperation]::SourceCopy)
        $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
        return [ordered]@{ succeeded = $true; error = $null; errorCode = $null; failureKind = $null }
    }
    catch {
        $errorCode = $null
        try {
            $errorCode = [RenderProbe.NativeMethods]::GetNativeErrorCode($_.Exception)
        }
        catch {
            # The original capture exception remains the useful failure record.
        }
        $preflightCaptureFailed = ($null -ne $script:Display['preflight']) -and
            (-not [bool]$script:Display['preflight'].hostCapture.succeeded)
        $failureKind = if ($preflightCaptureFailed -or ($errorCode -eq 6)) {
            'host-capture-unavailable'
        }
        else {
            'screen-copy-failed'
        }
        return [ordered]@{
            succeeded   = $false
            error       = $_.Exception.Message
            errorCode   = $errorCode
            failureKind = $failureKind
        }
    }
    finally {
        if ($null -ne $graphics) { $graphics.Dispose() }
        if ($null -ne $bitmap) { $bitmap.Dispose() }
    }
}

function Initialize-NativeHelper {
    param([switch]$SuppressLog)
    if ($env:OS -ne 'Windows_NT') {
        throw 'render_probe requires Windows.'
    }
    if (-not (Test-Path -LiteralPath $NativeMethodsSourcePath -PathType Leaf)) {
        throw ('Native helper source is missing: {0}' -f $NativeMethodsSourcePath)
    }

    try {
        Add-Type -AssemblyName System.Drawing -ErrorAction Stop
    }
    catch {
        Add-Type -AssemblyName System.Drawing.Common -ErrorAction Stop
    }

    if ($null -eq ('RenderProbe.NativeMethods' -as [type])) {
        $drawingAssembly = [System.Drawing.Bitmap].Assembly
        $references = New-Object System.Collections.Generic.List[string]
        [void]$references.Add($drawingAssembly.Location)
        if ($PSVersionTable.PSEdition -eq 'Core') {
            # PowerShell 7's System.Drawing.Common has split dependencies that
            # Add-Type does not infer from the primary assembly reference.
            foreach ($referenceName in $drawingAssembly.GetReferencedAssemblies()) {
                try {
                    $referenceAssembly = [System.Reflection.Assembly]::Load($referenceName)
                    if (-not [string]::IsNullOrWhiteSpace($referenceAssembly.Location)) {
                        [void]$references.Add($referenceAssembly.Location)
                    }
                }
                catch {
                    # Framework assemblies without a file path are compiler defaults.
                }
            }
        }
        Add-Type -Path $NativeMethodsSourcePath -ReferencedAssemblies @($references | Select-Object -Unique) -ErrorAction Stop
    }

    $script:Display['dpiAwareness'] = [RenderProbe.NativeMethods]::TryEnablePerMonitorDpiAwareness()
    if (-not $SuppressLog) {
        Add-RunLog -Level 'info' -Message ('Loaded Windows capture helper (DPI awareness: {0}).' -f $script:Display['dpiAwareness'])
    }
}

function Get-WindowHandleText {
    param([Parameter(Mandatory = $true)][System.IntPtr]$Handle)

    if ($Handle -eq [System.IntPtr]::Zero) {
        return $null
    }
    return ('0x{0:X}' -f $Handle.ToInt64())
}

function Get-ProcessState {
    $state = [ordered]@{
        alive            = $false
        responding       = $null
        windowHandle     = [System.IntPtr]::Zero
        windowHandleText = $null
        windowTitle      = $null
        windowRectangle  = $null
        stateError       = $null
    }

    if ($null -eq $script:GameProcess) {
        return [pscustomobject]$state
    }

    try {
        $script:GameProcess.Refresh()
        if ($script:GameProcess.HasExited) {
            return [pscustomobject]$state
        }

        $state['alive'] = $true
        try {
            $state['responding'] = [bool]$script:GameProcess.Responding
        }
        catch {
            $state['stateError'] = ('Could not query process responding state: {0}' -f $_.Exception.Message)
        }

        $handle = [RenderProbe.NativeMethods]::FindVisibleTopLevelWindowForProcess($script:GameProcess.Id)
        if ($handle -ne [System.IntPtr]::Zero) {
            $state['windowHandle'] = $handle
            $state['windowHandleText'] = Get-WindowHandleText -Handle $handle
            $state['windowTitle'] = [RenderProbe.NativeMethods]::GetWindowTitle($handle)
            $rectangle = New-Object RenderProbe.RECT
            if ([RenderProbe.NativeMethods]::GetWindowRect($handle, [ref]$rectangle)) {
                $state['windowRectangle'] = Get-RectRecord -Rectangle $rectangle
            }
            else {
                $state['stateError'] = 'GetWindowRect returned false.'
            }
        }
    }
    catch {
        $state['stateError'] = $_.Exception.Message
    }

    return [pscustomobject]$state
}

function Convert-ProcessStateRecord {
    param(
        [Parameter(Mandatory = $true)][object]$State,
        [Parameter(Mandatory = $true)][double]$ElapsedSeconds
    )

    return [ordered]@{
        elapsedSeconds = [Math]::Round($ElapsedSeconds, 3)
        observedUtc    = (Get-Date).ToUniversalTime().ToString('o')
        alive          = [bool]$State.alive
        responding     = $State.responding
        windowHandle   = $State.windowHandleText
        windowTitle    = $State.windowTitle
        windowRectangle = $State.windowRectangle
        stateError     = $State.stateError
    }
}

function Add-ProcessObservation {
    param([Parameter(Mandatory = $true)][double]$ElapsedSeconds)

    $state = Get-ProcessState
    [void]$script:Observations.Add([pscustomobject](Convert-ProcessStateRecord -State $state -ElapsedSeconds $ElapsedSeconds))
    return $state
}

function New-CaptureRecord {
    param(
        [Parameter(Mandatory = $true)][int]$RequestedAtSeconds,
        [Parameter(Mandatory = $true)][double]$ElapsedSeconds
    )

    return [ordered]@{
        requestedAtSeconds = $RequestedAtSeconds
        elapsedSeconds     = [Math]::Round($ElapsedSeconds, 3)
        observedUtc        = (Get-Date).ToUniversalTime().ToString('o')
        status             = 'pending'
        processAlive       = $null
        processResponding  = $null
        windowHandle       = $null
        windowTitle        = $null
        target             = $null
        fallbackToPrimaryMonitor = $false
        rectangle          = $null
        attempts           = @()
        selected           = $null
        error              = $null
    }
}

function Add-CaptureAttempt {
    param(
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$Capture,
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$Attempt
    )

    $Capture['attempts'] = @($Capture['attempts']) + [pscustomobject]$Attempt
}

function Invoke-RequestedCapture {
    param(
        [Parameter(Mandatory = $true)][int]$RequestedAtSeconds,
        [Parameter(Mandatory = $true)][double]$ElapsedSeconds
    )

    $capture = New-CaptureRecord -RequestedAtSeconds $RequestedAtSeconds -ElapsedSeconds $ElapsedSeconds
    $state = Get-ProcessState
    $capture['processAlive'] = [bool]$state.alive
    $capture['processResponding'] = $state.responding
    $capture['windowHandle'] = $state.windowHandleText
    $capture['windowTitle'] = $state.windowTitle

    if (-not $state.alive) {
        $capture['status'] = 'skipped-process-exited'
        $capture['error'] = 'The game process had already exited before this capture time.'
        [void]$script:CaptureRecords.Add([pscustomobject]$capture)
        Add-RunLog -Level 'warning' -Message ('Skipped capture at {0}s because Bully.exe had exited.' -f $RequestedAtSeconds)
        return
    }

    $targetRectangle = $null
    $target = 'main-window'
    if (($state.windowHandle -ne [System.IntPtr]::Zero) -and ($null -ne $state.windowRectangle) -and
        ($state.windowRectangle.width -gt 0) -and ($state.windowRectangle.height -gt 0)) {
        $targetRectangle = $state.windowRectangle
    }
    else {
        $primaryRectangle = [RenderProbe.NativeMethods]::GetPrimaryMonitorBounds()
        $targetRectangle = Get-RectRecord -Rectangle $primaryRectangle
        $target = 'primary-monitor-fallback'
        $capture['fallbackToPrimaryMonitor'] = $true
        Add-RunWarning -Message ('No visible main window was available at {0}s; capturing the primary monitor.' -f $RequestedAtSeconds)
    }

    $capture['target'] = $target
    $capture['rectangle'] = $targetRectangle
    $captureLabel = '{0:D4}' -f $RequestedAtSeconds
    $printSelected = $null

    if ($target -eq 'main-window') {
        $printPath = Join-Path $CaptureDirectory ('capture-{0}s-printwindow.png' -f $captureLabel)
        $printResult = Write-PngWithPrintWindow -WindowHandle $state.windowHandle -Rectangle ([pscustomobject]$targetRectangle) -Path $printPath
        $printAttempt = [ordered]@{
            method          = 'PrintWindow'
            outputPath      = if ($printResult.succeeded) { $printPath } else { $null }
            apiReturnedTrue = $printResult.apiReturnedTrue
            succeeded       = $printResult.succeeded
            analysis        = $null
            error           = $printResult.error
        }
        if ($printResult.succeeded) {
            $printAttempt['analysis'] = Analyze-ImageFile -Path $printPath
            $printSelected = [pscustomobject]$printAttempt
        }
        Add-CaptureAttempt -Capture $capture -Attempt $printAttempt
        if ($printResult.succeeded) {
            Add-RunLog -Level 'info' -Message ('PrintWindow wrote capture at {0}s ({1}).' -f $RequestedAtSeconds, $printAttempt.analysis.classification)
        }
        else {
            Add-RunLog -Level 'warning' -Message ('PrintWindow failed at {0}s: {1}' -f $RequestedAtSeconds, $printResult.error)
        }
    }

    $printWasUsable = ($null -ne $printSelected) -and $printSelected.analysis.succeeded -and
        ($printSelected.analysis.classification -eq 'nonblank')
    if (-not $printWasUsable) {
        $screenPath = Join-Path $CaptureDirectory ('capture-{0}s-copy-from-screen.png' -f $captureLabel)
        $screenResult = Write-PngWithScreenCopy -Rectangle ([pscustomobject]$targetRectangle) -Path $screenPath
        $screenAttempt = [ordered]@{
            method          = 'CopyFromScreen'
            outputPath      = if ($screenResult.succeeded) { $screenPath } else { $null }
            apiReturnedTrue = $null
            succeeded       = $screenResult.succeeded
            analysis        = $null
            failureKind     = $screenResult.failureKind
            errorCode       = $screenResult.errorCode
            error           = $screenResult.error
        }
        if ($screenResult.succeeded) {
            $screenAttempt['analysis'] = Analyze-ImageFile -Path $screenPath
            $capture['selected'] = [pscustomobject]$screenAttempt
            Add-RunLog -Level 'info' -Message ('CopyFromScreen wrote capture at {0}s ({1}; target={2}).' -f $RequestedAtSeconds, $screenAttempt.analysis.classification, $target)
        }
        else {
            Add-RunLog -Level 'warning' -Message ('CopyFromScreen failed at {0}s: {1}' -f $RequestedAtSeconds, $screenResult.error)
        }
        Add-CaptureAttempt -Capture $capture -Attempt $screenAttempt
    }

    if ($null -eq $capture['selected'] -and $null -ne $printSelected) {
        $capture['selected'] = $printSelected
    }

    if ($null -ne $capture['selected']) {
        $capture['status'] = 'captured'
    }
    else {
        $hostCaptureAttempt = @($capture['attempts'] | Where-Object { $_.failureKind -eq 'host-capture-unavailable' } | Select-Object -First 1)
        if ($hostCaptureAttempt.Count -gt 0) {
            $capture['status'] = 'host-capture-unavailable'
            $capture['error'] = 'Host screen capture is unavailable; CopyFromScreen could not produce an image.'
        }
        else {
            $capture['status'] = 'capture-failed'
            $capture['error'] = 'Neither PrintWindow nor CopyFromScreen produced an analyzable image.'
        }
    }

    [void]$script:CaptureRecords.Add([pscustomobject]$capture)
}

function Add-UnscheduledCaptureRecord {
    param(
        [Parameter(Mandatory = $true)][int]$RequestedAtSeconds,
        [Parameter(Mandatory = $true)][string]$Status,
        [Parameter(Mandatory = $true)][string]$Reason
    )

    $record = New-CaptureRecord -RequestedAtSeconds $RequestedAtSeconds -ElapsedSeconds 0
    $record['status'] = $Status
    $record['error'] = $Reason
    [void]$script:CaptureRecords.Add([pscustomobject]$record)
}

function Set-ProcessExitMetadata {
    if ($null -eq $script:GameProcess) {
        return
    }

    try {
        $script:GameProcess.Refresh()
        if ($script:GameProcess.HasExited) {
            $script:ProcessInfo['exitUtc'] = (Get-Date).ToUniversalTime().ToString('o')
            try {
                $script:ProcessInfo['exitCode'] = $script:GameProcess.ExitCode
            }
            catch {
                Add-RunWarning -Message ('Could not read Bully.exe exit code. {0}' -f $_.Exception.Message)
            }
        }
    }
    catch {
        Add-RunWarning -Message ('Could not refresh Bully.exe exit metadata. {0}' -f $_.Exception.Message)
    }
}

function Stop-GameProcess {
    param([Parameter(Mandatory = $true)][string]$Reason)

    if ($null -eq $script:GameProcess) {
        return
    }

    $state = Get-ProcessState
    if (-not $state.alive) {
        Set-ProcessExitMetadata
        return
    }

    try {
        $script:GameProcess.Kill()
        $exited = $script:GameProcess.WaitForExit(5000)
        if ($exited) {
            $script:ProcessInfo['terminatedByHarness'] = $true
            $script:ProcessInfo['terminationReason'] = $Reason
            Add-RunLog -Level 'info' -Message ('Terminated Bully.exe: {0}.' -f $Reason)
        }
        else {
            Add-RunError -Message ('Bully.exe did not exit within 5 seconds of the {0} termination request.' -f $Reason)
        }
    }
    catch {
        Add-RunError -Message ('Failed to terminate Bully.exe during {0}. {1}' -f $Reason, $_.Exception.Message)
    }
    finally {
        Set-ProcessExitMetadata
    }
}

function Invoke-GameProbe {
    if (-not (Test-Path -LiteralPath $GameExePath -PathType Leaf)) {
        throw ('Game executable is missing: {0}' -f $GameExePath)
    }

    $scheduledCaptureTimes = @()
    foreach ($captureTime in @($NormalizedCaptureAtSeconds)) {
        if ($captureTime -lt 0) {
            throw ('CaptureAtSeconds contains a negative time: {0}' -f $captureTime)
        }
        if ($captureTime -le $DurationSeconds) {
            $scheduledCaptureTimes += [int]$captureTime
        }
        else {
            Add-UnscheduledCaptureRecord -RequestedAtSeconds ([int]$captureTime) -Status 'not-scheduled-after-duration' -Reason ('Requested capture time exceeds DurationSeconds ({0}).' -f $DurationSeconds)
            Add-RunWarning -Message ('Capture at {0}s exceeds the {1}s duration and will not run.' -f $captureTime, $DurationSeconds)
        }
    }
    $scheduledCaptureTimes = @($scheduledCaptureTimes | Sort-Object -Unique)

    $script:GameProcess = Start-Process -FilePath $GameExePath -WorkingDirectory $GameDirectory -PassThru
    # Arm finally cleanup immediately after Start-Process returns.
    $script:RunStartedProcess = $true
    $script:ProcessInfo['launched'] = $true
    $script:ProcessInfo['processId'] = $script:GameProcess.Id
    $script:ProcessInfo['launchUtc'] = (Get-Date).ToUniversalTime().ToString('o')
    Add-RunLog -Level 'info' -Message ('Launched Bully.exe (PID {0}) with working directory {1}.' -f $script:GameProcess.Id, $GameDirectory)

    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $nextObservationSeconds = 0.0
    $nextCaptureIndex = 0
    $endedEarly = $false

    while ($true) {
        $elapsed = $watch.Elapsed.TotalSeconds
        if ($elapsed -ge $nextObservationSeconds) {
            [void](Add-ProcessObservation -ElapsedSeconds $elapsed)
            $nextObservationSeconds += 1.0
        }

        while (($nextCaptureIndex -lt $scheduledCaptureTimes.Count) -and
            ($elapsed + 0.005 -ge [double]$scheduledCaptureTimes[$nextCaptureIndex])) {
            Invoke-RequestedCapture -RequestedAtSeconds ([int]$scheduledCaptureTimes[$nextCaptureIndex]) -ElapsedSeconds $elapsed
            $nextCaptureIndex++
            $elapsed = $watch.Elapsed.TotalSeconds
        }

        $state = Get-ProcessState
        if (-not $state.alive) {
            $endedEarly = $true
            $script:ProcessInfo['exitedBeforeTimeout'] = $true
            Set-ProcessExitMetadata
            Add-RunWarning -Message ('Bully.exe exited before the {0}s timeout.' -f $DurationSeconds)
            break
        }

        if ($elapsed -ge $DurationSeconds) {
            $script:ProcessInfo['survivedToTimeout'] = $true
            break
        }

        $nextDeadline = [double]$DurationSeconds
        if ($nextCaptureIndex -lt $scheduledCaptureTimes.Count) {
            $nextDeadline = [Math]::Min($nextDeadline, [double]$scheduledCaptureTimes[$nextCaptureIndex])
        }
        $millisecondsUntilDeadline = [int][Math]::Ceiling(($nextDeadline - $elapsed) * 1000.0)
        $sleepMilliseconds = [Math]::Min(250, [Math]::Max(20, $millisecondsUntilDeadline))
        Start-Sleep -Milliseconds $sleepMilliseconds
    }

    if ($endedEarly) {
        while ($nextCaptureIndex -lt $scheduledCaptureTimes.Count) {
            Add-UnscheduledCaptureRecord -RequestedAtSeconds ([int]$scheduledCaptureTimes[$nextCaptureIndex]) -Status 'skipped-process-exited' -Reason 'The game exited before this scheduled capture.'
            $nextCaptureIndex++
        }
    }
    elseif ($script:ProcessInfo['survivedToTimeout']) {
        Stop-GameProcess -Reason 'bounded-duration-timeout'
    }
}

function Copy-ArtifactFile {
    param(
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$DestinationName
    )

    $record = [ordered]@{
        sourcePath = $SourcePath
        copiedPath = $null
        exists     = (Test-Path -LiteralPath $SourcePath -PathType Leaf)
        sha256     = $null
        error      = $null
    }
    if (-not $record['exists']) {
        return [pscustomobject]$record
    }

    try {
        $destinationPath = Join-Path $ArtifactDirectory $DestinationName
        Copy-Item -LiteralPath $SourcePath -Destination $destinationPath -Force
        $record['copiedPath'] = $destinationPath
        $record['sha256'] = Get-Sha256 -Path $destinationPath
    }
    catch {
        $record['error'] = $_.Exception.Message
        Add-RunWarning -Message ('Could not copy artifact {0}. {1}' -f $SourcePath, $_.Exception.Message)
    }
    return [pscustomobject]$record
}

function Copy-ProxyLogEvidence {
    param(
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$Before,
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$After,
        [Parameter(Mandatory = $true)][bool]$BaselineUnavailable
    )

    $preRunLength = if ([bool]$Before['exists']) { [long]$Before['length'] } else { 0L }
    $postRunLength = if ([bool]$After['exists']) { [long]$After['length'] } else { $null }
    $primaryPath = Join-Path $ArtifactDirectory 'bully_d3d9proxy.log'
    $record = [ordered]@{
        sourcePath              = $ProxyLogPath
        copiedPath              = $null
        exists                  = [bool]$After['exists']
        sha256                  = $null
        error                   = $null
        before                  = $Before
        after                   = $After
        changedDuringRun        = if ($BaselineUnavailable) { $null } else { Test-FileChangedSinceSnapshot -Before $Before -After $After }
        preRunByteLength        = if ([bool]$Before['exists']) { [long]$Before['length'] } else { $null }
        postRunByteLength       = $postRunLength
        archiveStartByteOffset  = $null
        archiveEndByteOffsetExclusive = $null
        archivedByteLength      = $null
        deltaExtractionMethod   = $null
        deltaExtractionReason   = $null
        preRunPrefixSha256      = if ([bool]$Before['exists']) { $Before['sha256'] } else { $null }
        postRunSha256           = if ([bool]$After['exists']) { $After['sha256'] } else { $null }
        postRunPrefixSha256     = $null
        prefixVerification      = $null
        collectionPhase         = 'after-process-termination-before-proxy-restoration'
    }
    $fullLog = $null

    if (-not [bool]$After['exists']) {
        $record['deltaExtractionMethod'] = 'post-run-log-missing'
        $record['deltaExtractionReason'] = 'The proxy log did not exist after process termination.'
        return [pscustomobject]@{
            primary = [pscustomobject]$record
            full    = $fullLog
        }
    }

    try {
        if ($BaselineUnavailable) {
            $fullPost = Copy-ArtifactFile -SourcePath $ProxyLogPath -DestinationName 'bully_d3d9proxy.log'
            $record['copiedPath'] = $fullPost.copiedPath
            $record['sha256'] = $fullPost.sha256
            $record['error'] = $fullPost.error
            $record['archivedByteLength'] = $postRunLength
            $record['archiveStartByteOffset'] = 0
            $record['archiveEndByteOffsetExclusive'] = $postRunLength
            $record['deltaExtractionMethod'] = 'full-post-log-baseline-unavailable'
            $record['deltaExtractionReason'] = 'No pre-run proxy-log baseline was available; the full post-run log was archived.'
            $record['prefixVerification'] = 'not-available'
        }
        elseif (-not [bool]$Before['exists']) {
            Copy-FileByteRange -SourcePath $ProxyLogPath -Offset 0 -Length $postRunLength -DestinationPath $primaryPath
            $record['copiedPath'] = $primaryPath
            $record['sha256'] = Get-Sha256 -Path $primaryPath
            $record['archivedByteLength'] = $postRunLength
            $record['archiveStartByteOffset'] = 0
            $record['archiveEndByteOffsetExclusive'] = $postRunLength
            $record['deltaExtractionMethod'] = 'whole-post-log-created-during-run'
            $record['deltaExtractionReason'] = 'The proxy log did not exist before the run, so the complete post-run log is the run delta.'
            $record['prefixVerification'] = 'not-required-no-pre-run-log'
        }
        elseif ($postRunLength -lt $preRunLength) {
            $fullPost = Copy-ArtifactFile -SourcePath $ProxyLogPath -DestinationName 'bully_d3d9proxy.log'
            $record['copiedPath'] = $fullPost.copiedPath
            $record['sha256'] = $fullPost.sha256
            $record['error'] = $fullPost.error
            $record['archivedByteLength'] = $postRunLength
            $record['archiveStartByteOffset'] = 0
            $record['archiveEndByteOffsetExclusive'] = $postRunLength
            $record['deltaExtractionMethod'] = 'full-post-log-after-truncation'
            $record['deltaExtractionReason'] = ('Post-run log length ({0}) is smaller than pre-run length ({1}); the full post-run log was archived.' -f $postRunLength, $preRunLength)
            $record['prefixVerification'] = 'not-attempted-post-run-log-shorter'
        }
        else {
            $postPrefixHash = Get-FilePrefixSha256 -Path $ProxyLogPath -Length $preRunLength
            $record['postRunPrefixSha256'] = $postPrefixHash
            if (($null -ne $postPrefixHash) -and ($postPrefixHash -eq $Before['sha256'])) {
                $deltaLength = $postRunLength - $preRunLength
                Copy-FileByteRange -SourcePath $ProxyLogPath -Offset $preRunLength -Length $deltaLength -DestinationPath $primaryPath
                $record['copiedPath'] = $primaryPath
                $record['sha256'] = Get-Sha256 -Path $primaryPath
                $record['archivedByteLength'] = $deltaLength
                $record['archiveStartByteOffset'] = $preRunLength
                $record['archiveEndByteOffsetExclusive'] = $postRunLength
                $record['deltaExtractionMethod'] = 'appended-byte-range'
                $record['deltaExtractionReason'] = if ($deltaLength -eq 0) {
                    'The post-run log exactly matched the pre-run log; an empty delta archive was written.'
                }
                else {
                    ('The post-run log preserved the pre-run byte prefix; bytes {0} through {1} were archived.' -f $preRunLength, ($postRunLength - 1))
                }
                $record['prefixVerification'] = 'pre-run-prefix-sha256-matched'
                $fullLog = Copy-ArtifactFile -SourcePath $ProxyLogPath -DestinationName 'bully_d3d9proxy.full.log'
            }
            else {
                $fullPost = Copy-ArtifactFile -SourcePath $ProxyLogPath -DestinationName 'bully_d3d9proxy.log'
                $record['copiedPath'] = $fullPost.copiedPath
                $record['sha256'] = $fullPost.sha256
                $record['error'] = $fullPost.error
                $record['archivedByteLength'] = $postRunLength
                $record['archiveStartByteOffset'] = 0
                $record['archiveEndByteOffsetExclusive'] = $postRunLength
                $record['deltaExtractionMethod'] = 'full-post-log-after-replacement-or-prefix-mismatch'
                $record['deltaExtractionReason'] = 'The post-run log did not preserve the pre-run byte prefix; the full post-run log was archived to avoid an ambiguous delta.'
                $record['prefixVerification'] = 'pre-run-prefix-sha256-mismatched-or-unavailable'
            }
        }
    }
    catch {
        $record['error'] = $_.Exception.Message
        $record['deltaExtractionMethod'] = 'log-archive-failed'
        $record['deltaExtractionReason'] = 'The proxy log could not be archived after process termination.'
        Add-RunWarning -Message ('Could not archive proxy log evidence. {0}' -f $_.Exception.Message)
    }

    return [pscustomobject]@{
        primary = [pscustomobject]$record
        full    = $fullLog
    }
}

function Collect-ProxyBitmapArtifact {
    param(
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$DestinationName,
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$Before,
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$After,
        [Parameter(Mandatory = $true)][bool]$BaselineUnavailable,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $artifact = Copy-ArtifactFile -SourcePath $SourcePath -DestinationName $DestinationName
    $changedDuringRun = if ($BaselineUnavailable) { $null } else { Test-FileChangedSinceSnapshot -Before $Before -After $After }
    $artifact | Add-Member -NotePropertyName changedDuringRun -NotePropertyValue $changedDuringRun
    $artifact | Add-Member -NotePropertyName before -NotePropertyValue $Before
    $artifact | Add-Member -NotePropertyName after -NotePropertyValue $After

    if (($null -ne $artifact.copiedPath) -and ($changedDuringRun -eq $true)) {
        $analysis = Analyze-ImageFile -Path $artifact.copiedPath
        Add-RunLog -Level 'info' -Message ('Collected proxy {0} BMP ({1}).' -f $Label, $analysis.classification)
        return [ordered]@{
            artifact       = $artifact
            classification = $analysis.classification
            analysis       = $analysis
        }
    }

    return [ordered]@{
        artifact       = $artifact
        classification = if ($BaselineUnavailable) { 'baseline-unavailable' } elseif ($artifact.exists) { 'not-updated-during-run' } else { 'not-present' }
        analysis       = $null
    }
}

function Collect-RuntimeArtifacts {
    $preRun = $script:Artifacts['preRun']
    if ($null -eq $preRun) {
        # Helper/setup failures can occur before the normal baseline snapshot.
        # Preserve artifact collection in finally without claiming provenance.
        $preRun = [ordered]@{
            proxyLog      = Get-FileSnapshot -Path $ProxyLogPath
            backbufferBmp = Get-FileSnapshot -Path $BackbufferBmpPath
            frontbufferBmp = Get-FileSnapshot -Path $FrontbufferBmpPath
            baselineUnavailableBeforeCollection = $true
        }
        $script:Artifacts['preRun'] = $preRun
    }
    $baselineUnavailable = [bool]$preRun['baselineUnavailableBeforeCollection']
    $proxyLogBefore = $preRun['proxyLog']
    $backbufferBefore = $preRun['backbufferBmp']
    $frontbufferBefore = $preRun['frontbufferBmp']
    $proxyLogAfter = Get-FileSnapshot -Path $ProxyLogPath
    $backbufferAfter = Get-FileSnapshot -Path $BackbufferBmpPath
    $frontbufferAfter = Get-FileSnapshot -Path $FrontbufferBmpPath

    $proxyLogEvidence = Copy-ProxyLogEvidence -Before $proxyLogBefore -After $proxyLogAfter -BaselineUnavailable $baselineUnavailable
    $script:Artifacts['proxyLog'] = $proxyLogEvidence.primary
    $script:Artifacts['proxyLogFull'] = $proxyLogEvidence.full
    $script:Artifacts['activeIni'] = Copy-ArtifactFile -SourcePath $GameIniPath -DestinationName 'bully_d3d9proxy.active.ini'

    $script:BackbufferAnalysis = Collect-ProxyBitmapArtifact -SourcePath $BackbufferBmpPath -DestinationName 'bully_renderprobe_backbuffer.bmp' -Before $backbufferBefore -After $backbufferAfter -BaselineUnavailable $baselineUnavailable -Label 'backbuffer'
    $script:Artifacts['proxyBackbufferBmp'] = $script:BackbufferAnalysis
    $script:FrontbufferAnalysis = Collect-ProxyBitmapArtifact -SourcePath $FrontbufferBmpPath -DestinationName 'bully_renderprobe_frontbuffer.bmp' -Before $frontbufferBefore -After $frontbufferAfter -BaselineUnavailable $baselineUnavailable -Label 'frontbuffer'
    $script:Artifacts['proxyFrontbufferBmp'] = $script:FrontbufferAnalysis
}

function Get-EventMessageText {
    param([Parameter(Mandatory = $true)][object]$Event)

    try {
        return [string]$Event.Message
    }
    catch {
        return ('<message unavailable: {0}>' -f $_.Exception.Message)
    }
}

function Get-EventDataRecord {
    param([Parameter(Mandatory = $true)][object]$Event)

    $data = [ordered]@{}
    try {
        [xml]$xml = $Event.ToXml()
        $index = 0
        foreach ($node in @($xml.Event.EventData.Data)) {
            $name = $null
            if ($node -is [System.Xml.XmlElement]) {
                $name = $node.GetAttribute('Name')
            }
            if ([string]::IsNullOrWhiteSpace($name)) {
                $name = [string]$node.Name
            }
            if ([string]::IsNullOrWhiteSpace($name)) {
                $name = 'data{0}' -f $index
            }
            $data[$name] = [string]$node.InnerText
            $index++
        }
    }
    catch {
        $data['xmlParseError'] = $_.Exception.Message
    }
    return $data
}

function Get-RegexGroup {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Pattern
    )

    $match = [regex]::Match($Text, $Pattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
    if ($match.Success) {
        return $match.Groups[1].Value.Trim()
    }
    return $null
}

function Get-EventDataValue {
    param(
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$Data,
        [Parameter(Mandatory = $true)][string[]]$Names
    )

    foreach ($requestedName in $Names) {
        foreach ($key in @($Data.Keys)) {
            if ([string]::Equals([string]$key, $requestedName, [System.StringComparison]::OrdinalIgnoreCase)) {
                $value = [string]$Data[$key]
                if (-not [string]::IsNullOrWhiteSpace($value)) {
                    return $value.Trim()
                }
            }
        }
    }
    return $null
}

function ConvertTo-CanonicalHex {
    param([string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return $null
    }
    $match = [regex]::Match($Value, '(?i)(?:0x)?([0-9a-f]+)')
    if (-not $match.Success) {
        return $null
    }
    try {
        $number = [System.Convert]::ToUInt64($match.Groups[1].Value, 16)
        return ('0x{0:x}' -f $number)
    }
    catch {
        return $null
    }
}

function Collect-RecentWerApplicationErrors {
    # A brief pre-run allowance covers event-log timestamp granularity without
    # turning a prior unrelated Bully crash into evidence for this invocation.
    $windowStart = $RunStart.AddSeconds(-1)
    if (-not [string]::IsNullOrWhiteSpace([string]$script:ProcessInfo['launchUtc'])) {
        try {
            $windowStart = [System.DateTimeOffset]::Parse([string]$script:ProcessInfo['launchUtc']).LocalDateTime.AddSeconds(-1)
        }
        catch {
            Add-RunWarning -Message ('Could not parse the launch timestamp for WER filtering. {0}' -f $_.Exception.Message)
        }
    }
    $windowEnd = (Get-Date).AddMinutes(1)
    $records = New-Object System.Collections.ArrayList
    $queryError = $null

    try {
        $events = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; StartTime = $windowStart } -MaxEvents 500 -ErrorAction Stop
        foreach ($event in @($events)) {
            $provider = [string]$event.ProviderName
            if (($provider -ne 'Application Error') -and ($provider -ne 'Windows Error Reporting')) {
                continue
            }
            if (($null -ne $event.TimeCreated) -and ($event.TimeCreated -gt $windowEnd)) {
                continue
            }

            $message = Get-EventMessageText -Event $event
            $eventData = Get-EventDataRecord -Event $event
            $eventDataText = (($eventData.GetEnumerator() | ForEach-Object { '{0}={1}' -f $_.Key, $_.Value }) -join [Environment]::NewLine)
            $searchText = $message + [Environment]::NewLine + $eventDataText
            if ($searchText -notmatch '(?i)\bBully\.exe\b') {
                continue
            }

            $faultOffset = Get-EventDataValue -Data $eventData -Names @(
                'FaultOffset',
                'FaultingOffset',
                'ExceptionOffset',
                'fault offset',
                'faulting offset',
                'exception offset'
            )
            if ($null -eq $faultOffset) {
                $faultOffset = Get-RegexGroup -Text $searchText -Pattern '(?i)(?:fault(?:ing)?|exception)\s+offset\s*[:=]\s*((?:0x)?[0-9a-f]+)'
            }
            if ($null -eq $faultOffset) {
                $faultOffset = Get-RegexGroup -Text $searchText -Pattern 'Bully\.exe\s*\+\s*((?:0x)?[0-9a-f]+)'
            }
            $faultOffset = ConvertTo-CanonicalHex -Value $faultOffset
            $exceptionCode = Get-EventDataValue -Data $eventData -Names @('ExceptionCode', 'exception code')
            if ($null -eq $exceptionCode) {
                $exceptionCode = Get-RegexGroup -Text $searchText -Pattern 'exception code:\s*((?:0x)?[0-9a-f]+)'
            }
            $exceptionCode = ConvertTo-CanonicalHex -Value $exceptionCode
            $applicationName = Get-EventDataValue -Data $eventData -Names @('FaultingApplicationName', 'ApplicationName', 'AppName', 'faulting application name')
            if ($null -eq $applicationName) {
                $applicationName = Get-RegexGroup -Text $searchText -Pattern 'faulting application name:\s*([^,\r\n]+)'
            }

            $knownMatches = @()
            foreach ($signature in @($KnownFailureSignatures)) {
                $applicationMatches = ($searchText -match ('(?i)\b{0}\b' -f [regex]::Escape([string]$signature.application)))
                $exceptionMatches = ($null -ne $exceptionCode) -and ($exceptionCode -eq (ConvertTo-CanonicalHex -Value ([string]$signature.exceptionCode)))
                $offsetMatches = ($null -ne $faultOffset) -and ($faultOffset -eq (ConvertTo-CanonicalHex -Value ([string]$signature.faultOffset)))
                if ($applicationMatches -and $exceptionMatches -and $offsetMatches) {
                    $knownMatches += [string]$signature.description
                }
            }

            $record = [ordered]@{
                timeCreatedUtc      = if ($null -ne $event.TimeCreated) { $event.TimeCreated.ToUniversalTime().ToString('o') } else { $null }
                provider            = $provider
                eventId             = [int]$event.Id
                recordId            = [long]$event.RecordId
                level               = [string]$event.LevelDisplayName
                applicationName     = $applicationName
                exceptionCode       = $exceptionCode
                faultOffset         = $faultOffset
                knownFailureMatches = @($knownMatches)
                eventData           = $eventData
                message             = $message
            }
            [void]$records.Add([pscustomobject]$record)
        }
    }
    catch {
        $queryError = $_.Exception.Message
        Add-RunWarning -Message ('Could not query recent Application Error / WER events. {0}' -f $queryError)
    }

    $script:WerRecords = @($records)
    $faultOffsets = @($script:WerRecords |
        ForEach-Object { $_.faultOffset } |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        Sort-Object -Unique)
    $script:ProcessInfo['faultOffsets'] = $faultOffsets
    $knownEvidence = @($script:WerRecords | Where-Object { @($_.knownFailureMatches).Count -gt 0 })
    $script:ProcessInfo['knownFailureEvidenceSeen'] = ($knownEvidence.Count -gt 0)

    $document = [ordered]@{
        queryWindowStartUtc = $windowStart.ToUniversalTime().ToString('o')
        queryWindowEndUtc   = $windowEnd.ToUniversalTime().ToString('o')
        application         = 'Bully.exe'
        knownFailureSignatures = @($KnownFailureSignatures)
        queryError          = $queryError
        events              = @($script:WerRecords)
    }
    try {
        Write-JsonFile -Path $WerPath -Object $document -Depth 12
    }
    catch {
        Add-RunWarning -Message ('Could not write WER event artifact. {0}' -f $_.Exception.Message)
    }
}

function Restore-DisplaySettings {
    if ($null -eq ('RenderProbe.NativeMethods' -as [type])) {
        $script:Display['restoreDecision'] = 'skipped-native-helper-unavailable'
        $script:Display['restoreResult'] = [ordered]@{
            code      = $null
            succeeded = $false
            meaning   = 'Native helper was unavailable; no display restore was attempted.'
        }
        $script:Display['afterCleanup'] = $null
        return
    }

    try {
        $script:Display['afterTermination'] = Get-PrimaryDisplaySnapshot
    }
    catch {
        Add-RunWarning -Message ('Could not record primary display state after termination. {0}' -f $_.Exception.Message)
    }

    if (($null -eq $script:Display['beforeLaunch']) -or ($null -eq $script:Display['afterTermination']) -or
        (-not [bool]$script:Display['beforeLaunch'].available) -or (-not [bool]$script:Display['afterTermination'].available)) {
        $script:Display['restoreDecision'] = 'skipped-display-snapshot-unavailable'
        $script:Display['restoreResult'] = [ordered]@{
            code      = $null
            succeeded = $false
            meaning   = 'Before-launch or after-termination display state was unavailable; no blind display restore was attempted.'
        }
        Add-RunWarning -Message 'Skipped ChangeDisplaySettings because a display snapshot was unavailable.'
        Record-AfterCleanupDisplayState
        return
    }

    if (Test-DisplayGeometryUnchanged -Before $script:Display['beforeLaunch'] -After $script:Display['afterTermination']) {
        $script:Display['restoreDecision'] = 'not-required-resolution-unchanged'
        $script:Display['restoreResult'] = [ordered]@{
            code      = $null
            succeeded = $true
            meaning   = 'Primary display bounds, size, and position were unchanged after process termination.'
        }
        Record-AfterCleanupDisplayState
        return
    }

    if (-not $script:RunStartedProcess) {
        $script:Display['restoreDecision'] = 'not-required-no-process-launched'
        $script:Display['restoreResult'] = [ordered]@{
            code      = $null
            succeeded = $true
            meaning   = 'No game process was launched; an externally changed display was not modified by the probe.'
        }
        Record-AfterCleanupDisplayState
        return
    }

    $script:Display['restoreDecision'] = 'attempted-display-geometry-changed'
    $script:Display['restoreAttempted'] = $true
    try {
        # ChangeDisplaySettings(NULL, 0) restores the persisted display mode.
        $resultCode = [RenderProbe.NativeMethods]::ChangeDisplaySettings([System.IntPtr]::Zero, 0)
        $script:Display['restoreResult'] = [ordered]@{
            code      = $resultCode
            succeeded = ($resultCode -eq 0)
            meaning   = if ($resultCode -eq 0) { 'DISP_CHANGE_SUCCESSFUL' } else { 'nonzero ChangeDisplaySettings result' }
        }
        if ($resultCode -ne 0) {
            Add-RunWarning -Message ('ChangeDisplaySettings(NULL, 0) returned {0}.' -f $resultCode)
        }
    }
    catch {
        $script:Display['restoreResult'] = [ordered]@{
            code      = $null
            succeeded = $false
            meaning   = $_.Exception.Message
        }
        Add-RunWarning -Message ('Could not restore persisted display settings. {0}' -f $_.Exception.Message)
    }

    Record-AfterCleanupDisplayState
}

function Get-ProbeOutcome {
    if ($script:PreflightBlocked) {
        return [ordered]@{
            succeeded                         = $false
            exitCode                          = 1
            status                            = 'preflight-failed'
            analyzedCaptureCount              = 0
            nonblankCaptureCount              = 0
            blankOrLowInformationCaptureCount = 0
            processSurvivedToTimeout          = $false
            earlyExitOrCrashDetected           = $false
            reasons                            = @('Display preflight failed; the launcher did not stage files or start Bully.exe.')
        }
    }

    $analyzedCaptures = @($script:CaptureRecords | Where-Object {
        ($null -ne $_.selected) -and ($null -ne $_.selected.analysis) -and $_.selected.analysis.succeeded
    })
    $nonblankCaptures = @($analyzedCaptures | Where-Object { $_.selected.analysis.classification -eq 'nonblank' })
    $blankOrLowInfoCaptures = @($analyzedCaptures | Where-Object { $_.selected.analysis.classification -ne 'nonblank' })
    $earlyExitOrCrash = [bool]$script:ProcessInfo['exitedBeforeTimeout'] -or [bool]$script:ProcessInfo['knownFailureEvidenceSeen']
    $screenCapturesTrustworthy = ($script:Display['screenCapturesTrustworthy'] -ne $false)
    $succeeded = [bool]$script:ProcessInfo['survivedToTimeout'] -and ($nonblankCaptures.Count -gt 0) -and (-not $earlyExitOrCrash) -and $screenCapturesTrustworthy

    $reasons = New-Object System.Collections.ArrayList
    if (-not $script:ProcessInfo['survivedToTimeout']) {
        [void]$reasons.Add('Bully.exe did not survive to the configured timeout.')
    }
    if ($earlyExitOrCrash) {
        [void]$reasons.Add('Bully.exe exited early or matching crash evidence was found.')
    }
    if ($analyzedCaptures.Count -eq 0) {
        [void]$reasons.Add('No capture produced an analyzable image.')
    }
    if (-not $screenCapturesTrustworthy) {
        [void]$reasons.Add('Host display preflight failed and -AllowVirtualDisplay was used; screen captures are not trustworthy for a successful visual run.')
    }
    elseif ($nonblankCaptures.Count -eq 0) {
        [void]$reasons.Add('All analyzable captures were blank or low-information.')
    }
    if ($script:RunErrors.Count -gt 0) {
        [void]$reasons.Add('The harness recorded one or more operational errors.')
        $succeeded = $false
    }
    if ($succeeded) {
        [void]$reasons.Add('Bully.exe survived the bounded duration and at least one capture was nonblank.')
    }

    return [ordered]@{
        succeeded                         = $succeeded
        exitCode                          = if ($succeeded) { 0 } else { 1 }
        status                            = if ($succeeded) { 'passed' } elseif (-not $screenCapturesTrustworthy) { 'screen-captures-untrusted' } else { 'failed' }
        analyzedCaptureCount              = $analyzedCaptures.Count
        nonblankCaptureCount              = $nonblankCaptures.Count
        blankOrLowInformationCaptureCount = $blankOrLowInfoCaptures.Count
        processSurvivedToTimeout           = [bool]$script:ProcessInfo['survivedToTimeout']
        earlyExitOrCrashDetected           = $earlyExitOrCrash
        reasons                            = @($reasons)
    }
}

function ConvertTo-DisplaySnapshotSummary {
    param([AllowNull()][object]$Snapshot)

    if ($null -eq $Snapshot) {
        return 'not-recorded'
    }
    if (-not [bool]$Snapshot.available) {
        return ('unavailable: {0}' -f $Snapshot.error)
    }

    $bounds = $Snapshot.bounds
    $modeText = 'mode-unavailable'
    if ($null -ne $Snapshot.mode) {
        $modeText = '{0}x{1} at ({2},{3}), {4}bpp, {5}Hz, orientation={6}, flags={7}' -f `
            $Snapshot.mode.width, $Snapshot.mode.height, $Snapshot.mode.positionX, $Snapshot.mode.positionY, `
            $Snapshot.mode.bitsPerPixel, $Snapshot.mode.displayFrequency, $Snapshot.mode.displayOrientation, $Snapshot.mode.displayFlags
    }
    return ('device={0}; bounds=({1},{2})-({3},{4}) {5}x{6}; {7}' -f `
        $Snapshot.deviceName, $bounds.left, $bounds.top, $bounds.right, $bounds.bottom, $bounds.width, $bounds.height, $modeText)
}

function Write-HumanSummary {
    param([Parameter(Mandatory = $true)][System.Collections.IDictionary]$Outcome)

    $lines = New-Object System.Collections.ArrayList
    [void]$lines.Add('Bully renderer probe')
    [void]$lines.Add(('Run: {0}' -f $RunId))
    [void]$lines.Add(('Result: {0} (status={1}; exit {2})' -f $(if ($Outcome.succeeded) { 'PASS' } else { 'FAIL' }), $Outcome.status, $Outcome.exitCode))
    [void]$lines.Add(('Renderer controls: backend={0}; on12_device={1}; force_swap_effect={2}; force_present_interval={3}; duration={4}s' -f `
        $Backend, $On12Device, $ForceSwapEffect, $ForcePresentInterval, $DurationSeconds))
    [void]$lines.Add(('Renderer INI overrides: activeIniStatus={0}; effective={1}' -f `
        $script:Installation['rendererOverrides']['activeIniStatus'], `
        $(if ($null -ne $script:Installation['rendererOverrides']['activeIniEffective']) { $script:Installation['rendererOverrides']['activeIniEffective'] | ConvertTo-Json -Compress } else { 'not-applied' })))
    $preflight = $script:Display['preflight']
    if ($null -ne $preflight) {
        [void]$lines.Add(('Display preflight: {0}; allowVirtualDisplay={1}; overrideUsed={2}; screenCapturesTrustworthy={3}; session={4}; remote={5}' -f `
            $preflight.status, [bool]$AllowVirtualDisplay, $script:Display['preflightOverrideUsed'], `
            $script:Display['screenCapturesTrustworthy'], $script:Display['sessionName'], $script:Display['remoteSession']))
        [void]$lines.Add('Preflight checks:')
        foreach ($check in @($preflight.checks)) {
            $checkSuffix = if ([string]::IsNullOrWhiteSpace([string]$check.error)) { '' } else { ('; error={0}' -f $check.error) }
            [void]$lines.Add(('  {0}: {1}{2}' -f $check.name, $check.status, $checkSuffix))
        }
        if (@($preflight.failedCapabilities).Count -gt 0) {
            [void]$lines.Add(('  Failed capabilities: {0}' -f (@($preflight.failedCapabilities) -join ', ')))
        }
        if (@($preflight.errors).Count -gt 0) {
            foreach ($preflightError in @($preflight.errors)) {
                [void]$lines.Add(('  Preflight error: {0}' -f $preflightError))
            }
        }
        if (@($preflight.screens).Count -eq 0) {
            [void]$lines.Add('Screen devices: none enumerated')
        }
        else {
            [void]$lines.Add('Screen devices:')
            foreach ($screen in @($preflight.screens)) {
                [void]$lines.Add(('  device={0}; primary={1}; bounds=({2},{3})-({4},{5}) {6}x{7}' -f `
                    $screen.deviceName, $screen.primary, $screen.bounds.left, $screen.bounds.top, `
                    $screen.bounds.right, $screen.bounds.bottom, $screen.bounds.width, $screen.bounds.height))
            }
        }
    }
    [void]$lines.Add(('Display before launch: {0}' -f (ConvertTo-DisplaySnapshotSummary -Snapshot $script:Display['beforeLaunch'])))
    [void]$lines.Add(('Display after termination: {0}' -f (ConvertTo-DisplaySnapshotSummary -Snapshot $script:Display['afterTermination'])))
    [void]$lines.Add(('Display after cleanup: {0}' -f (ConvertTo-DisplaySnapshotSummary -Snapshot $script:Display['afterCleanup'])))
    [void]$lines.Add(('Display restore: decision={0}; attempted={1}; result={2}' -f `
        $script:Display['restoreDecision'], $script:Display['restoreAttempted'], `
        $(if ($null -ne $script:Display['restoreResult']) { $script:Display['restoreResult'].meaning } else { 'not-recorded' })))
    [void]$lines.Add(('Process: launched={0}; survivedToTimeout={1}; exitedBeforeTimeout={2}; terminatedByHarness={3}; processExitCode={4}' -f `
        $script:ProcessInfo.launched, $script:ProcessInfo.survivedToTimeout, $script:ProcessInfo.exitedBeforeTimeout, `
        $script:ProcessInfo.terminatedByHarness, $script:ProcessInfo.exitCode))
    if (@($script:ProcessInfo.faultOffsets).Count -gt 0) {
        [void]$lines.Add(('WER fault offsets: {0}' -f (@($script:ProcessInfo.faultOffsets) -join ', ')))
    }
    [void]$lines.Add('Captures:')
    if ($script:CaptureRecords.Count -eq 0) {
        [void]$lines.Add('  none')
    }
    else {
        foreach ($capture in @($script:CaptureRecords)) {
            $selection = $capture.selected
            $method = if ($null -ne $selection) { $selection.method } else { 'none' }
            $classification = if (($null -ne $selection) -and ($null -ne $selection.analysis)) { $selection.analysis.classification } else { $capture.status }
            [void]$lines.Add(('  {0}s: {1} via {2}; target={3}; fallback={4}' -f `
                $capture.requestedAtSeconds, $classification, $method, $capture.target, $capture.fallbackToPrimaryMonitor))
        }
    }
    $proxyLogEvidence = $script:Artifacts['proxyLog']
    if ($null -ne $proxyLogEvidence) {
        [void]$lines.Add(('Proxy log: path={0}; preRunBytes={1}; postRunBytes={2}; archiveOffset=[{3},{4}); archivedBytes={5}; method={6}' -f `
            $proxyLogEvidence.copiedPath, $proxyLogEvidence.preRunByteLength, $proxyLogEvidence.postRunByteLength, `
            $proxyLogEvidence.archiveStartByteOffset, $proxyLogEvidence.archiveEndByteOffsetExclusive, `
            $proxyLogEvidence.archivedByteLength, $proxyLogEvidence.deltaExtractionMethod))
        [void]$lines.Add(('  Log hashes: pre={0}; post={1}; archived={2}; prefix={3}' -f `
            $proxyLogEvidence.preRunPrefixSha256, $proxyLogEvidence.postRunSha256, $proxyLogEvidence.sha256, $proxyLogEvidence.prefixVerification))
        if (-not [string]::IsNullOrWhiteSpace([string]$proxyLogEvidence.deltaExtractionReason)) {
            [void]$lines.Add(('  Log extraction: {0}' -f $proxyLogEvidence.deltaExtractionReason))
        }
    }
    [void]$lines.Add(('Backbuffer BMP: {0}' -f $(if ($null -ne $script:BackbufferAnalysis) { $script:BackbufferAnalysis.classification } else { 'not-collected' })))
    [void]$lines.Add(('Frontbuffer BMP: {0}' -f $(if ($null -ne $script:FrontbufferAnalysis) { $script:FrontbufferAnalysis.classification } else { 'not-collected' })))
    [void]$lines.Add(('Report: {0}' -f $ReportPath))
    [void]$lines.Add(('WER events: {0}' -f $WerPath))
    if ($script:RunWarnings.Count -gt 0) {
        [void]$lines.Add('Warnings:')
        foreach ($warning in @($script:RunWarnings)) {
            [void]$lines.Add(('  {0}' -f $warning))
        }
    }
    if ($script:RunErrors.Count -gt 0) {
        [void]$lines.Add('Errors:')
        foreach ($errorMessage in @($script:RunErrors)) {
            [void]$lines.Add(('  {0}' -f $errorMessage))
        }
    }
    [System.IO.File]::WriteAllLines($SummaryPath, [string[]]$lines, $Utf8NoBom)
}

function Write-FinalReport {
    param([Parameter(Mandatory = $true)][System.Collections.IDictionary]$Outcome)

    $report = [ordered]@{
        schemaVersion = 1
        run = [ordered]@{
            id               = $RunId
            startedUtc       = $RunStart.ToUniversalTime().ToString('o')
            finishedUtc      = if ($null -ne $script:RunEnd) { $script:RunEnd.ToUniversalTime().ToString('o') } else { (Get-Date).ToUniversalTime().ToString('o') }
            projectRoot      = $ProjectRoot
            runDirectory     = $RunDirectory
            backend          = $Backend
            on12Device       = $On12Device
            forceSwapEffect  = $ForceSwapEffect
            forcePresentInterval = $ForcePresentInterval
            presentationControls = $RequestedPresentationControls
            durationSeconds  = $DurationSeconds
            captureAtSeconds = $NormalizedCaptureAtSeconds
            noInstall        = [bool]$NoInstall
            forceInstall     = [bool]$ForceInstall
            allowVirtualDisplay = [bool]$AllowVirtualDisplay
            thresholds       = $Thresholds
        }
        outcome              = $Outcome
        installation         = $script:Installation
        display              = $script:Display
        process              = $script:ProcessInfo
        captures             = @($script:CaptureRecords)
        observations         = @($script:Observations)
        artifacts            = $script:Artifacts
        proxyBackbuffer      = $script:BackbufferAnalysis
        proxyFrontbuffer     = $script:FrontbufferAnalysis
        werApplicationErrors = [ordered]@{
            artifactPath             = $WerPath
            count                    = @($script:WerRecords).Count
            knownFailureEvidenceSeen = $script:ProcessInfo['knownFailureEvidenceSeen']
        }
        warnings             = @($script:RunWarnings)
        errors               = @($script:RunErrors)
        fatalError           = $script:FatalError
        activity             = @($script:Activity)
    }
    Write-JsonFile -Path $ReportPath -Object $report -Depth 16
}

if ($ValidateDisplayOnly) {
    $validationFailed = $false
    try {
        Initialize-NativeHelper -SuppressLog
        $preflight = Invoke-DisplayPreflight
        $script:Display['screenCapturesTrustworthy'] = [bool]$preflight.passed
        [pscustomobject][ordered]@{
            mode                = 'validate-display-only'
            allowVirtualDisplay = [bool]$AllowVirtualDisplay
            display             = $script:Display
            preflight           = $preflight
        } | ConvertTo-Json -Depth 16
        if (-not [bool]$preflight.passed) {
            $validationFailed = $true
        }
    }
    catch {
        $validationFailed = $true
        [pscustomobject][ordered]@{
            mode                = 'validate-display-only'
            allowVirtualDisplay = [bool]$AllowVirtualDisplay
            error               = $_.Exception.Message
            errorType           = $_.Exception.GetType().FullName
        } | ConvertTo-Json -Depth 8
    }
    exit $(if ($validationFailed) { 1 } else { 0 })
}

try {
    Add-RunLog -Level 'info' -Message ('Starting render probe {0}.' -f $RunId)
    Initialize-NativeHelper
    $preflight = Invoke-DisplayPreflight
    $script:Display['screenCapturesTrustworthy'] = [bool]$preflight.passed
    try {
        $script:Display['beforeLaunch'] = Get-PrimaryDisplaySnapshot
    }
    catch {
        Add-RunWarning -Message ('Could not record primary display state before launch. {0}' -f $_.Exception.Message)
    }

    if (-not [bool]$preflight.passed) {
        if ($AllowVirtualDisplay) {
            $script:PreflightOverridden = $true
            $script:Display['preflightOverrideUsed'] = $true
            $script:Display['screenCapturesTrustworthy'] = $false
            Add-RunWarning -Message ('Display preflight failed but -AllowVirtualDisplay was supplied. Screen captures are not trustworthy: {0}' -f ($preflight.failedCapabilities -join ', '))
        }
        else {
            $script:PreflightBlocked = $true
            $script:Installation['status'] = 'skipped-preflight-failed'
            Add-RunError -Message ('preflight-failed: display safety checks failed; no game files were staged and Bully.exe was not launched. Failed checks: {0}' -f ($preflight.failedCapabilities -join ', '))
        }
    }

    if (-not $script:PreflightBlocked) {
        $script:Artifacts['preRun'] = [ordered]@{
            proxyLog      = Get-FileSnapshot -Path $ProxyLogPath
            backbufferBmp = Get-FileSnapshot -Path $BackbufferBmpPath
            frontbufferBmp = Get-FileSnapshot -Path $FrontbufferBmpPath
        }
        if (($null -ne $script:Display['beforeLaunch']) -and [bool]$script:Display['beforeLaunch'].available) {
            Add-RunLog -Level 'info' -Message ('Primary display before launch: {0}x{1} at ({2},{3}).' -f `
                $script:Display['beforeLaunch'].bounds.width, $script:Display['beforeLaunch'].bounds.height, `
                $script:Display['beforeLaunch'].bounds.left, $script:Display['beforeLaunch'].bounds.top)
        }

        if ($NoInstall) {
            $script:Installation['status'] = 'skipped-no-install'
            $script:Installation['gameD3D9BeforeSha256'] = Get-Sha256 -Path $GameD3D9Path
            $script:Installation['gameIniBeforeSha256'] = Get-Sha256 -Path $GameIniPath
            $script:Installation['rendererOverrides']['activeIniStatus'] = 'not-applied-no-install'
            Add-RunWarning -Message ('-NoInstall was supplied; the launcher will use the game folder exactly as it exists. Requested renderer controls were not applied: backend={0}; on12_device={1}; force_swap_effect={2}; force_present_interval={3}.' -f `
                $Backend, $On12Device, $ForceSwapEffect, $ForcePresentInterval)
        }
        else {
            Install-RendererProxy
        }

        Invoke-GameProbe
    }
}
catch {
    $script:FatalError = [ordered]@{
        message = $_.Exception.Message
        type    = $_.Exception.GetType().FullName
        stack   = $_.ScriptStackTrace
    }
    Add-RunError -Message ('Unhandled probe error: {0}' -f $_.Exception.Message)
}
finally {
    try {
        if ($script:RunStartedProcess) {
            $state = Get-ProcessState
            if ($state.alive) {
                Stop-GameProcess -Reason 'finally-cleanup'
            }
            else {
                Set-ProcessExitMetadata
            }
        }
    }
    catch {
        Add-RunError -Message ('Process cleanup failed. {0}' -f $_.Exception.Message)
    }

    try {
        Collect-RuntimeArtifacts
    }
    catch {
        Add-RunError -Message ('Artifact collection failed. {0}' -f $_.Exception.Message)
    }

    try {
        Collect-RecentWerApplicationErrors
    }
    catch {
        Add-RunError -Message ('WER collection failed. {0}' -f $_.Exception.Message)
    }

    try {
        Restore-StagedGameFile -State $script:IniInstallState -DestinationPath $GameIniPath -Label 'renderer-ini'
        Restore-StagedGameFile -State $script:DllInstallState -DestinationPath $GameD3D9Path -Label 'd3d9'
        $script:Installation['gameIniAfterCleanupSha256'] = Get-Sha256 -Path $GameIniPath
        $script:Installation['gameD3D9AfterCleanupSha256'] = Get-Sha256 -Path $GameD3D9Path
        if ($script:Installation['status'] -eq 'installed') {
            $script:Installation['status'] = 'installed-and-restored'
        }
    }
    catch {
        Add-RunError -Message ('Game file restoration orchestration failed. {0}' -f $_.Exception.Message)
    }

    try {
        Restore-DisplaySettings
    }
    catch {
        Add-RunError -Message ('Display cleanup failed. {0}' -f $_.Exception.Message)
    }

    $script:RunEnd = Get-Date
    try {
        $outcome = Get-ProbeOutcome
        $script:ExitCode = [int]$outcome.exitCode
        Write-FinalReport -Outcome $outcome
        Write-HumanSummary -Outcome $outcome
        Add-RunLog -Level 'info' -Message ('Probe finished with exit code {0}. Report: {1}' -f $script:ExitCode, $ReportPath)
    }
    catch {
        $script:ExitCode = 1
        try {
            $fallback = [ordered]@{
                fatalReportWriteError = $_.Exception.Message
                runId                  = $RunId
                runDirectory           = $RunDirectory
            }
            Write-JsonFile -Path $ReportPath -Object $fallback -Depth 4
        }
        catch { }
        Write-Error ('render_probe could not write its final report: {0}' -f $_.Exception.Message)
    }
}

exit $script:ExitCode
