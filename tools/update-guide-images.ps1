# Renders the user guide screenshots and writes them to docs/images/ as PNG files.
# Run from the repository root after building: powershell -File tools/update-guide-images.ps1
param(
    [string]$Build = "build/development/cmake",
    [string]$Sample = "assets/funky-beat.duck"
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$captures = "agents/reports/guide-captures"
$images = "docs/images"
$program = Join-Path $Build "tests/bin/guide_captures.exe"

if (-not (Test-Path $program)) {
    throw "Build the guide_captures target first: cmake --build $Build --target guide_captures"
}

& $program $Sample

if ($LASTEXITCODE -ne 0) {
    throw "guide_captures failed with exit code $LASTEXITCODE"
}

New-Item -ItemType Directory -Force $images | Out-Null

foreach ($bmp in Get-ChildItem "$captures/*.bmp") {
    $image = [System.Drawing.Image]::FromFile($bmp.FullName)
    $target = Join-Path (Resolve-Path $images) ($bmp.BaseName + ".png")
    $image.Save($target, [System.Drawing.Imaging.ImageFormat]::Png)
    $image.Dispose()
    Write-Output "Wrote $target"
}
