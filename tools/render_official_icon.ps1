# Convert the owner's original PNG to Windows icon sizes, only in external temp.
# SPDX-License-Identifier: GPL-2.0-or-later
param([Parameter(Mandatory=$true)][string]$Source,[Parameter(Mandatory=$true)][string]$Out)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
$original=[Drawing.Image]::FromFile($Source)
try {
    foreach($size in @(16,32,48,64,128,256)) {
        $bitmap=New-Object Drawing.Bitmap($size,$size,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $graphics=[Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.Clear([Drawing.Color]::Transparent)
            $graphics.InterpolationMode=[Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.PixelOffsetMode=[Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $factor=[Math]::Min($size/$original.Width,$size/$original.Height)
            $width=[int][Math]::Round($original.Width*$factor);$height=[int][Math]::Round($original.Height*$factor)
            $graphics.DrawImage($original,[int](($size-$width)/2),[int](($size-$height)/2),$width,$height)
            $bitmap.Save((Join-Path $Out ($size.ToString()+'.png')),[Drawing.Imaging.ImageFormat]::Png)
        } finally {$graphics.Dispose();$bitmap.Dispose()}
    }
} finally {$original.Dispose()}
