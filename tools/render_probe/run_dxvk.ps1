# Requires Windows PowerShell 5.1+ or PowerShell 7+ on Windows.
# Stages an x86 DXVK d3d9.dll only for a bounded active-console probe.
[CmdletBinding()]
param(
    # Relative paths are resolved from the repository root. When omitted, the
    # ignored local DXVK dependency is used if it exists.
    [Alias('DxvkD3D9Path')]
    [string]$DxvkDllPath,

    [ValidatePattern('^[0-9a-fA-F]{64}$')]
    [string]$ExpectedSha256,

    [ValidateRange(1, 3600)]
    [int]$DurationSeconds = 35,

    [string[]]$CaptureAtSeconds = @('5', '15', '30'),

    # Passes through the harness's narrow opt-in for an unknown existing proxy.
    [switch]$ForceInstall,

    # Required for runtime because run_active.ps1 can launch Bully.exe on the
    # currently unlocked physical console desktop.
    [switch]$AllowActiveDesktopLaunch,

    # Verifies source eligibility and normalized controls without changing the
    # game folder, creating output, scheduling a task, or launching Bully.exe.
    [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'

$ScriptDirectory = $PSScriptRoot
$ProjectRoot = (Resolve-Path -LiteralPath (Join-Path $ScriptDirectory '..\..')).Path
$GameDirectory = Join-Path $ProjectRoot 'Bully Scholarship Edition'
$BridgePath = Join-Path $ScriptDirectory 'run_active.ps1'
$DefaultDxvkDllPath = Join-Path $ProjectRoot 'deps\dxvk-3.0.2\x32\d3d9.dll'
$DxvkD3D9Path = Join-Path $GameDirectory 'dxvk_d3d9.dll'
$DxvkConfigPath = Join-Path $GameDirectory 'dxvk.conf'
$DxvkLogPath = Join-Path $GameDirectory 'Bully_d3d9.log'
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$DxvkConfigText = "dxvk.hud = devinfo,version,api`r`n"

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    $stream = $null
    try {
        $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
        return ([System.BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    }
    finally {
        if ($null -ne $stream) {
            $stream.Dispose()
        }
        $algorithm.Dispose()
    }
}

function Resolve-DxvkSourcePath {
    param([AllowNull()][string]$RequestedPath)

    $candidate = $RequestedPath
    if ([string]::IsNullOrWhiteSpace($candidate)) {
        $candidate = $DefaultDxvkDllPath
    }
    elseif (-not [System.IO.Path]::IsPathRooted($candidate)) {
        $candidate = Join-Path $ProjectRoot $candidate
    }

    $resolvedPath = [System.IO.Path]::GetFullPath($candidate)
    if (-not (Test-Path -LiteralPath $resolvedPath -PathType Leaf)) {
        if ([string]::IsNullOrWhiteSpace($RequestedPath)) {
            throw ('No DXVK DLL was supplied and the default source is missing: {0}. Supply -DxvkDllPath with an x86 DXVK d3d9.dll.' -f $resolvedPath)
        }
        throw ('DXVK DLL source is missing: {0}' -f $resolvedPath)
    }

    return $resolvedPath
}

function Get-PeArchitecture {
    param([Parameter(Mandatory = $true)][string]$Path)

    $stream = $null
    $reader = $null
    try {
        $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
        if ($stream.Length -lt 64) {
            throw 'file is shorter than the DOS header.'
        }

        $reader = New-Object System.IO.BinaryReader($stream, [System.Text.Encoding]::ASCII, $true)
        if ($reader.ReadUInt16() -ne [uint16]0x5A4D) {
            throw 'DOS signature MZ is missing.'
        }

        $stream.Position = 0x3C
        $peOffset = [int64]$reader.ReadUInt32()
        if (($peOffset -lt 0x40) -or ($peOffset -gt ($stream.Length - 24))) {
            throw 'PE header offset is outside the file.'
        }

        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne [uint32]0x00004550) {
            throw 'PE signature is missing.'
        }

        $machine = [uint16]$reader.ReadUInt16()
        $stream.Position = $peOffset + 20
        $optionalHeaderSize = [uint16]$reader.ReadUInt16()
        $optionalHeaderOffset = $peOffset + 24
        if (($optionalHeaderSize -lt 2) -or ($optionalHeaderOffset -gt ($stream.Length - 2))) {
            throw 'optional PE header is incomplete.'
        }

        $stream.Position = $optionalHeaderOffset
        $optionalHeaderMagic = [uint16]$reader.ReadUInt16()
        $machineName = if ($machine -eq [uint16]0x014C) { 'I386' } else { ('0x{0:X4}' -f $machine) }
        $formatName = if ($optionalHeaderMagic -eq [uint16]0x010B) { 'PE32' } elseif ($optionalHeaderMagic -eq [uint16]0x020B) { 'PE32+' } else { ('0x{0:X4}' -f $optionalHeaderMagic) }

        return [pscustomobject][ordered]@{
            machine             = ('0x{0:X4}' -f $machine)
            machineName         = $machineName
            optionalHeaderMagic = ('0x{0:X4}' -f $optionalHeaderMagic)
            format              = $formatName
            isX86               = (($machine -eq [uint16]0x014C) -and ($optionalHeaderMagic -eq [uint16]0x010B))
        }
    }
    catch {
        throw ('Could not verify PE architecture for {0}: {1}' -f $Path, $_.Exception.Message)
    }
    finally {
        if ($null -ne $reader) {
            $reader.Dispose()
        }
        if ($null -ne $stream) {
            $stream.Dispose()
        }
    }
}

function ConvertTo-BoundedCaptureTimes {
    [OutputType([string[]])]
    param(
        [Parameter(Mandatory = $true)][string[]]$InputValues,
        [Parameter(Mandatory = $true)][int]$MaximumSeconds
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
            if ($token -notmatch '^\d+$') {
                throw ("Invalid CaptureAtSeconds token '{0}'. Use non-negative base-10 integers separated by commas, semicolons, or whitespace." -f $token)
            }

            $parsed = 0
            if (-not [int]::TryParse($token, [System.Globalization.NumberStyles]::None, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$parsed)) {
                throw ("CaptureAtSeconds token '{0}' is outside the Int32 range." -f $token)
            }
            if ($parsed -gt $MaximumSeconds) {
                throw ("CaptureAtSeconds token '{0}' exceeds DurationSeconds ({1})." -f $token, $MaximumSeconds)
            }
            [void]$normalized.Add($parsed)
        }
    }

    if ($normalized.Count -eq 0) {
        throw 'CaptureAtSeconds must contain at least one non-negative base-10 integer.'
    }

    return [string[]]@($normalized | Sort-Object -Unique | ForEach-Object {
        $_.ToString([System.Globalization.CultureInfo]::InvariantCulture)
    })
}

function Get-BullyProcessIds {
    return @(Get-Process -Name Bully -ErrorAction SilentlyContinue | ForEach-Object { [int]$_.Id })
}

function New-FileState {
    param([Parameter(Mandatory = $true)][string]$Path)

    return [ordered]@{
        path                 = $Path
        originalExisted      = $false
        originalSha256       = $null
        backupPath           = $null
        stageAttempted       = $false
        stagedSha256         = $null
        restoreStatus        = 'not-attempted'
        restorationSucceeded = $false
        error                = $null
    }
}

function Save-OriginalFile {
    param(
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$State,
        [Parameter(Mandatory = $true)][string]$BackupPath
    )

    $path = [string]$State['path']
    if (Test-Path -LiteralPath $path -PathType Container) {
        throw ('Expected a file but found a directory at {0}.' -f $path)
    }

    $State['originalExisted'] = Test-Path -LiteralPath $path -PathType Leaf
    if (-not $State['originalExisted']) {
        return
    }

    $State['originalSha256'] = Get-Sha256 -Path $path
    Copy-Item -LiteralPath $path -Destination $BackupPath -Force
    if ((Get-Sha256 -Path $BackupPath) -ne $State['originalSha256']) {
        throw ('Backup hash mismatch for {0}; refusing to stage a replacement.' -f $path)
    }
    $State['backupPath'] = $BackupPath
}

function Replace-FileFromSource {
    param(
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$DestinationPath,
        [Parameter(Mandatory = $true)][string]$ExpectedHash,
        [AllowNull()][string]$ExpectedDestinationHash,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $destinationDirectory = Split-Path -Path $DestinationPath -Parent
    $temporaryPath = Join-Path $destinationDirectory ('.render-probe-dxvk-{0}-{1}.tmp' -f $PID, $Label)
    $replaceBackupPath = $temporaryPath + '.backup'
    try {
        Copy-Item -LiteralPath $SourcePath -Destination $temporaryPath -Force
        if ((Get-Sha256 -Path $temporaryPath) -ne $ExpectedHash) {
            throw ('Temporary {0} hash mismatch.' -f $Label)
        }

        if (Test-Path -LiteralPath $DestinationPath -PathType Leaf) {
            if ((-not [string]::IsNullOrWhiteSpace($ExpectedDestinationHash)) -and
                ((Get-Sha256 -Path $DestinationPath) -ne $ExpectedDestinationHash)) {
                throw ('{0} changed before it could be replaced.' -f $DestinationPath)
            }
            try {
                [System.IO.File]::Replace($temporaryPath, $DestinationPath, $replaceBackupPath)
            }
            catch {
                # File.Replace is unavailable on some local filesystems. The
                # caller has already verified ownership before this fallback.
                if ((-not [string]::IsNullOrWhiteSpace($ExpectedDestinationHash)) -and
                    ((Get-Sha256 -Path $DestinationPath) -ne $ExpectedDestinationHash)) {
                    throw ('{0} changed while atomic replacement was attempted.' -f $DestinationPath)
                }
                Copy-Item -LiteralPath $temporaryPath -Destination $DestinationPath -Force
                Remove-Item -LiteralPath $temporaryPath -Force -ErrorAction SilentlyContinue
            }
        }
        elseif (Test-Path -LiteralPath $DestinationPath) {
            throw ('Expected a file destination but found another item at {0}.' -f $DestinationPath)
        }
        else {
            [System.IO.File]::Move($temporaryPath, $DestinationPath)
        }

        if ((Get-Sha256 -Path $DestinationPath) -ne $ExpectedHash) {
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

function Stage-VerifiedFile {
    param(
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$State,
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$SourceHash,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $destinationPath = [string]$State['path']
    if ($State['originalExisted']) {
        if ((Get-Sha256 -Path $destinationPath) -ne $State['originalSha256']) {
            throw ('{0} changed after its backup was verified; refusing to stage over it.' -f $destinationPath)
        }
    }
    elseif (Test-Path -LiteralPath $destinationPath) {
        throw ('{0} appeared after its absence was recorded; refusing to stage over it.' -f $destinationPath)
    }

    $State['stageAttempted'] = $true
    $State['stagedSha256'] = $SourceHash
    $expectedDestinationHash = if ($State['originalExisted']) { [string]$State['originalSha256'] } else { $null }
    Replace-FileFromSource -SourcePath $SourcePath -DestinationPath $destinationPath -ExpectedHash $SourceHash -ExpectedDestinationHash $expectedDestinationHash -Label $Label
}

function Clear-DxvkLogForRun {
    param([Parameter(Mandatory = $true)][System.Collections.IDictionary]$State)

    $State['stageAttempted'] = $true
    $path = [string]$State['path']
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        Remove-Item -LiteralPath $path -Force
    }
    if (Test-Path -LiteralPath $path) {
        throw ('Could not clear DXVK log path before launch: {0}' -f $path)
    }
}

function Restore-StagedFile {
    param([Parameter(Mandatory = $true)][System.Collections.IDictionary]$State)

    if (-not $State['stageAttempted']) {
        $State['restoreStatus'] = 'not-staged'
        $State['restorationSucceeded'] = $true
        return
    }

    $path = [string]$State['path']
    try {
        $exists = Test-Path -LiteralPath $path -PathType Leaf
        $currentHash = if ($exists) { Get-Sha256 -Path $path } else { $null }
        if ($State['originalExisted'] -and ($currentHash -eq $State['originalSha256'])) {
            $State['restoreStatus'] = 'already-original'
            $State['restorationSucceeded'] = $true
            return
        }
        if ((-not $State['originalExisted']) -and (-not $exists)) {
            $State['restoreStatus'] = 'already-absent'
            $State['restorationSucceeded'] = $true
            return
        }
        if ($currentHash -ne $State['stagedSha256']) {
            $State['restoreStatus'] = 'skipped-current-file-did-not-match-helper-stage'
            $State['error'] = 'The current file did not match the helper-installed hash.'
            return
        }

        if ($State['originalExisted']) {
            Replace-FileFromSource -SourcePath $State['backupPath'] -DestinationPath $path -ExpectedHash $State['originalSha256'] -ExpectedDestinationHash $State['stagedSha256'] -Label ('restore-' + (Split-Path -Leaf $path))
            $State['restoreStatus'] = 'restored-original'
        }
        else {
            Remove-Item -LiteralPath $path -Force
            $State['restoreStatus'] = 'removed-helper-created-file'
        }
        $State['restorationSucceeded'] = $true
    }
    catch {
        $State['restoreStatus'] = 'restore-failed'
        $State['error'] = $_.Exception.Message
    }
}

function Restore-DxvkLog {
    param([Parameter(Mandatory = $true)][System.Collections.IDictionary]$State)

    if (-not $State['stageAttempted']) {
        $State['restoreStatus'] = 'not-staged'
        $State['restorationSucceeded'] = $true
        return
    }

    $path = [string]$State['path']
    try {
        if ($State['originalExisted']) {
            Replace-FileFromSource -SourcePath $State['backupPath'] -DestinationPath $path -ExpectedHash $State['originalSha256'] -Label 'restore-Bully_d3d9.log'
            $State['restoreStatus'] = 'restored-original'
        }
        elseif (Test-Path -LiteralPath $path -PathType Leaf) {
            Remove-Item -LiteralPath $path -Force
            $State['restoreStatus'] = 'removed-helper-created-log'
        }
        else {
            $State['restoreStatus'] = 'already-absent'
        }
        $State['restorationSucceeded'] = $true
    }
    catch {
        $State['restoreStatus'] = 'restore-failed'
        $State['error'] = $_.Exception.Message
    }
}

function Copy-DxvkLogToOutput {
    param(
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$OutputPath
    )

    if (-not (Test-Path -LiteralPath $SourcePath -PathType Leaf)) {
        return $null
    }

    Copy-Item -LiteralPath $SourcePath -Destination $OutputPath -Force
    return $OutputPath
}

function Get-ChildReportPathFromBridgeOutput {
    param([Parameter(Mandatory = $true)][string]$StdoutPath)

    if (-not (Test-Path -LiteralPath $StdoutPath -PathType Leaf)) {
        return $null
    }

    $reportPath = $null
    foreach ($line in [System.IO.File]::ReadAllLines($StdoutPath)) {
        $candidate = $line.Trim()
        if (($candidate.Length -lt 2) -or (-not $candidate.StartsWith('{')) -or (-not $candidate.EndsWith('}'))) {
            continue
        }

        try {
            $bridgeResult = $candidate | ConvertFrom-Json -ErrorAction Stop
            if (($bridgeResult.mode -eq 'active-console-bridge') -and
                ($null -ne $bridgeResult.PSObject.Properties['reportPath']) -and
                (-not [string]::IsNullOrWhiteSpace([string]$bridgeResult.reportPath))) {
                $reportPath = [string]$bridgeResult.reportPath
            }
        }
        catch {
            # Other child output is not a bridge result. Do not infer a path.
        }
    }

    return $reportPath
}

function Stop-NewBullyProcesses {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [int[]]$OriginalProcessIds
    )

    $newProcessIds = @(Get-BullyProcessIds | Where-Object { $OriginalProcessIds -notcontains $_ })
    $stoppedProcessIds = New-Object 'System.Collections.Generic.List[int]'
    $errors = New-Object 'System.Collections.Generic.List[string]'
    foreach ($processId in $newProcessIds) {
        try {
            Stop-Process -Id $processId -Force -ErrorAction Stop
            [void]$stoppedProcessIds.Add($processId)
        }
        catch {
            [void]$errors.Add(('Could not stop Bully.exe PID {0}: {1}' -f $processId, $_.Exception.Message))
        }
    }

    Start-Sleep -Milliseconds 200
    $remainingProcessIds = @(Get-BullyProcessIds | Where-Object { $OriginalProcessIds -notcontains $_ })
    return [ordered]@{
        foundProcessIds     = @($newProcessIds)
        stoppedProcessIds   = @($stoppedProcessIds)
        remainingProcessIds = @($remainingProcessIds)
        errors              = @($errors)
        succeeded           = (($remainingProcessIds.Count -eq 0) -and ($errors.Count -eq 0))
    }
}

function Write-AtomicJson {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object]$Object
    )

    $temporaryPath = '{0}.{1}.{2}.tmp' -f $Path, $PID, [Guid]::NewGuid().ToString('N')
    try {
        [System.IO.File]::WriteAllText($temporaryPath, ($Object | ConvertTo-Json -Depth 10), $Utf8NoBom)
        Move-Item -LiteralPath $temporaryPath -Destination $Path -Force
    }
    finally {
        if (Test-Path -LiteralPath $temporaryPath -PathType Leaf) {
            Remove-Item -LiteralPath $temporaryPath -Force -ErrorAction SilentlyContinue
        }
    }
}

$resolvedDxvkDllPath = $null
$sourceSha256 = $null
$sourceArchitecture = $null
$normalizedCaptureTimes = $null
$runId = $null
$outputDirectory = $null
$stateDirectory = $null
$manifestPath = $null
$bridgeStdoutPath = $null
$bridgeStderrPath = $null
$configArtifactPath = $null
$capturedDxvkLogPath = $null
$childReportPath = $null
$childExitCode = $null
$runtimeError = $null
$exitCode = 1
$originalBullyProcessIds = @()
$processOwnershipEstablished = $false
$processCleanup = [ordered]@{
    foundProcessIds     = @()
    stoppedProcessIds   = @()
    remainingProcessIds = @()
    errors              = @()
    succeeded           = $true
    status              = 'not-needed'
}
$dxvkDllState = New-FileState -Path $DxvkD3D9Path
$dxvkConfigState = New-FileState -Path $DxvkConfigPath
$dxvkLogState = New-FileState -Path $DxvkLogPath
$restorationSucceeded = $false

try {
    $normalizedCaptureTimes = [string[]](ConvertTo-BoundedCaptureTimes -InputValues $CaptureAtSeconds -MaximumSeconds $DurationSeconds)
    $resolvedDxvkDllPath = Resolve-DxvkSourcePath -RequestedPath $DxvkDllPath
    $sourceSha256 = Get-Sha256 -Path $resolvedDxvkDllPath
    if (-not $ValidateOnly) {
        $runId = 'dxvk-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-pid' + $PID + '-' + [Guid]::NewGuid().ToString('N')
        $outputDirectory = Join-Path $ProjectRoot (Join-Path 'dump\render-probe' $runId)
        $stateDirectory = Join-Path $outputDirectory 'state'
        $manifestPath = Join-Path $outputDirectory 'dxvk-manifest.json'
        $bridgeStdoutPath = Join-Path $outputDirectory 'bridge-stdout.txt'
        $bridgeStderrPath = Join-Path $outputDirectory 'bridge-stderr.txt'
        $configArtifactPath = Join-Path $outputDirectory 'active-dxvk.conf'
        New-Item -ItemType Directory -Path $stateDirectory -Force | Out-Null
    }
    $sourceArchitecture = Get-PeArchitecture -Path $resolvedDxvkDllPath
    if (-not [bool]$sourceArchitecture.isX86) {
        throw ('DXVK source is not a verified x86 PE32 DLL (machine={0}, format={1}): {2}' -f $sourceArchitecture.machine, $sourceArchitecture.format, $resolvedDxvkDllPath)
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedSha256) -and
        (-not [string]::Equals($sourceSha256, $ExpectedSha256, [System.StringComparison]::OrdinalIgnoreCase))) {
        throw ('DXVK source SHA-256 mismatch. Expected {0}; actual {1}.' -f $ExpectedSha256.ToLowerInvariant(), $sourceSha256)
    }
    if (-not (Test-Path -LiteralPath $BridgePath -PathType Leaf)) {
        throw ('Active-console bridge is missing: {0}' -f $BridgePath)
    }

    if ($ValidateOnly) {
        [pscustomobject][ordered]@{
            mode                  = 'validate-only'
            sourcePath            = $resolvedDxvkDllPath
            sourceSha256          = $sourceSha256
            sourceArchitecture    = $sourceArchitecture
            durationSeconds       = $DurationSeconds
            captureAtSeconds      = $normalizedCaptureTimes
            bridgePath            = $BridgePath
            gameFolderMutated     = $false
            activeDesktopLaunched = $false
        } | ConvertTo-Json -Depth 6
        $exitCode = 0
    }
    else {
        if (-not $AllowActiveDesktopLaunch) {
            throw 'This helper can launch Bully.exe on the active physical console desktop. Re-run with -AllowActiveDesktopLaunch after confirming that desktop is safe to use.'
        }
        $bullyExePath = Join-Path $GameDirectory 'Bully.exe'
        if (-not (Test-Path -LiteralPath $bullyExePath -PathType Leaf)) {
            throw ('Bully.exe is missing: {0}' -f $bullyExePath)
        }

        $originalBullyProcessIds = @(Get-BullyProcessIds)
        if ($originalBullyProcessIds.Count -gt 0) {
            throw ('Bully.exe is already running (PID(s): {0}); close it before starting a DXVK probe.' -f ($originalBullyProcessIds -join ', '))
        }
        $processOwnershipEstablished = $true

        [System.IO.File]::WriteAllText($configArtifactPath, $DxvkConfigText, [System.Text.Encoding]::ASCII)
        $configSha256 = Get-Sha256 -Path $configArtifactPath

        Save-OriginalFile -State $dxvkDllState -BackupPath (Join-Path $stateDirectory 'original-dxvk_d3d9.dll')
        Save-OriginalFile -State $dxvkConfigState -BackupPath (Join-Path $stateDirectory 'original-dxvk.conf')
        Save-OriginalFile -State $dxvkLogState -BackupPath (Join-Path $stateDirectory 'original-Bully_d3d9.log')

        Stage-VerifiedFile -State $dxvkDllState -SourcePath $resolvedDxvkDllPath -SourceHash $sourceSha256 -Label 'dxvk_d3d9.dll'
        Stage-VerifiedFile -State $dxvkConfigState -SourcePath $configArtifactPath -SourceHash $configSha256 -Label 'dxvk.conf'
        Clear-DxvkLogForRun -State $dxvkLogState

        $bridgeParameters = @{
            Backend                  = 'dxvk'
            DurationSeconds          = $DurationSeconds
            CaptureAtSeconds         = $normalizedCaptureTimes
            AllowActiveDesktopLaunch = $true
        }
        if ($ForceInstall) {
            $bridgeParameters['ForceInstall'] = $true
        }

        & $BridgePath @bridgeParameters 1> $bridgeStdoutPath 2> $bridgeStderrPath
        $childExitCode = [int]$LASTEXITCODE
        $childReportPath = Get-ChildReportPathFromBridgeOutput -StdoutPath $bridgeStdoutPath
        $exitCode = $childExitCode
    }
}
catch {
    $runtimeError = $_.Exception.Message
    [Console]::Error.WriteLine(('run_dxvk.ps1 failed: {0}' -f $runtimeError))
    $exitCode = 1
}
finally {
    if (-not $ValidateOnly) {
        if (($null -ne $bridgeStdoutPath) -and (Test-Path -LiteralPath $bridgeStdoutPath -PathType Leaf)) {
            $childReportPath = Get-ChildReportPathFromBridgeOutput -StdoutPath $bridgeStdoutPath
        }

        if ($processOwnershipEstablished) {
            try {
                $processCleanup = Stop-NewBullyProcesses -OriginalProcessIds $originalBullyProcessIds
            }
            catch {
                $processCleanup = [ordered]@{
                    foundProcessIds     = @()
                    stoppedProcessIds   = @()
                    remainingProcessIds = @()
                    errors              = @($_.Exception.Message)
                    succeeded           = $false
                    status              = 'cleanup-failed'
                }
            }
        }

        if ($null -ne $outputDirectory) {
            if ($dxvkLogState['stageAttempted']) {
                try {
                    $capturedDxvkLogPath = Copy-DxvkLogToOutput -SourcePath $DxvkLogPath -OutputPath (Join-Path $outputDirectory 'Bully_d3d9.log')
                }
                catch {
                    $dxvkLogState['error'] = ('Could not archive run DXVK log: {0}' -f $_.Exception.Message)
                }
            }

            Restore-DxvkLog -State $dxvkLogState
            Restore-StagedFile -State $dxvkConfigState
            Restore-StagedFile -State $dxvkDllState

            $restorationSucceeded = [bool]$dxvkDllState['restorationSucceeded'] -and
                [bool]$dxvkConfigState['restorationSucceeded'] -and
                [bool]$dxvkLogState['restorationSucceeded'] -and
                (($null -ne $processCleanup) -and [bool]$processCleanup['succeeded'])
            if (-not $restorationSucceeded) {
                $exitCode = 2
            }

            $manifest = [ordered]@{
                schemaVersion       = 1
                mode                = 'dxvk-active-console-probe'
                runId               = $runId
                sourcePath          = $resolvedDxvkDllPath
                sourceSha256        = $sourceSha256
                expectedSha256      = if ([string]::IsNullOrWhiteSpace($ExpectedSha256)) { $null } else { $ExpectedSha256.ToLowerInvariant() }
                sourceArchitecture  = $sourceArchitecture
                stagedPaths         = [ordered]@{
                    dxvkD3D9 = $DxvkD3D9Path
                    dxvkConf = $DxvkConfigPath
                    dxvkLog  = $DxvkLogPath
                }
                bridgeStdoutPath    = $bridgeStdoutPath
                bridgeStderrPath    = $bridgeStderrPath
                childReportPath     = $childReportPath
                childExitCode       = $childExitCode
                exitCode            = $exitCode
                restoration         = [ordered]@{
                    succeeded      = $restorationSucceeded
                    dxvkD3D9       = $dxvkDllState
                    dxvkConf       = $dxvkConfigState
                    dxvkLog        = $dxvkLogState
                    processCleanup = $processCleanup
                }
                capturedDxvkLogPath = $capturedDxvkLogPath
                error               = $runtimeError
                finishedUtc         = (Get-Date).ToUniversalTime().ToString('o')
            }
            try {
                Write-AtomicJson -Path $manifestPath -Object $manifest
            }
            catch {
                [Console]::Error.WriteLine(('run_dxvk.ps1 could not write its manifest: {0}' -f $_.Exception.Message))
                $exitCode = 2
            }
        }
    }
}

exit $exitCode
