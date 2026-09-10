#Requires -Version 5.1
<#
.SYNOPSIS
Bind/attach a selected Windows USB device to WSL, then build/upload Nano firmware.
.EXAMPLE
.\Upload-Firmware.ps1 -BusId 1-1 -Port /dev/ttyUSB0
.EXAMPLE
.\Upload-Firmware.ps1 -BusId 1-1 -AttachOnly
.NOTES
Run in Windows PowerShell. First-time binding needs Administrator privileges.
Use -WhatIf to show the operation without binding, attaching or uploading.
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9]+-[0-9]+(\.[0-9]+)*$')]
    [string]$BusId,

    [ValidatePattern('^/dev/[^\r\n]+$')]
    [string]$Port,

    [ValidateNotNullOrEmpty()]
    [string]$Distribution = 'Ubuntu-24.04',

    [ValidatePattern('^/[^\r\n]+$')]
    [string]$ProjectPath = '/home/mike/repos/GpuFanController',

    [ValidateNotNullOrEmpty()]
    [string]$Fqbn = 'arduino:avr:nano:cpu=atmega328old',

    [switch]$AttachOnly
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $AttachOnly -and [string]::IsNullOrWhiteSpace($Port)) {
    throw 'Specify -Port (the Linux serial port for this controller), or use -AttachOnly.'
}
$null = Get-Command usbipd -ErrorAction Stop
$null = Get-Command wsl.exe -ErrorAction Stop

function Invoke-CheckedCommand {
    param([string]$Command, [string[]]$CommandArguments)
    & $Command @CommandArguments
    if ($LASTEXITCODE -ne 0) { throw "$Command failed (exit $LASTEXITCODE). No further steps were run." }
}

$stateText = & usbipd state
if ($LASTEXITCODE -ne 0) { throw 'Cannot read usbipd state. Install/start usbipd-win 5.x.' }
$state = ($stateText -join "`n") | ConvertFrom-Json
$devices = @($state.Devices | Where-Object { $_.BusId -eq $BusId })
if ($devices.Count -ne 1) { throw "Bus ID $BusId is not connected. Run 'usbipd list' and select the Nano." }
$device = $devices[0]
# Adapter identity is a safety filter, not proof of which firmware/device is connected.
if ($device.InstanceId -notmatch '^USB\\VID_([0-9A-F]{4})&PID_([0-9A-F]{4})\\') {
    throw 'Cannot identify the selected USB adapter.'
}
$vendor = $Matches[1].ToLowerInvariant()
$product = $Matches[2].ToLowerInvariant()
$supported = "$vendor`:$product" -in @('1a86:7523', '0403:6001', '10c4:ea60') -or $vendor -in @('2341', '2a03')
if (-not $supported) { throw 'Selected device is not a supported Nano/USB-serial candidate. Refusing to attach it.' }
$needsBind = [string]::IsNullOrEmpty([string]$device.PersistedGuid)
$needsAttach = [string]::IsNullOrEmpty([string]$device.ClientIPAddress)
$target = "$($device.Description), Windows bus $BusId -> WSL $Distribution"
$action = if ($AttachOnly) { 'Bind if needed and attach to WSL (no upload)' } else { "Bind if needed, attach, build and upload/verify on $Port" }
if (-not $PSCmdlet.ShouldProcess($target, $action)) { return }
if ($needsBind) {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "First-time binding requires Administrator PowerShell. Run: usbipd bind --busid $BusId ; then rerun this script normally."
    }
}

# Validate the intended distro/project before changing USB ownership. Also starts WSL.
if ($AttachOnly) {
    Invoke-CheckedCommand 'wsl.exe' @('--distribution', $Distribution, '--exec', 'true')
} else {
    $uploadScript = $ProjectPath.TrimEnd('/') + '/scripts/upload-firmware.sh'
    Invoke-CheckedCommand 'wsl.exe' @('--distribution', $Distribution, '--exec', 'test', '-f', $uploadScript)
}
if ($needsBind) {
    Invoke-CheckedCommand 'usbipd' @('bind', '--busid', $BusId)
} else { Write-Host "USB $BusId is already shared; binding skipped." }
if ($needsAttach) {
    # Deliberately no --force, --auto-attach or detaching of another client's device.
    Invoke-CheckedCommand 'usbipd' @('attach', '--wsl', $Distribution, '--busid', $BusId)
} else {
    Write-Host "USB $BusId already has an attached client. Leaving its attachment unchanged."
}
if ($AttachOnly) {
    Write-Host 'Attachment step complete; nothing uploaded. Check the selected device is visible in WSL before using it.'
    return
}

$ready = $false
for ($attempt = 0; $attempt -lt 20; $attempt++) {
    & wsl.exe --distribution $Distribution --exec test -c $Port
    if ($LASTEXITCODE -eq 0) { $ready = $true; break }
    Start-Sleep -Milliseconds 250
}
if (-not $ready) { throw "Serial port $Port did not appear in $Distribution. Check attachment and the Linux port; no upload attempted." }
Write-Host "Uploading to the explicitly selected port $Port. Ensure it belongs to Windows bus $BusId."
Write-Host 'Close serial tools/competing daemon sessions first; the Nano will reset.'
Invoke-CheckedCommand 'wsl.exe' @('--distribution', $Distribution, '--exec', 'bash', $uploadScript, '--port', $Port, '--fqbn', $Fqbn)
Write-Host 'Upload and verification completed. The USB device remains attached to WSL for the daemon.'
