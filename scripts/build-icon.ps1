$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$projectRoot = Split-Path -Parent $PSScriptRoot
$svgPath = Join-Path $projectRoot 'assets\quickclipboard.svg'
$icoPath = Join-Path $projectRoot 'assets\quickclipboard.ico'
$previewPath = Join-Path $projectRoot 'assets\quickclipboard.png'
$sourceTime = @((Get-Item -LiteralPath $svgPath).LastWriteTimeUtc, (Get-Item -LiteralPath $PSCommandPath).LastWriteTimeUtc) | Sort-Object -Descending | Select-Object -First 1
if ((Test-Path -LiteralPath $icoPath) -and (Test-Path -LiteralPath $previewPath) -and (Get-Item -LiteralPath $icoPath).LastWriteTimeUtc -ge $sourceTime -and (Get-Item -LiteralPath $previewPath).LastWriteTimeUtc -ge $sourceTime) { return }
[xml]$svg = [System.IO.File]::ReadAllText($svgPath)

# 绘制图标的矢量轮廓，小尺寸略加粗以保持文件线条清晰。
function New-IconBitmap([int]$size) {
    $bitmap = [System.Drawing.Bitmap]::new($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.Clear([System.Drawing.Color]::Transparent)
        $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $graphics.ScaleTransform(($size / 64.0), ($size / 64.0))
        foreach ($element in $svg.DocumentElement.ChildNodes) {
            $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
            try {
                if ($element.LocalName -eq 'rect') {
                    $x = [float]$element.x; $y = [float]$element.y; $w = [float]$element.width; $h = [float]$element.height; $d = [float]$element.rx * 2
                    $path.AddArc($x, $y, $d, $d, 180, 90)
                    $path.AddArc(($x + $w - $d), $y, $d, $d, 270, 90)
                    $path.AddArc(($x + $w - $d), ($y + $h - $d), $d, $d, 0, 90)
                    $path.AddArc($x, ($y + $h - $d), $d, $d, 90, 90)
                    $path.CloseFigure()
                } elseif ($element.LocalName -eq 'path') {
                    $tokens = [regex]::Matches($element.d, '[MLZ]|-?\d+(?:\.\d+)?')
                    $last = [System.Drawing.PointF]::new([float]0, [float]0)
                    for ($i = 0; $i -lt $tokens.Count;) {
                        $command = $tokens[$i++].Value
                        if ($command -eq 'Z') { $path.CloseFigure(); continue }
                        $point = [System.Drawing.PointF]::new([float]::Parse($tokens[$i++].Value, [System.Globalization.CultureInfo]::InvariantCulture), [float]::Parse($tokens[$i++].Value, [System.Globalization.CultureInfo]::InvariantCulture))
                        if ($command -eq 'M') { $path.StartFigure() } else { $path.AddLine($last, $point) }
                        $last = $point
                    }
                } else { continue }
                if ($element.fill -and $element.fill -ne 'none') {
                    $brush = [System.Drawing.SolidBrush]::new([System.Drawing.ColorTranslator]::FromHtml($element.fill))
                    try { $graphics.FillPath($brush, $path) } finally { $brush.Dispose() }
                }
                if ($element.stroke) {
                    $width = [float]::Parse($element.GetAttribute('stroke-width'), [System.Globalization.CultureInfo]::InvariantCulture)
                    if ($size -le 24) { $width = [Math]::Max($width, 3.2) }
                    $pen = [System.Drawing.Pen]::new([System.Drawing.ColorTranslator]::FromHtml($element.stroke), $width)
                    $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
                    $pen.StartCap = $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
                    try { $graphics.DrawPath($pen, $path) } finally { $pen.Dispose() }
                }
            } finally { $path.Dispose() }
        }
    } finally { $graphics.Dispose() }
    return $bitmap
}

# 小尺寸使用标准 32 位位图，256 像素使用 PNG，兼顾任务栏和资源管理器。
function Get-IconFrame([System.Drawing.Bitmap]$bitmap) {
    $size = $bitmap.Width; $stream = [System.IO.MemoryStream]::new()
    try {
        if ($size -eq 256) { $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png) }
        else {
            $writer = [System.IO.BinaryWriter]::new($stream, [System.Text.Encoding]::UTF8, $true)
            try {
                $maskStride = [int]([Math]::Ceiling($size / 32.0) * 4)
                $writer.Write([int]40); $writer.Write([int]$size); $writer.Write([int]($size * 2))
                $writer.Write([uint16]1); $writer.Write([uint16]32); $writer.Write([int]0)
                $writer.Write([int]($size * $size * 4 + $maskStride * $size))
                foreach ($unused in 1..4) { $writer.Write([int]0) }
                for ($y = $size - 1; $y -ge 0; $y--) {
                    for ($x = 0; $x -lt $size; $x++) {
                        $color = $bitmap.GetPixel($x, $y)
                        $writer.Write([byte]$color.B); $writer.Write([byte]$color.G); $writer.Write([byte]$color.R); $writer.Write([byte]$color.A)
                    }
                }
                for ($y = $size - 1; $y -ge 0; $y--) {
                    $mask = [byte[]]::new($maskStride)
                    for ($x = 0; $x -lt $size; $x++) { if ($bitmap.GetPixel($x, $y).A -eq 0) { $mask[[int][Math]::Floor($x / 8.0)] = $mask[[int][Math]::Floor($x / 8.0)] -bor (128 -shr ($x % 8)) } }
                    $writer.Write($mask)
                }
            } finally { $writer.Dispose() }
        }
        return ,$stream.ToArray()
    } finally { $stream.Dispose() }
}
$sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
$frames = [System.Collections.Generic.List[byte[]]]::new()
foreach ($size in $sizes) {
    $bitmap = New-IconBitmap $size
    try {
        if ($size -eq 128) { $bitmap.Save($previewPath, [System.Drawing.Imaging.ImageFormat]::Png) }
        $frames.Add((Get-IconFrame $bitmap))
    } finally { $bitmap.Dispose() }
}
$output = [System.IO.File]::Create($icoPath); $writer = [System.IO.BinaryWriter]::new($output)
try {
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $dimension = if ($sizes[$i] -eq 256) { 0 } else { $sizes[$i] }
        $writer.Write([byte]$dimension); $writer.Write([byte]$dimension); $writer.Write([byte]0); $writer.Write([byte]0)
        $writer.Write([uint16]1); $writer.Write([uint16]32); $writer.Write([int]$frames[$i].Length); $writer.Write([int]$offset)
        $offset += $frames[$i].Length
    }
    foreach ($frame in $frames) { $writer.Write($frame) }
} finally { $writer.Dispose(); $output.Dispose() }
Write-Host '已生成 QuickClipboard 多尺寸程序图标'
