param([switch]$CheckOnly, [switch]$RestoreOnly, [switch]$RestoreConfig, [switch]$Status, [switch]$MaximizeOnly, [string]$SteamExe = '', [string]$GameExe = '')
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class PBZWindow {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int index);
    [DllImport("user32.dll")] public static extern int SetWindowLong(IntPtr h, int index, int value);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int height, uint flags);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT rect);
    [DllImport("user32.dll")] public static extern bool AdjustWindowRectEx(ref RECT rect, int style, bool menu, int exStyle);
    [DllImport("user32.dll")] public static extern IntPtr GetSystemMenu(IntPtr h, bool revert);
    [DllImport("user32.dll")] public static extern uint EnableMenuItem(IntPtr menu, uint item, uint flags);
    [DllImport("user32.dll")] public static extern bool DrawMenuBar(IntPtr h);
    public static void EnableMaximize(IntPtr h) {
        int style = GetWindowLong(h, -16);
        int desired = style | 0x00010000 | 0x00040000;
        if (style != desired) {
            SetWindowLong(h, -16, desired);
            SetWindowPos(h, IntPtr.Zero, 0, 0, 0, 0, 0x37);
        }
        EnableMenuItem(GetSystemMenu(h, false), 0xF030, 0);
        DrawMenuBar(h);
    }
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int command);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT rect);
    [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr h, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct MONITORINFO {
        public int Size; public RECT Monitor, Work; public uint Flags;
    }
    [DllImport("user32.dll")] public static extern bool GetMonitorInfo(IntPtr monitor, ref MONITORINFO info);
    public static void RestoreSize(IntPtr h) {
        if (IsZoomed(h) || IsIconic(h)) ShowWindow(h, 9);
        EnableMaximize(h);
        SetClientSize(h);
    }

    public static void SetClientSize(IntPtr h) {
        IntPtr previous = SetThreadDpiAwarenessContext(new IntPtr(-4));
        try {
            for (int attempt = 0; attempt < 3; attempt++) {
                RECT outer, client;
                if (!GetWindowRect(h, out outer) || !GetClientRect(h, out client))
                    throw new InvalidOperationException("Cannot read PBZ window dimensions.");
                int width = outer.Right - outer.Left + 1280 - (client.Right - client.Left);
                int height = outer.Bottom - outer.Top + 720 - (client.Bottom - client.Top);
                MONITORINFO info = new MONITORINFO { Size = Marshal.SizeOf(typeof(MONITORINFO)) };
                if (!GetMonitorInfo(MonitorFromWindow(h, 2), ref info))
                    throw new InvalidOperationException("Cannot read monitor work area.");
                int x = info.Work.Left + Math.Max(0, (info.Work.Right-info.Work.Left-width)/2);
                int y = info.Work.Top + Math.Max(0, (info.Work.Bottom-info.Work.Top-height)/2);
                if (!SetWindowPos(h, IntPtr.Zero, x, y, width, height, 0x14))
                    throw new InvalidOperationException("Cannot resize PBZ window.");
                if (CorrectSize(h)) return;
            }
        } finally { if (previous != IntPtr.Zero) SetThreadDpiAwarenessContext(previous); }
    }
    public static bool CorrectSize(IntPtr h) {
        RECT r; return GetClientRect(h, out r) && r.Right-r.Left == 1280 && r.Bottom-r.Top == 720;
    }
}
"@
[void][PBZWindow]::SetThreadDpiAwarenessContext([IntPtr]::new(-4))
function Find-Steam {
    $candidates = New-Object System.Collections.Generic.List[string]
    try { $steamPath = (Get-ItemProperty -LiteralPath 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath } catch { $steamPath = '' }
    if ($steamPath) { $candidates.Add((Join-Path $steamPath 'steam.exe')) }
    foreach ($root in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) { if ($root) { $candidates.Add((Join-Path $root 'Steam\steam.exe')) } }
    return $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
function Find-Game([string]$steam) {
    if (!$steam) { return $null }
    $vdf = Join-Path (Split-Path -Parent $steam) 'steamapps\libraryfolders.vdf'
    $libraries = New-Object System.Collections.Generic.List[string]
    if (Test-Path -LiteralPath $vdf) {
        foreach ($match in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) { $libraries.Add($match.Groups[1].Value.Replace('\\', '\')) }
    }
    foreach ($library in $libraries) {
        $candidate = Join-Path $library 'steamapps\common\PBZ\platform\steam\PBZ.exe'
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    return $null
}
if (!$SteamExe) { $SteamExe = Find-Steam }
if (!$GameExe -and $SteamExe) { $GameExe = Find-Game $SteamExe }
if (!$SteamExe -or !$GameExe -or !(Test-Path -LiteralPath $SteamExe) -or !(Test-Path -LiteralPath $GameExe)) {
    throw 'Steam or PBZ executable was not found. Pass -SteamExe and -GameExe explicitly.'
}
$steamExe = $SteamExe
$gameExe = $GameExe
if ($CheckOnly) { Write-Output 'Script syntax, native helper and executable paths OK.'; exit 0 }
if ($RestoreConfig) {
    $restoreConfig = Join-Path $env:LOCALAPPDATA 'ProjectPBZ\Saved\Config\Windows\GameUserSettings.ini'
    $restoreBackup = $restoreConfig + '.windowed-launcher.bak'
    if (!(Test-Path -LiteralPath $restoreBackup)) { throw 'No windowed-launcher backup was found.' }
    Copy-Item -LiteralPath $restoreBackup -Destination $restoreConfig -Force
    Write-Output 'Restored GameUserSettings.ini from the windowed-launcher backup.'
    exit 0
}
if ($Status) {
    $active = @(Get-Process PBZ -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $gameExe -and $_.MainWindowHandle -ne 0 })
    if ($active.Count -ne 1) { throw 'Expected one PBZ game window.' }
    $handle = $active[0].MainWindowHandle
    $client = New-Object PBZWindow+RECT
    if (![PBZWindow]::GetClientRect($handle, [ref]$client)) { throw 'Cannot read PBZ client dimensions.' }
    $style = [PBZWindow]::GetWindowLong($handle, -16)
    $normal = ![PBZWindow]::IsZoomed($handle) -and ![PBZWindow]::IsIconic($handle)
    $windowed = ($style -band 0x00C00000) -eq 0x00C00000
    $result = [ordered]@{
        checkedAt = (Get-Date).ToString('o'); processId = $active[0].Id
        clientWidth = $client.Right - $client.Left; clientHeight = $client.Bottom - $client.Top
        normalWindow = $normal; captionedWindow = $windowed
        valid = $normal -and $windowed -and [PBZWindow]::CorrectSize($handle)
    }
    $result | ConvertTo-Json
    if (!$result.valid) { exit 2 }
    exit 0
}
try {
    $running = @(Get-Process PBZ -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) {
        $active = $running | Where-Object { $_.Path -eq $gameExe -and $_.MainWindowHandle -ne 0 } | Select-Object -First 1
        if (!$active) { throw 'PBZ is starting. Wait for its window, then run this script again.' }
        if ($MaximizeOnly) {
            [PBZWindow]::EnableMaximize($active.MainWindowHandle)
            [void][PBZWindow]::ShowWindow($active.MainWindowHandle, 3)
            Start-Sleep -Milliseconds 250
            if (![PBZWindow]::IsZoomed($active.MainWindowHandle)) { throw 'Could not maximize the PBZ window.' }
            exit 0
        }
        for ($attempt = 0; $attempt -lt 6; $attempt++) {
            $active.Refresh()
            [PBZWindow]::RestoreSize($active.MainWindowHandle)
            Start-Sleep -Milliseconds 250
        }
        if (![PBZWindow]::CorrectSize($active.MainWindowHandle)) { throw 'Could not restore the client area to 1280x720.' }
        exit 0
    }
    if ($RestoreOnly -or $MaximizeOnly) { throw 'PBZ is not running.' }

    # Preserve other settings and keep a backup before changing display settings.
    $config = Join-Path $env:LOCALAPPDATA 'ProjectPBZ\Saved\Config\Windows\GameUserSettings.ini'
    $section = '[/Script/ProjectPBZ.PBZSettingsLocal]'
    $values = [ordered]@{
        ResolutionSizeX=1280; ResolutionSizeY=720
        LastUserConfirmedResolutionSizeX=1280; LastUserConfirmedResolutionSizeY=720
        FullscreenMode=2; LastConfirmedFullscreenMode=2; PreferredFullscreenMode=2
        DesiredScreenWidth=1280; DesiredScreenHeight=720
        LastUserConfirmedDesiredScreenWidth=1280; LastUserConfirmedDesiredScreenHeight=720
    }
    $lines = [Collections.Generic.List[string]]::new()
    $backup = $config + '.windowed-launcher.bak'
    if (Test-Path -LiteralPath $config) {
        if (!(Test-Path -LiteralPath $backup)) { Copy-Item -LiteralPath $config -Destination $backup }
        $lines.AddRange([string[]][IO.File]::ReadAllLines($config))
    } else {
        [void][IO.Directory]::CreateDirectory((Split-Path $config))
    }
    $start = $lines.IndexOf($section)
    if ($start -lt 0) { $lines.Add($section); $start = $lines.Count - 1 }
    $end = $start + 1
    while ($end -lt $lines.Count -and !$lines[$end].StartsWith('[')) { $end++ }
    foreach ($key in $values.Keys) {
        $found = $false
        for ($i = $start + 1; $i -lt $end; $i++) {
            if ($lines[$i] -match ('^' + [regex]::Escape($key) + '=')) {
                $lines[$i] = "$key=$($values[$key])"; $found = $true
            }
        }
        if (!$found) { $lines.Insert($end, "$key=$($values[$key])"); $end++ }
    }
    [IO.File]::WriteAllLines($config, $lines, [Text.UTF8Encoding]::new($false))
    Start-Process -FilePath $steamExe -ArgumentList '-applaunch 4115450 -windowed -ResX=1280 -ResY=720' -WindowStyle Hidden
    $deadline = (Get-Date).AddMinutes(3)
    $game = $null
    do {
        Start-Sleep -Milliseconds 500
        $game = Get-Process PBZ -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -eq $gameExe -and $_.MainWindowHandle -ne 0 } |
            Select-Object -First 1
    } until ($game -or (Get-Date) -gt $deadline)
    if (!$game) { throw 'Timed out waiting for the PBZ game window.' }
    $handle = $game.MainWindowHandle
    [PBZWindow]::EnableMaximize($handle)
    [PBZWindow]::RestoreSize($handle)
    if (![PBZWindow]::CorrectSize($handle)) { throw 'Could not set the game client area to 1280x720.' }
    # Initialization is complete; leave subsequent window sizing to the user.
} catch {
    Add-Type -AssemblyName System.Windows.Forms
    [void][Windows.Forms.MessageBox]::Show($_.Exception.Message, 'PBZ Windowed Launcher')
    exit 1
}
