param(
    [string]$TitleContains = "Urban Reign",
    [Parameter(Mandatory = $true)][string]$OutFile
)
# Like capture_window.ps1, but copies the window's client area from the screen (the composed image). Needed for PCSX2's
# GS (a DXGI child window that PrintWindow returns black); the window must be visible and not covered.
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class ScreenCaptureNative {
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int count);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT point);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@
[void][ScreenCaptureNative]::SetProcessDPIAware()
$found = [IntPtr]::Zero
$callback = [ScreenCaptureNative+EnumProc]{
    param($hWnd, $lParam)
    if (-not [ScreenCaptureNative]::IsWindowVisible($hWnd)) { return $true }
    $sb = New-Object System.Text.StringBuilder 512
    [void][ScreenCaptureNative]::GetWindowText($hWnd, $sb, 512)
    if ($sb.ToString().Contains($TitleContains)) { $script:found = $hWnd; return $false }
    return $true
}
[void][ScreenCaptureNative]::EnumWindows($callback, [IntPtr]::Zero)
if ($found -eq [IntPtr]::Zero) { Write-Error "window containing '$TitleContains' not found"; exit 1 }
$rect = New-Object ScreenCaptureNative+RECT
[void][ScreenCaptureNative]::GetClientRect($found, [ref]$rect)
$origin = New-Object ScreenCaptureNative+POINT
[void][ScreenCaptureNative]::ClientToScreen($found, [ref]$origin)
$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
$bitmap = New-Object System.Drawing.Bitmap $width, $height
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.CopyFromScreen($origin.X, $origin.Y, 0, 0, (New-Object System.Drawing.Size $width, $height))
$graphics.Dispose()
$bitmap.Save($OutFile, [System.Drawing.Imaging.ImageFormat]::Png)
$bitmap.Dispose()
Write-Output "captured ${width}x${height} -> $OutFile"
