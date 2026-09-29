param(
  [Parameter(Mandatory=$true)]
  [string]$GameRoot,
  [double]$CenterX = 0,
  [double]$CenterY = 0,
  [double]$Radius = 350,
  [int]$Interior = 0,
  [int]$MaxInstances = 0
)

$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$Importer = Join-Path $RepoRoot "ports\racer\vc_local_import.py"
$BuildDir = Join-Path $RepoRoot "build\vc-local"
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

Write-Host "===== Vice City local import: inventory ====="
$inventoryArgs = @($Importer, "--game-root", $GameRoot, "--inventory-only", "--output-report", (Join-Path $BuildDir "vc_inventory.json"))
py -3 @inventoryArgs
if ($LASTEXITCODE -ne 0) { throw "Vice City inventory failed." }

Write-Host ""
Write-Host "===== Checking rwfury ====="
py -3 -c "import rwfury; print('rwfury OK')" 2>$null
if ($LASTEXITCODE -ne 0) {
  Write-Host "Installing rwfury (MIT) into current Python environment..."
  py -3 -m pip install rwfury
  if ($LASTEXITCODE -ne 0) { throw "rwfury installation failed." }
}

Write-Host ""
Write-Host "===== Packing playable radius ====="
$packArgs = @(
  $Importer,
  "--game-root", $GameRoot,
  "--center-x", "$CenterX",
  "--center-y", "$CenterY",
  "--radius", "$Radius",
  "--interior", "$Interior",
  "--sector-m", "64",
  "--world-scale", "240",
  "--output-header", (Join-Path $BuildDir "vc_city_map.h"),
  "--output-report", (Join-Path $BuildDir "vc_city_report.json")
)
if ($MaxInstances -gt 0) { $packArgs += @("--max-instances", "$MaxInstances") }

py -3 @packArgs
if ($LASTEXITCODE -ne 0) { throw "Vice City city pack failed." }

Write-Host ""
Write-Host "===== RESULT ====="
Write-Host ("Inventory : " + (Join-Path $BuildDir "vc_inventory.json"))
Write-Host ("Map report: " + (Join-Path $BuildDir "vc_city_report.json"))
Write-Host ("C header  : " + (Join-Path $BuildDir "vc_city_map.h"))
Write-Host ""
Write-Host "No GTA game files were copied into the repository."
