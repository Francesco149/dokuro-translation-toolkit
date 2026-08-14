# win/screenshot.ps1 — capture the Windows desktop to a PNG (for WSL-driven
# GUI verification of dokuro-editor.exe). Coordinates are virtual-screen
# pixels of this process (call from a 96-dpi session for a 1:1 map to
# SendInput coords).
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File screenshot.ps1 -Out C:\path\shot.png
#        [-X x -Y y -W w -H h]   crop to region (screen coords) — useful for
#                                vision-model close-ups of a known window rect
param([string]$Out = "C:\dokuro-test\shot.png",
      [int]$X = -1, [int]$Y = -1, [int]$W = 0, [int]$H = 0)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$b = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.X, $b.Y, 0, 0, $bmp.Size)
$g.Dispose()
if ($X -ge 0) {
    $crop = $bmp.Clone((New-Object System.Drawing.Rectangle($X, $Y, $W, $H)), $bmp.PixelFormat)
    $bmp.Dispose()
    $bmp = $crop
}
$w = $bmp.Width; $h = $bmp.Height
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output ("saved: " + $Out + " (" + $w + "x" + $h + ")")
