param([switch]$SkipTests, [string]$BuildDir = 'build')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    & (Join-Path $PSScriptRoot 'build-icon.ps1')
    cmake -S . -B $BuildDir -A x64
    if ($LASTEXITCODE -ne 0) { throw 'CMake 配置失败' }
    cmake --build $BuildDir --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw 'Release 构建失败' }
    if (-not $SkipTests) {
        ctest --test-dir $BuildDir -C Release --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw '自动化测试失败' }
    }
    New-Item -ItemType Directory -Force dist | Out-Null
    Copy-Item "$BuildDir\Release\QuickClipboard.exe" dist\QuickClipboard.exe -Force
    Copy-Item README.md dist\README.md -Force
    $packageFiles = @('dist\QuickClipboard.exe', 'dist\README.md')
    if (Test-Path -LiteralPath assets\screenshots) {
        New-Item -ItemType Directory -Force dist\assets\screenshots | Out-Null
        Copy-Item assets\screenshots\*.png dist\assets\screenshots -Force
        $packageFiles += 'dist\assets'
    }
    Compress-Archive -Path $packageFiles -DestinationPath dist\QuickClipboard-1.0.0-win-x64.zip -Force
    Write-Host "已生成 $projectRoot\dist\QuickClipboard.exe"
} finally { Pop-Location }
