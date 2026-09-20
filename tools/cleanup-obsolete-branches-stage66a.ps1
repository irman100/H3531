param(
    [switch]$Apply,
    [switch]$DropStage64
)

$ErrorActionPreference = "Stop"

$TargetBranch = "feature/h3531-stage66a-persistent-desktop"
$TargetRef = "origin/$TargetBranch"

$AlwaysKeep = @(
    "main",
    $TargetBranch
)

if (-not $DropStage64) {
    $AlwaysKeep += "feature/h3531-stage64-lxde"
}

Write-Host "H3531 Stage6.6A branch cleanup"
Write-Host "Target: $TargetRef"
Write-Host ""

git fetch origin --prune
if ($LASTEXITCODE -ne 0) {
    throw "git fetch failed"
}

git rev-parse --verify $TargetRef *> $null
if ($LASTEXITCODE -ne 0) {
    throw "Target ref not found: $TargetRef"
}

$Branches = @(
    git for-each-ref --format='%(refname:strip=3)' refs/remotes/origin/
) | Where-Object {
    $_ -and $_ -ne "HEAD"
} | Sort-Object -Unique

$SafeToDelete = @()
$Preserved = @()

foreach ($Branch in $Branches) {
    if ($AlwaysKeep -contains $Branch) {
        $Preserved += [PSCustomObject]@{
            Branch = $Branch
            Reason = "explicit keep"
        }
        continue
    }

    git merge-base --is-ancestor "origin/$Branch" $TargetRef 2>$null

    if ($LASTEXITCODE -eq 0) {
        $SafeToDelete += $Branch
    }
    else {
        $Preserved += [PSCustomObject]@{
            Branch = $Branch
            Reason = "contains commits not in Stage6.6A"
        }
    }
}

Write-Host "===== SAFE OBSOLETE BRANCHES ====="
foreach ($Branch in $SafeToDelete) {
    Write-Host "DELETE  $Branch"
}

Write-Host ""
Write-Host "===== PRESERVED BRANCHES ====="
foreach ($Item in $Preserved) {
    Write-Host ("KEEP    {0}  [{1}]" -f $Item.Branch, $Item.Reason)
}

Write-Host ""
Write-Host ("Safe obsolete branches: {0}" -f $SafeToDelete.Count)
Write-Host ("Preserved branches:      {0}" -f $Preserved.Count)

if (-not $Apply) {
    Write-Host ""
    Write-Host "DRY RUN ONLY. Nothing was deleted."
    Write-Host "After Stage6.6A hardware validation run:"
    Write-Host "  .\tools\cleanup-obsolete-branches-stage66a.ps1 -Apply"
    Write-Host ""
    Write-Host "After Stage6.5D rollback is no longer needed, also remove stage64:"
    Write-Host "  .\tools\cleanup-obsolete-branches-stage66a.ps1 -Apply -DropStage64"
    exit 0
}

if ($SafeToDelete.Count -eq 0) {
    Write-Host "Nothing to delete."
    exit 0
}

Write-Host ""
Write-Host "===== DELETING VERIFIED ANCESTOR BRANCHES ====="

foreach ($Branch in $SafeToDelete) {
    Write-Host "Deleting origin/$Branch ..."
    git push origin --delete "$Branch"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed deleting origin/$Branch"
    }
}

git fetch origin --prune

Write-Host ""
Write-Host "Cleanup complete."
