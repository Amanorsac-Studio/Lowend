# preview-installer.ps1 - screenshots the installer's pages WITHOUT installing.
#
# Runs the PreviewOnly build of installer\LowEnd.iss (no elevation), walks
# Welcome -> Licence -> Components through UI Automation, saves a PNG of each
# page, then cancels. Nothing is written outside the output folder. The
# screenshots are the evidence the Installer & Packaging Standard §12 asks for.
param([Parameter(Mandatory)] [string] $Setup, [Parameter(Mandatory)] [string] $OutDir)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class Win {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
  public struct RECT { public int L, T, R, B; }
}
"@
[Win]::SetProcessDPIAware() | Out-Null
New-Item -ItemType Directory -Force $OutDir | Out-Null

$proc = Start-Process -FilePath $Setup -PassThru
# Inno's loader starts the real setup as a child (.tmp) process, which owns the window.
$wizard = $null; $owner = $null
for ($i = 0; $i -lt 40 -and -not $wizard; $i++) {
    Start-Sleep -Milliseconds 500
    $owner = Get-Process | Where-Object { $_.MainWindowTitle -like "Setup - *" -and $_.ProcessName -like "LowEnd-*" } | Select-Object -First 1
    if ($owner) { $wizard = [Windows.Automation.AutomationElement]::FromHandle($owner.MainWindowHandle) }
}
if (-not $wizard) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue; throw "The installer window did not appear." }
$cands = @($proc.Id, $owner.Id)

function PageTitle { ($wizard.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition) | ForEach-Object { $_.Current.Name } | Where-Object { $_ } | Select-Object -First 6) -join " | " }
function Shot([string] $name) {
    Start-Sleep -Milliseconds 900
    Write-Host ("page: " + (PageTitle))
    $h = [IntPtr] $wizard.Current.NativeWindowHandle
    $r = New-Object Win+RECT; [Win]::GetWindowRect($h, [ref] $r) | Out-Null
    $bmp = New-Object Drawing.Bitmap(($r.R - $r.L), ($r.B - $r.T))
    $g = [Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
    [Win]::PrintWindow($h, $dc, 2) | Out-Null
    $g.ReleaseHdc($dc); $g.Dispose()
    $bmp.Save((Join-Path $OutDir "$name.png"), [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    Write-Host "saved $name.png"
}
function Press([string] $label) {
    $cond = New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::NameProperty, $label)
    $b = $wizard.FindFirst([Windows.Automation.TreeScope]::Descendants, $cond)
    if (-not $b) { throw "No control named '$label'." }
    # A themed button does not always offer UI Automation's Invoke pattern, so
    # it is clicked the way Windows itself would: BM_CLICK to its own handle.
    [Win]::SendMessage([IntPtr] $b.Current.NativeWindowHandle, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
}
try {
    Shot "1-welcome"
    Press "Next"
    Shot "2-licence"
    Press "I accept the agreement"
    Press "Next"
    Shot "3-components"
}
finally {
    Get-Process -Id $cands -ErrorAction SilentlyContinue | Stop-Process -Force
}
