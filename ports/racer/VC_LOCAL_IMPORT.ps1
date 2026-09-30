param(
  [Parameter(Mandatory=$true)]
  [string]$GameRoot,
  [double]$CenterX = 0,
  [double]$CenterY = 0,
  [double]$Radius = 350,
  [int]$Interior = 0,
  [int]$MaxInstances = 0,
  [double]$SectorM = 24,
  [string]$VehicleModel = "sentinel",
  [int]$VehicleDetailBudget = 4500,
  [string]$UsbRacerDir = ""
)

$ErrorActionPreference = "Stop"
$StandaloneImporter = Join-Path $PSScriptRoot "vc_local_import.py"
$StandaloneVehicleImporter = Join-Path $PSScriptRoot "vc_vehicle_import.py"
if ((Test-Path $StandaloneImporter) -and (Test-Path $StandaloneVehicleImporter)) {
  $Importer = $StandaloneImporter
  $VehicleImporter = $StandaloneVehicleImporter
  $BuildDir = Join-Path $PSScriptRoot "build\vc-local"
} else {
  $RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
  $Importer = Join-Path $RepoRoot "ports\racer\vc_local_import.py"
  $VehicleImporter = Join-Path $RepoRoot "ports\racer\vc_vehicle_import.py"
  $BuildDir = Join-Path $RepoRoot "build\vc-local"
}
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

Write-Host "===== Vice City local import: inventory ====="
$inventoryArgs = @($Importer, "--game-root", $GameRoot, "--inventory-only", "--output-report", (Join-Path $BuildDir "vc_inventory.json"))
py -3 @inventoryArgs
if ($LASTEXITCODE -ne 0) { throw "Vice City inventory failed." }

Write-Host ""
Write-Host "===== Checking rwfury ====="
# Do not import the package just to test presence: if it is missing, Python
# prints a traceback to stderr and PowerShell with ErrorActionPreference=Stop
# can turn that expected condition into a terminating NativeCommandError.
py -3 -c "import importlib.util,sys; sys.exit(0 if importlib.util.find_spec('rwfury') else 1)"
$rwfuryPresent = ($LASTEXITCODE -eq 0)
if (-not $rwfuryPresent) {
  Write-Host "Installing rwfury (MIT) into current Python environment..."
  py -3 -m pip install rwfury
  if ($LASTEXITCODE -ne 0) { throw "rwfury installation failed." }
}
py -3 -c "import rwfury; print('rwfury OK')"
if ($LASTEXITCODE -ne 0) { throw "rwfury import failed after installation." }

Write-Host ""
Write-Host "===== Packing playable radius ====="
$packArgs = @(
  $Importer,
  "--game-root", $GameRoot,
  "--center-x", "$CenterX",
  "--center-y", "$CenterY",
  "--radius", "$Radius",
  "--interior", "$Interior",
  "--sector-m", "$SectorM",
  "--world-scale", "240",
  "--output-header", (Join-Path $BuildDir "vc_city_map.h"),
  "--output-bin", (Join-Path $BuildDir "VCMAP.BIN"),
  "--output-report", (Join-Path $BuildDir "vc_city_report.json")
)
if ($MaxInstances -gt 0) { $packArgs += @("--max-instances", "$MaxInstances") }

py -3 @packArgs
if ($LASTEXITCODE -ne 0) { throw "Vice City city pack failed." }

Write-Host ""
Write-Host ("===== Packing player vehicle: " + $VehicleModel + " =====")
$vehicleArgs = @(
  $VehicleImporter,
  "--game-root", $GameRoot,
  "--model", $VehicleModel,
  "--detail-budget", "$VehicleDetailBudget",
  "--world-scale", "240",
  "--output-bin", (Join-Path $BuildDir "VCVEH.BIN"),
  "--output-report", (Join-Path $BuildDir "vc_vehicle_report.json")
)
py -3 @vehicleArgs
if ($LASTEXITCODE -ne 0) { throw "Vice City vehicle pack failed." }

Write-Host ""
Write-Host "===== RESULT ====="
Write-Host ("Inventory : " + (Join-Path $BuildDir "vc_inventory.json"))
Write-Host ("Map report: " + (Join-Path $BuildDir "vc_city_report.json"))
Write-Host ("C header  : " + (Join-Path $BuildDir "vc_city_map.h"))
Write-Host ("Runtime map: " + (Join-Path $BuildDir "VCMAP.BIN"))
Write-Host ("Runtime collision: " + (Join-Path $BuildDir "VCCOL.BIN"))
Write-Host ("Vehicle report: " + (Join-Path $BuildDir "vc_vehicle_report.json"))
Write-Host ("Runtime vehicle: " + (Join-Path $BuildDir "VCVEH.BIN"))

if ($UsbRacerDir -ne "") {
  if (-not (Test-Path $UsbRacerDir)) { throw "UsbRacerDir does not exist: $UsbRacerDir" }
  Copy-Item -Force (Join-Path $BuildDir "VCMAP.BIN") (Join-Path $UsbRacerDir "VCMAP.BIN")
  Copy-Item -Force (Join-Path $BuildDir "VCCOL.BIN") (Join-Path $UsbRacerDir "VCCOL.BIN")
  Copy-Item -Force (Join-Path $BuildDir "VCVEH.BIN") (Join-Path $UsbRacerDir "VCVEH.BIN")
  Write-Host ("Copied VCMAP.BIN + VCCOL.BIN + VCVEH.BIN -> " + $UsbRacerDir)
}

Write-Host ""
Write-Host "No GTA source assets were copied into the repository or GitHub."
