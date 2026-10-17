# Verify a signed native ViGEmBus driver inside a disposable Windows ARM64 CI runner.
# Read-only except for writing the JSON evidence report. No driver install,
# certificate import, reboot, test signing or system setting changes here.
[CmdletBinding()]
param(
  [string]$OutputPath = "$env:RUNNER_TEMP\vigem-arm64-smoke.json",
  [int]$WaitSeconds = 45
)
$ErrorActionPreference = 'Stop'
$osArch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
if ($osArch -ne 'Arm64') { throw "Expected native Windows ARM64 CI runner, got $osArch" }
$driverPath = Join-Path $env:windir 'System32\drivers\ViGEmBus.sys'
if (!(Test-Path -LiteralPath $driverPath)) { throw "ViGEmBus.sys is not installed" }
$signature = Get-AuthenticodeSignature -LiteralPath $driverPath
if ($signature.Status.ToString() -ne 'Valid') {
  throw "Installed ViGEmBus driver signature invalid: $($signature.Status)"
}
$bytes = [System.IO.File]::ReadAllBytes($driverPath)
$offset = [BitConverter]::ToInt32($bytes, 0x3c)
$machine = [BitConverter]::ToUInt16($bytes, $offset + 4)
if ($machine -ne 0xAA64) { throw ('Expected ARM64 driver (0xAA64), found 0x{0:X4}' -f $machine) }
Write-Host ("Signed native ViGEmBus.sys verified: PE Machine=0x{0:X4}" -f $machine)

function Find-BusDevices {
  $all = @(Get-PnpDevice -ErrorAction SilentlyContinue)
  $presentIds = @{}
  foreach ($connected in @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue)) {
    $presentIds[[string]$connected.InstanceId] = $true
  }
  $matched = @()
  foreach ($dev in $all) {
    $instance = [string]$dev.InstanceId
    $label = [string]$dev.FriendlyName
    $looksRelevant = ($instance -match '(?i)ViGEm|VirtualGamepad') -or
                     ($label -match '(?i)ViGEm|Nefarius.*(Virtual|Gamepad)|Virtual Gamepad Emulation')
    $service = ''
    if ($looksRelevant -or $dev.Class -eq 'System') {
      try {
        $prop = Get-PnpDeviceProperty -InstanceId $instance -KeyName 'DEVPKEY_Device_Service' -ErrorAction Stop
        $service = [string]$prop.Data
      } catch {}
    }
    if ($looksRelevant -or $service -match '(?i)^ViGEmBus$') {
      $problem = $null
      try {
        $problem = (Get-PnpDeviceProperty -InstanceId $instance -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction Stop).Data
      } catch {}
      $matched += [pscustomobject]@{
        Name = $label
        InstanceId = $instance
        Status = [string]$dev.Status
        Present = $presentIds.ContainsKey($instance)
        Class = [string]$dev.Class
        Service = $service
        ProblemCode = $problem
      }
    }
  }
  return @($matched)
}
$service = Get-CimInstance Win32_SystemDriver -Filter "Name = 'ViGEmBus'" -ErrorAction SilentlyContinue
$deadline = (Get-Date).AddSeconds($WaitSeconds)
$devices = @()
$healthy = @()
do {
  $devices = @(Find-BusDevices)
  $healthy = @($devices | Where-Object {
    $_.Status -eq 'OK' -and $_.Present -and
    ($_.Service -match '(?i)^ViGEmBus$' -or $_.InstanceId -match '(?i)ViGEmBus')
  })
  if ($healthy.Count -gt 0) { break }
  if ((Get-Date) -ge $deadline) { break }
  Start-Sleep -Seconds 5
} while ($true)
$service = Get-CimInstance Win32_SystemDriver -Filter "Name = 'ViGEmBus'" -ErrorAction SilentlyContinue
$driverMatches = @(Get-CimInstance Win32_PnPSignedDriver -ErrorAction SilentlyContinue |
  Where-Object {
    $_.DeviceName -match '(?i)ViGEm|Nefarius.*Gamepad' -or
    $_.DeviceID -match '(?i)ViGEm' -or $_.InfName -match '(?i)vigem'
  } |
  Select-Object DeviceName, DeviceID, DeviceClass, InfName, DriverVersion, IsSigned)
$evidence = [ordered]@{
  os = $osArch
  driver_path = $driverPath
  file_architecture = 'ARM64'
  driver_authenticode_status = $signature.Status.ToString()
  signer = $signature.SignerCertificate.Subject
  service = if ($service) { @{ Name=$service.Name; State=$service.State; Started=$service.Started; StartMode=$service.StartMode } } else { $null }
  pnp_devices = @($devices)
  pnp_signed_driver_matches = @($driverMatches)
  pnp_success = ($healthy.Count -gt 0)
  driver_install_attempted_in_ci = $true
  physical_user_pc_modified = $false
  checks = 'Signed native driver, service registration and PnP device; no X360/DS4 function test'
}
$pathDir = Split-Path -Parent $OutputPath
if ($pathDir) { New-Item -ItemType Directory -Path $pathDir -Force | Out-Null }
$evidence | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Write-Host "Evidence: $OutputPath"
Write-Host "Service: $($service | Out-String)"
Write-Host "Matching PnP devices: $($devices | ConvertTo-Json -Depth 5)"
Write-Host "PnP driver records: $($driverMatches | ConvertTo-Json -Depth 5)"
if (!$evidence.pnp_success) {
  throw "ViGEmBus binary is signed ARM64, but no matching healthy present PnP bus was found within $WaitSeconds seconds. Evidence saved. Do not conclude driver works."
}
Write-Host "PASS: ViGEmBus driver is signed ARM64, present, and reports healthy PnP state."
