
$ErrorActionPreference = "Continue"
$root = (Get-Location).Path

Write-Host "`n========== GIT VERSION ==========" -ForegroundColor Cyan
git --version

Write-Host "`n========== ROOT FILES ==========" -ForegroundColor Cyan
Get-ChildItem $root -Force |
    Select-Object Mode, Name, Length |
    Format-Table -AutoSize

Write-Host "`n========== SHARED FILES ==========" -ForegroundColor Cyan

$shared = Join-Path $root "shared"

if (Test-Path $shared) {
    Get-ChildItem $shared -Recurse -File |
        Select-Object FullName, Length |
        Format-Table -AutoSize
}

Write-Host "`n========== MINHOOK PACKAGE ==========" -ForegroundColor Cyan

$packages = Join-Path $root "packages"

if (Test-Path $packages) {
    Get-ChildItem $packages -Recurse -File |
        Select-Object FullName, Length |
        Format-Table -AutoSize
}

Write-Host "`n========== PROJECT SETTINGS ==========" -ForegroundColor Cyan

Get-ChildItem $root -Recurse -Filter "*.vcxproj" -File |
    ForEach-Object {
        Write-Host "`nPROJECT: $($_.FullName)" -ForegroundColor Green

        [xml]$xml = Get-Content $_.FullName -Raw

        foreach ($tag in @(
            "AdditionalIncludeDirectories",
            "AdditionalLibraryDirectories",
            "AdditionalDependencies",
            "OutDir",
            "IntDir",
            "TargetName",
            "TargetExt",
            "PreprocessorDefinitions"
        )) {
            $nodes = $xml.SelectNodes("//*[local-name()='$tag']")

            foreach ($node in $nodes) {
                Write-Host "  $tag = $($node.InnerText)"
            }
        }
    }

Write-Host "`n========== LARGE FILES ==========" -ForegroundColor Cyan

Get-ChildItem $root -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object {
        $_.Length -gt 10MB -and
        $_.FullName -notmatch '\\(\.vs|Debug|Release|x64|Win32)\\'
    } |
    Select-Object FullName,
        @{Name="SizeMB"; Expression={[math]::Round($_.Length / 1MB, 2)}} |
    Format-Table -AutoSize

Write-Host "`n========== EXISTING IGNORE FILE ==========" -ForegroundColor Cyan

$ignore = Join-Path $root ".gitignore"

if (Test-Path $ignore) {
    Get-Content $ignore
} else {
    Write-Host "No .gitignore exists."
}

Write-Host "`n========== END ==========" -ForegroundColor Cyan
Write-Host "Read-only check. No files modified."
