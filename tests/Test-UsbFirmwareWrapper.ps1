#Requires -Version 5.1
# All native commands are replaced by in-process test doubles. No USB/WSL writes.
param([string]$ProjectPath = (Split-Path -Parent $PSScriptRoot))
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$global:UsbFirmwareTestCalls = [System.Collections.Generic.List[string]]::new()
$global:UsbFirmwareTestAttached = $false
$global:UsbFirmwareTestFailAttach = $false
$global:UsbFirmwareTestFailUpload = $false
$global:UsbFirmwareTestKnownDevice = $true
$global:UsbFirmwareTestShared = $true
$global:UsbFirmwareTestInstance = 'USB\VID_1A86&PID_7523\TEST'

function global:usbipd {
    $values = @($args)
    $global:UsbFirmwareTestCalls.Add('usbipd ' + ($values -join ' '))
    $global:LASTEXITCODE = 0
    if ($values[0] -eq 'state') {
        $id = if ($global:UsbFirmwareTestKnownDevice) { '1-1' } else { '1-9' }
        $guid = if ($global:UsbFirmwareTestShared) { 'test-guid' } else { $null }
        $client = if ($global:UsbFirmwareTestAttached) { '172.20.0.1' } else { $null }
        @{Devices = @(@{BusId=$id; InstanceId=$global:UsbFirmwareTestInstance; Description='Test Nano'; PersistedGuid=$guid; ClientIPAddress=$client})} | ConvertTo-Json -Depth 4
    } elseif ($values[0] -eq 'attach' -and $global:UsbFirmwareTestFailAttach) { $global:LASTEXITCODE = 3 }
}
function global:wsl.exe {
    $values = @($args)
    $global:UsbFirmwareTestCalls.Add('wsl.exe ' + ($values -join ' '))
    $global:LASTEXITCODE = 0
    if ($values -contains 'bash' -and $global:UsbFirmwareTestFailUpload) { $global:LASTEXITCODE = 7 }
}
function Check([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Reject([scriptblock]$Action) {
    $rejected = $false
    try { & $Action } catch { $rejected = $true }
    Check $rejected 'Expected failure was not propagated'
}
$wrapper = Join-Path $ProjectPath 'scripts/Upload-Firmware.ps1'
$tokens = $null
$errors = $null
$null = [System.Management.Automation.Language.Parser]::ParseFile($wrapper, [ref]$tokens, [ref]$errors)
Check ($errors.Count -eq 0) 'PowerShell syntax error'
try {
    & $wrapper -BusId 1-1 -Port /dev/ttyUSB0 -Confirm:$false
    Check ($global:UsbFirmwareTestCalls -contains 'usbipd attach --wsl Ubuntu-24.04 --busid 1-1') 'Missing one-shot attach'
    Check (-not ($global:UsbFirmwareTestCalls | Where-Object { $_ -like 'usbipd bind*' })) 'Already-shared device rebound'
    Check ($global:UsbFirmwareTestCalls[-1] -like 'wsl.exe *bash */scripts/upload-firmware.sh --port /dev/ttyUSB0 --fqbn arduino:avr:nano:cpu=atmega328old') 'Wrong upload command'
    $global:UsbFirmwareTestCalls.Clear()
    $global:UsbFirmwareTestAttached = $true
    & $wrapper -BusId 1-1 -AttachOnly -Confirm:$false
    Check (-not ($global:UsbFirmwareTestCalls | Where-Object { $_ -like 'usbipd attach*' -or $_ -like '* bash *' })) 'Existing attachment or attach-only was mutated'
    $global:UsbFirmwareTestCalls.Clear()
    $global:UsbFirmwareTestShared = $false
    & $wrapper -BusId 1-1 -Port /dev/ttyUSB0 -WhatIf
    Check ($global:UsbFirmwareTestCalls.Count -eq 1 -and $global:UsbFirmwareTestCalls[0] -eq 'usbipd state') 'WhatIf performed more than a state read'
    $global:UsbFirmwareTestCalls.Clear()
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        & $wrapper -BusId 1-1 -AttachOnly -Confirm:$false
        Check ($global:UsbFirmwareTestCalls -contains 'usbipd bind --busid 1-1') 'Unshared device was not bound'
    } else {
        Reject { & $wrapper -BusId 1-1 -AttachOnly -Confirm:$false }
        Check ($global:UsbFirmwareTestCalls.Count -eq 1) 'Non-admin first binding should fail before WSL or USB changes'
    }
    $global:UsbFirmwareTestShared = $true
    $global:UsbFirmwareTestAttached = $false
    $global:UsbFirmwareTestCalls.Clear()
    $global:UsbFirmwareTestFailAttach = $true
    Reject { & $wrapper -BusId 1-1 -Port /dev/ttyUSB0 -Confirm:$false }
    Check (-not ($global:UsbFirmwareTestCalls | Where-Object { $_ -like '* bash *' })) 'Upload continued after failed attach'
    $global:UsbFirmwareTestFailAttach = $false
    $global:UsbFirmwareTestFailUpload = $true
    Reject { & $wrapper -BusId 1-1 -Port /dev/ttyUSB0 -Confirm:$false }
    $global:UsbFirmwareTestFailUpload = $false
    $global:UsbFirmwareTestCalls.Clear()
    Reject { & $wrapper -BusId 1-1 -Confirm:$false }
    Check ($global:UsbFirmwareTestCalls.Count -eq 0) 'Missing Linux port should fail before external commands'
    $global:UsbFirmwareTestKnownDevice = $false
    Reject { & $wrapper -BusId 1-1 -AttachOnly -Confirm:$false }
    $global:UsbFirmwareTestKnownDevice = $true
    $global:UsbFirmwareTestInstance = 'USB\VID_1BCF&PID_2B96\CAMERA'
    Reject { & $wrapper -BusId 1-1 -AttachOnly -Confirm:$false }
    Write-Host 'USB wrapper mock tests passed: shared/attached states, dry-run, explicit port, device filter and error propagation.'
} finally {
    Remove-Item Function:\usbipd, Function:\wsl.exe
}
