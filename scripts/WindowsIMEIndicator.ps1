#Requires -Version 5.1
#Requires -Modules International
<#
.SYNOPSIS
Opens a menu to control the Windows input indicator for the current user.
.DESCRIPTION
Hide and Show select the legacy language bar and set ShowStatus to 3 or 0.
Taskbar selects the standard Windows taskbar input indicator. Status only reads
the current settings. With no action, an interactive menu is displayed.
The existing per-application switching mode is preserved.
If Windows denies stopping ctfmon, a separate helper requests administrator
approval for that step only. Settings and startup stay in the original user context.
Exit IME Steady before changing settings, then activate Pinyin and launch it again.
.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\WindowsIMEIndicator.ps1
.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\WindowsIMEIndicator.ps1 Taskbar
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Position = 0)]
    [ValidateSet('Menu', 'Hide', 'Show', 'Taskbar', 'Status')]
    [string]$Action = 'Menu',
    [Parameter(DontShow = $true)]
    [switch]$StopTextServices,
    [Parameter(DontShow = $true)]
    [int]$TargetSessionId = -1
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$languageBarPath = 'HKCU:\Software\Microsoft\CTF\LangBar'
$helperScriptPath = $PSCommandPath

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    try {
        $principal = [Security.Principal.WindowsPrincipal]::new($identity)
        return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    } finally { $identity.Dispose() }
}

function Test-AccessDenied {
    param([Management.Automation.ErrorRecord]$Failure)

    $exception = $Failure.Exception
    while ($exception) {
        if ($exception -is [UnauthorizedAccessException] -or
            ($exception -is [ComponentModel.Win32Exception] -and $exception.NativeErrorCode -eq 5) -or
            $exception.HResult -eq -2147024891) {
            return $true
        }
        $exception = $exception.InnerException
    }
    return $Failure.CategoryInfo.Category -eq [Management.Automation.ErrorCategory]::PermissionDenied
}

function Get-SessionTextServices {
    param([int]$SessionId)

    Get-Process -Name ctfmon -ErrorAction SilentlyContinue |
        Where-Object { $_.SessionId -eq $SessionId }
}

# The elevated mode stops only this desktop session's text-input process.
# It never reads/writes HKCU or starts ctfmon under the administrator's token.
if ($StopTextServices) {
    if ($TargetSessionId -lt 0 -or $TargetSessionId -ne [Diagnostics.Process]::GetCurrentProcess().SessionId) { exit 2 }
    if (!(Test-IsAdministrator)) { exit 3 }
    if (!$PSCmdlet.ShouldProcess("Text input in session $TargetSessionId", 'Stop ctfmon')) { exit 0 }
    try {
        Get-SessionTextServices $TargetSessionId | Stop-Process -Force -ErrorAction Stop
        exit 0
    } catch {
        if (Test-AccessDenied $_) { exit 5 }
        exit 1
    }
}

function Invoke-ElevatedTextServicesStop {
    param([int]$SessionId)

    Write-Host 'Windows denied the normal restart. Approve the UAC prompt to restart text input.' -ForegroundColor Yellow
    $powershell = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $arguments = @('-NoLogo', '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', ('"' + $helperScriptPath + '"'), '-StopTextServices', '-TargetSessionId', [string]$SessionId)
    try {
        $helper = Start-Process -FilePath $powershell -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -Wait -PassThru -ErrorAction Stop
    } catch {
        $exception = $_.Exception
        while ($exception) {
            if ($exception -is [ComponentModel.Win32Exception] -and $exception.NativeErrorCode -eq 1223) {
                throw 'Administrator approval was cancelled.'
            }
            $exception = $exception.InnerException
        }
        throw
    }
    switch ($helper.ExitCode) {
        0 { return }
        2 { throw 'The restart helper was not in the requested desktop session.' }
        3 { throw 'The restart helper did not receive administrator rights.' }
        5 { throw 'Windows still denied stopping ctfmon after administrator approval.' }
        default { throw "The restart helper failed with exit code $($helper.ExitCode)." }
    }
}

function Restart-TextServices {
    $currentSession = [Diagnostics.Process]::GetCurrentProcess().SessionId
    try {
        Get-SessionTextServices $currentSession | Stop-Process -Force -ErrorAction Stop
    } catch {
        if (!(Test-AccessDenied $_) -or (Test-IsAdministrator)) { throw }
        Invoke-ElevatedTextServicesStop $currentSession
    }
    Start-Sleep -Milliseconds 500
    # Windows can restart ctfmon itself. Avoid starting a duplicate if it has.
    if (@(Get-SessionTextServices $currentSession).Count -eq 0) {
        Start-Process -FilePath (Join-Path $env:WINDIR 'System32\ctfmon.exe') -WindowStyle Hidden
    }
}

function Get-IndicatorStatus {
    $options = Get-WinLanguageBarOption
    $showStatus = Get-ItemPropertyValue -LiteralPath $languageBarPath -Name ShowStatus -ErrorAction SilentlyContinue
    [pscustomobject]@{
        IsLegacyLanguageBar = $options.IsLegacyLanguageBar
        IsLegacySwitchingMode = $options.IsLegacySwitchingMode
        ShowStatus = $showStatus
    }
}

function Invoke-IndicatorAction {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([string]$SelectedAction)

    if ($SelectedAction -eq 'Status') {
        Get-IndicatorStatus
        return
    }
    if (!$PSCmdlet.ShouldProcess('Windows input indicator for the current user', "$SelectedAction and restart Text Services Framework")) {
        return
    }

    $old = Get-WinLanguageBarOption
    $useLegacyLanguageBar = $SelectedAction -ne 'Taskbar'
    Set-WinLanguageBarOption -UseLegacyLanguageBar:$useLegacyLanguageBar -UseLegacySwitchMode:$($old.IsLegacySwitchingMode)

    New-Item -Path $languageBarPath -Force | Out-Null
    $showStatus = if ($SelectedAction -eq 'Hide') { 3 } else { 0 }
    Set-ItemProperty -LiteralPath $languageBarPath -Name ShowStatus -Type DWord -Value $showStatus

    try {
        Restart-TextServices
    } catch {
        throw "The settings were saved, but Windows text input could not restart. $($_.Exception.Message) Sign out and back in to apply them."
    }

    Get-IndicatorStatus
}

if ($Action -ne 'Menu') {
    Invoke-IndicatorAction -SelectedAction $Action
    return
}

while ($true) {
    Write-Host ''
    Write-Host 'IME Steady - Windows input indicator' -ForegroundColor Cyan
    Write-Host '  1. Hide the Windows language bar'
    Write-Host '  2. Show the Windows language bar'
    Write-Host '  3. Use the standard Windows taskbar indicator'
    Write-Host '  4. Show current settings'
    Write-Host '  0. Exit'
    Write-Host ''
    Write-Host 'Exit IME Steady before changing settings. Changes restart Windows text input.'

    $choice = (Read-Host 'Choose an option').Trim()
    if ($choice -eq '0' -or $choice -eq 'q') { return }
    $selectedAction = switch ($choice) {
        '1' { 'Hide' }
        '2' { 'Show' }
        '3' { 'Taskbar' }
        '4' { 'Status' }
    }
    if (!$selectedAction) {
        Write-Host 'Choose 0, 1, 2, 3, or 4.' -ForegroundColor Yellow
        continue
    }

    try {
        Invoke-IndicatorAction -SelectedAction $selectedAction | Format-List | Out-Host
        if ($selectedAction -ne 'Status' -and !$WhatIfPreference) {
            Write-Host 'Settings updated. Activate Microsoft Pinyin and relaunch IME Steady.' -ForegroundColor Green
        }
    } catch {
        Write-Host ('Could not finish: ' + $_.Exception.Message) -ForegroundColor Red
    }
}
