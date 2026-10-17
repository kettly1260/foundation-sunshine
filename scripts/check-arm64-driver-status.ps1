# Read-only Sunshine ARM64 virtual-device status probe.
# Run in ordinary PowerShell on the Snapdragon host. No driver install,
# certificate import, PnP changes or elevation required.
[CmdletBinding()]
param(
  [string]$OutputPath = ".\sunshine-arm64-driver-status.json"
)
$ErrorActionPreference = "Stop"
$osArch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
$processArch = [System.Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString()
$all = @(Get-PnpDevice -ErrorAction SilentlyContinue)
function Find-Devices([string]$pattern) {
  @($all | Where-Object {
    $_.FriendlyName -match $pattern -or $_.InstanceId -match $pattern
  } | ForEach-Object {
    [ordered]@{ Name=$_.FriendlyName; Status=$_.Status; InstanceId=$_.InstanceId; Class=$_.Class }
  })
}
$items = [ordered]@{
  collected_utc = (Get-Date).ToUniversalTime().ToString("o")
  native_os_architecture = $osArch
  process_architecture = $processArch
  note = "Non-invasive status collection. Not proof a signed ARM64 driver can load."
  display = Find-Devices 'ZakoVDD|Zako Display|ROOT\\ZAKOVDD'
  mouse = Find-Devices 'ZakoVirtualMouse|Zako Virtual Mouse'
  gamepad = Find-Devices 'ViGEmBus|Virtual Gamepad Emulation Bus'
  controller = Find-Devices 'Xbox 360 Controller|Wireless Controller|DualShock'
}
$vigem = Join-Path $env:windir 'System32\drivers\ViGEmBus.sys'
if (Test-Path -LiteralPath $vigem) {
  $file = Get-Item -LiteralPath $vigem
  $sig = Get-AuthenticodeSignature -LiteralPath $vigem
  $items["gamepad_driver"] = [ordered]@{
    Path = $vigem
    Version = $file.VersionInfo.FileVersion
    SignatureStatus = $sig.Status.ToString()
    Signer = $sig.SignerCertificate.Subject
  }
} else {
  $items["gamepad_driver"] = @{ Found = $false }
}
$json = $items | ConvertTo-Json -Depth 9
$json | Out-File -FilePath $OutputPath -Encoding utf8
Write-Host "OS=$osArch Process=$processArch"
foreach ($group in @('display','mouse','gamepad','controller')) {
  $vals = @($items[$group])
  Write-Host "$group devices: $($vals.Count)"
  foreach ($val in $vals) { Write-Host "  $($val.Name): $($val.Status)" }
}
Write-Host "Saved non-invasive diagnostics: $OutputPath"
