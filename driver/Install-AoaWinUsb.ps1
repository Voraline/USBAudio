param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^USB\\VID_[0-9A-Fa-f]{4}&PID_[0-9A-Fa-f]{4}(&REV_[0-9A-Fa-f]{4})?(&MI_[0-9A-Fa-f]{2})?$')]
    [string]$PhoneHardwareId
)

$Identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$Principal = [Security.Principal.WindowsPrincipal]::new($Identity)
if (!$Principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this script from an elevated PowerShell window."
}

$HardwareIds = @(
    $PhoneHardwareId,
    "USB\VID_18D1&PID_2D00",
    "USB\VID_18D1&PID_2D01&MI_00"
)
$DeviceRows = [System.Collections.Generic.List[string]]::new()
for ($Index = 0; $Index -lt $HardwareIds.Count; $Index++) {
    $DeviceRows.Add("%DeviceName%=WinUSB_Install, $($HardwareIds[$Index])")
}

$InfText = @"
[Version]
Signature=`"`$Windows NT`$`"
Class=USBDevice
ClassGuid={88BAE032-5A81-49F0-BC3D-A4FF138216D6}
Provider=%ProviderName%
DriverVer=09/28/2026,1.0.0.0

[Manufacturer]
%ProviderName%=DeviceList,NTamd64

[DeviceList.NTamd64]
$($DeviceRows -join "`r`n")

[WinUSB_Install.NT]
Include=winusb.inf
Needs=WINUSB.NT

[WinUSB_Install.NT.Services]
Include=winusb.inf
Needs=WINUSB.NT.Services

[WinUSB_Install.NT.HW]
AddReg=WinUSB_Install.NT.HW.AddReg

[WinUSB_Install.NT.HW.AddReg]
HKR,,DeviceInterfaceGUIDs,0x10000,`"{A6D1C905-76DA-4DBB-8C82-91615486C8B5}`"

[Strings]
ProviderName=`"USB Audio Project`"
DeviceName=`"USB Audio Android Accessory`"
"@

$InfPath = Join-Path $PSScriptRoot "USBAudioWinUsb.inf"
Set-Content -LiteralPath $InfPath -Value $InfText -Encoding Ascii
pnputil.exe /add-driver $InfPath /install
if ($LASTEXITCODE -ne 0) {
    throw "Windows rejected the generated WinUSB driver package. A signed package for this phone interface is required by the active Windows driver-signing policy."
}
Write-Output "WinUSB package installed. Reconnect the phone, then run UsbAudioSender.exe with the phone's current VID and PID."
