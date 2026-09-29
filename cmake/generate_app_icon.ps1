param(
    [Parameter(Mandatory = $true)]
    [string]$Output
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$size = 256
$bitmap = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$graphics.Clear([System.Drawing.Color]::FromArgb(255, 12, 14, 20))

$green = [System.Drawing.Color]::FromArgb(255, 64, 214, 126)
$green2 = [System.Drawing.Color]::FromArgb(255, 46, 177, 104)
$dark = [System.Drawing.Color]::FromArgb(255, 10, 18, 14)

$outerBrush = New-Object System.Drawing.SolidBrush($green2)
$innerBrush = New-Object System.Drawing.SolidBrush($green)
$noteBrush = New-Object System.Drawing.SolidBrush($dark)

$graphics.FillEllipse($outerBrush, 24, 24, 208, 208)
$graphics.FillEllipse($innerBrush, 38, 38, 180, 180)

# A compact music-note mark that stays readable at 16x16 taskbar sizes.
$graphics.FillRectangle($noteBrush, 128, 76, 20, 91)
$graphics.FillRectangle($noteBrush, 143, 76, 50, 18)
$graphics.FillEllipse($noteBrush, 91, 145, 57, 46)
$graphics.FillEllipse($noteBrush, 145, 126, 57, 46)

$stream = New-Object System.IO.MemoryStream
$bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
$pngBytes = $stream.ToArray()

$directory = [System.IO.Path]::GetDirectoryName($Output)
if (-not [string]::IsNullOrWhiteSpace($directory)) {
    [System.IO.Directory]::CreateDirectory($directory) | Out-Null
}

$file = [System.IO.File]::Open($Output, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
$writer = New-Object System.IO.BinaryWriter($file)

# ICO header: reserved, type=icon, image count=1.
$writer.Write([UInt16]0)
$writer.Write([UInt16]1)
$writer.Write([UInt16]1)

# 256 is represented as 0 in the ICO directory entry.
$writer.Write([Byte]0)
$writer.Write([Byte]0)
$writer.Write([Byte]0)
$writer.Write([Byte]0)
$writer.Write([UInt16]1)
$writer.Write([UInt16]32)
$writer.Write([UInt32]$pngBytes.Length)
$writer.Write([UInt32]22)
$writer.Write($pngBytes)

$writer.Dispose()
$file.Dispose()
$stream.Dispose()
$noteBrush.Dispose()
$innerBrush.Dispose()
$outerBrush.Dispose()
$graphics.Dispose()
$bitmap.Dispose()
