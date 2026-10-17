param(
    [Parameter(Mandatory = $true)][string]$OutFile,
    [string]$ProcessName = "urban",
    [switch]$Window
)
# Captures the whole monitor the process's main window is on (what the screen shows, e.g. in fullscreen), or the
# primary monitor if the process has no window.
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class MonitorCaptureNative {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
[void][MonitorCaptureNative]::SetProcessDPIAware()
$screen = [System.Windows.Forms.Screen]::PrimaryScreen
$process = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
if ($process) {
    $screen = [System.Windows.Forms.Screen]::FromHandle($process.MainWindowHandle)
}
$bounds = $screen.Bounds
if ($Window -and $process) {
    # -Window: only the process's main window (as it is on screen; it must not be covered).
    $rect = New-Object MonitorCaptureNative+RECT
    [void][MonitorCaptureNative]::GetWindowRect($process.MainWindowHandle, [ref]$rect)
    $bounds = New-Object System.Drawing.Rectangle $rect.Left, $rect.Top, ($rect.Right - $rect.Left), ($rect.Bottom - $rect.Top)
}
$bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
$bitmap.Save($OutFile, [System.Drawing.Imaging.ImageFormat]::Png)
$graphics.Dispose()
$bitmap.Dispose()
Write-Output "captured $($bounds.Width)x$($bounds.Height) at $($bounds.X),$($bounds.Y)"
