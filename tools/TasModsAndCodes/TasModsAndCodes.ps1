# TAS Mods and Codes
# Picks the mods and codes that the Linux (WSL) build of Unleashed Recompiled loads, by writing
# ~/.config/UnleashedRecomp/ModsDB.ini in WSL (the file cpkredir.ini's ModsDbIni points at).
# Run with Windows PowerShell: powershell.exe -STA -File TasModsAndCodes.ps1
# Keep this file ASCII: Windows PowerShell reads scripts without a BOM as ANSI.
# -DryRun prints the ModsDB.ini it would write for the current setup and exits (no window, no writes).
param([switch]$DryRun)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase

# ---- Paths --------------------------------------------------------------------------------------
# Set by Initialize-Paths from the user's settings (Settings... button); see the WSL section.
$ModsFolder = ''
$ModsFolderSource = ''
$WslRoot = ''
$WslHome = ''
$WslDistroLabel = ''
$PortableNote = ''
$ConfigDirLinux = ''
$ConfigDir = ''
$CpkRedirPath = ''
$CpkRedirNote = ''
$ModsDbLinux = ''
$ModsDbPath = ''
$SetupsDir = ''
# The game's code list (from this repository). Codes not in this build are shown under "Not in this build".
$ConfigDefPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\UnleashedRecomp\user\config_def.h'))

$AppTitle = 'TAS Mods and Codes'

# ---- Dark theme ---------------------------------------------------------------------------------
# Styles shared by every window: pasted into each XAML window's resources (THEME placeholder) and
# parsed as a ResourceDictionary for windows and menus built in code.
$ThemeStyles = @'
    <SolidColorBrush x:Key="TextBrush" Color="#E8E8EA"/>
    <SolidColorBrush x:Key="SubtleBrush" Color="#A0A0A8"/>
    <SolidColorBrush x:Key="AccentBrush" Color="#2F7BE0"/>
    <Style x:Key="ThumbStyle" TargetType="Thumb">
      <Setter Property="Template"><Setter.Value>
        <ControlTemplate TargetType="Thumb">
          <Border x:Name="T" Background="#4E4E56" CornerRadius="4"/>
          <ControlTemplate.Triggers>
            <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="T" Property="Background" Value="#6A6A74"/></Trigger>
            <Trigger Property="IsDragging" Value="True"><Setter TargetName="T" Property="Background" Value="#80808C"/></Trigger>
          </ControlTemplate.Triggers>
        </ControlTemplate>
      </Setter.Value></Setter>
    </Style>
    <Style x:Key="PageButton" TargetType="RepeatButton">
      <Setter Property="Focusable" Value="False"/>
      <Setter Property="IsTabStop" Value="False"/>
      <Setter Property="Template"><Setter.Value>
        <ControlTemplate TargetType="RepeatButton"><Border Background="Transparent"/></ControlTemplate>
      </Setter.Value></Setter>
    </Style>
    <Style TargetType="ScrollBar">
      <Setter Property="Width" Value="11"/>
      <Setter Property="MinWidth" Value="11"/>
      <Setter Property="Template"><Setter.Value>
        <ControlTemplate TargetType="ScrollBar">
          <Border Background="Transparent" Padding="2,1">
            <Track x:Name="PART_Track" IsDirectionReversed="True">
              <Track.DecreaseRepeatButton><RepeatButton Style="{StaticResource PageButton}" Command="ScrollBar.PageUpCommand"/></Track.DecreaseRepeatButton>
              <Track.IncreaseRepeatButton><RepeatButton Style="{StaticResource PageButton}" Command="ScrollBar.PageDownCommand"/></Track.IncreaseRepeatButton>
              <Track.Thumb><Thumb Style="{StaticResource ThumbStyle}"/></Track.Thumb>
            </Track>
          </Border>
        </ControlTemplate>
      </Setter.Value></Setter>
      <Style.Triggers>
        <Trigger Property="Orientation" Value="Horizontal">
          <Setter Property="Width" Value="Auto"/>
          <Setter Property="MinWidth" Value="0"/>
          <Setter Property="Height" Value="11"/>
          <Setter Property="MinHeight" Value="11"/>
          <Setter Property="Template"><Setter.Value>
            <ControlTemplate TargetType="ScrollBar">
              <Border Background="Transparent" Padding="1,2">
                <Track x:Name="PART_Track">
                  <Track.DecreaseRepeatButton><RepeatButton Style="{StaticResource PageButton}" Command="ScrollBar.PageLeftCommand"/></Track.DecreaseRepeatButton>
                  <Track.IncreaseRepeatButton><RepeatButton Style="{StaticResource PageButton}" Command="ScrollBar.PageRightCommand"/></Track.IncreaseRepeatButton>
                  <Track.Thumb><Thumb Style="{StaticResource ThumbStyle}"/></Track.Thumb>
                </Track>
              </Border>
            </ControlTemplate>
          </Setter.Value></Setter>
        </Trigger>
      </Style.Triggers>
    </Style>
    <Style TargetType="Button">
      <Setter Property="Foreground" Value="#E8E8EA"/>
      <Setter Property="Background" Value="#33333A"/>
      <Setter Property="BorderBrush" Value="#4A4A52"/>
      <Setter Property="Padding" Value="11,4"/>
      <Setter Property="Margin" Value="0,0,6,6"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Template"><Setter.Value>
        <ControlTemplate TargetType="Button">
          <Border x:Name="Bd" Background="{TemplateBinding Background}" BorderBrush="{TemplateBinding BorderBrush}"
                  BorderThickness="1" CornerRadius="4" Padding="{TemplateBinding Padding}" SnapsToDevicePixels="True">
            <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center" RecognizesAccessKey="True"/>
          </Border>
          <ControlTemplate.Triggers>
            <Trigger Property="IsMouseOver" Value="True">
              <Setter TargetName="Bd" Property="Background" Value="#3F3F48"/>
              <Setter TargetName="Bd" Property="BorderBrush" Value="#64646E"/>
            </Trigger>
            <Trigger Property="IsPressed" Value="True"><Setter TargetName="Bd" Property="Background" Value="#28282E"/></Trigger>
            <Trigger Property="IsKeyboardFocused" Value="True"><Setter TargetName="Bd" Property="BorderBrush" Value="#2F7BE0"/></Trigger>
            <Trigger Property="IsEnabled" Value="False"><Setter TargetName="Bd" Property="Opacity" Value="0.4"/></Trigger>
          </ControlTemplate.Triggers>
        </ControlTemplate>
      </Setter.Value></Setter>
    </Style>
    <Style x:Key="PrimaryButton" TargetType="Button" BasedOn="{StaticResource {x:Type Button}}">
      <Setter Property="Background" Value="#2F7BE0"/>
      <Setter Property="BorderBrush" Value="#4A90EC"/>
      <Setter Property="Foreground" Value="White"/>
      <Setter Property="FontWeight" Value="SemiBold"/>
      <Style.Triggers>
        <Trigger Property="IsMouseOver" Value="True"><Setter Property="Background" Value="#3D8AF0"/></Trigger>
      </Style.Triggers>
    </Style>
    <Style TargetType="CheckBox">
      <Setter Property="Foreground" Value="#E8E8EA"/>
      <Setter Property="Template"><Setter.Value>
        <ControlTemplate TargetType="CheckBox">
          <Grid Background="Transparent">
            <Grid.ColumnDefinitions><ColumnDefinition Width="Auto"/><ColumnDefinition Width="*"/></Grid.ColumnDefinitions>
            <Border x:Name="Box" Width="16" Height="16" CornerRadius="3" Background="#2A2A30" BorderBrush="#6A6A74"
                    BorderThickness="1" VerticalAlignment="Top" Margin="0,1,0,0" SnapsToDevicePixels="True">
              <!-- The box's inside is 14x14: this mark's bounds are 2.5..11.5 x 3.75..10.25, centred on (7, 7). -->
              <Path x:Name="Mark" Data="M 2.5 7.25 L 5.5 10.25 L 11.5 3.75" Stroke="White" StrokeThickness="2"
                    StrokeStartLineCap="Round" StrokeEndLineCap="Round" StrokeLineJoin="Round" Visibility="Collapsed"/>
            </Border>
            <ContentPresenter x:Name="CP" Grid.Column="1" Margin="8,0,0,0" VerticalAlignment="Center"/>
          </Grid>
          <ControlTemplate.Triggers>
            <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="Box" Property="BorderBrush" Value="#A8A8B2"/></Trigger>
            <Trigger Property="IsChecked" Value="True">
              <Setter TargetName="Mark" Property="Visibility" Value="Visible"/>
              <Setter TargetName="Box" Property="Background" Value="#2F7BE0"/>
              <Setter TargetName="Box" Property="BorderBrush" Value="#2F7BE0"/>
            </Trigger>
            <Trigger Property="HasContent" Value="False"><Setter TargetName="CP" Property="Margin" Value="0"/></Trigger>
            <Trigger Property="IsEnabled" Value="False"><Setter Property="Opacity" Value="0.5"/></Trigger>
          </ControlTemplate.Triggers>
        </ControlTemplate>
      </Setter.Value></Setter>
    </Style>
    <Style TargetType="ComboBox">
      <Setter Property="Foreground" Value="#E8E8EA"/>
      <Setter Property="Padding" Value="9,5,26,5"/>
      <Setter Property="SnapsToDevicePixels" Value="True"/>
      <Setter Property="Template"><Setter.Value>
        <ControlTemplate TargetType="ComboBox">
          <Grid>
            <ToggleButton Focusable="False" ClickMode="Press"
                          IsChecked="{Binding IsDropDownOpen, Mode=TwoWay, RelativeSource={RelativeSource TemplatedParent}}">
              <ToggleButton.Template>
                <ControlTemplate TargetType="ToggleButton">
                  <Border x:Name="Bd" Background="#2A2A30" BorderBrush="#4A4A52" BorderThickness="1" CornerRadius="4">
                    <Path HorizontalAlignment="Right" VerticalAlignment="Center" Margin="0,0,10,0"
                          Data="M 0 0 L 4 4 L 8 0" Stroke="#B4B4BC" StrokeThickness="1.6"/>
                  </Border>
                  <ControlTemplate.Triggers>
                    <Trigger Property="IsMouseOver" Value="True">
                      <Setter TargetName="Bd" Property="Background" Value="#323239"/>
                      <Setter TargetName="Bd" Property="BorderBrush" Value="#64646E"/>
                    </Trigger>
                    <Trigger Property="IsChecked" Value="True"><Setter TargetName="Bd" Property="BorderBrush" Value="#2F7BE0"/></Trigger>
                  </ControlTemplate.Triggers>
                </ControlTemplate>
              </ToggleButton.Template>
            </ToggleButton>
            <ContentPresenter IsHitTestVisible="False" Margin="{TemplateBinding Padding}" VerticalAlignment="Center"
                              HorizontalAlignment="Left" Content="{TemplateBinding SelectionBoxItem}"
                              ContentTemplate="{TemplateBinding SelectionBoxItemTemplate}"/>
            <Popup x:Name="PART_Popup" Placement="Bottom" IsOpen="{TemplateBinding IsDropDownOpen}"
                   AllowsTransparency="True" Focusable="False" PopupAnimation="Fade">
              <Border Background="#2A2A30" BorderBrush="#4A4A52" BorderThickness="1" CornerRadius="4" Margin="0,2,0,0"
                      MinWidth="{Binding ActualWidth, RelativeSource={RelativeSource TemplatedParent}}"
                      MaxHeight="{TemplateBinding MaxDropDownHeight}" Padding="2">
                <ScrollViewer SnapsToDevicePixels="True">
                  <ItemsPresenter KeyboardNavigation.DirectionalNavigation="Contained"/>
                </ScrollViewer>
              </Border>
            </Popup>
          </Grid>
          <ControlTemplate.Triggers>
            <Trigger Property="IsEnabled" Value="False"><Setter Property="Opacity" Value="0.5"/></Trigger>
          </ControlTemplate.Triggers>
        </ControlTemplate>
      </Setter.Value></Setter>
    </Style>
    <Style TargetType="ComboBoxItem">
      <Setter Property="Foreground" Value="#E8E8EA"/>
      <Setter Property="Template"><Setter.Value>
        <ControlTemplate TargetType="ComboBoxItem">
          <Border x:Name="Bd" Background="Transparent" CornerRadius="3" Padding="8,5"><ContentPresenter/></Border>
          <ControlTemplate.Triggers>
            <Trigger Property="IsSelected" Value="True"><Setter TargetName="Bd" Property="Background" Value="#27466B"/></Trigger>
            <Trigger Property="IsHighlighted" Value="True"><Setter TargetName="Bd" Property="Background" Value="#2F7BE0"/></Trigger>
          </ControlTemplate.Triggers>
        </ControlTemplate>
      </Setter.Value></Setter>
    </Style>
    <Style TargetType="ListBox">
      <Setter Property="Background" Value="#1F1F23"/>
      <Setter Property="BorderBrush" Value="#3A3A41"/>
      <Setter Property="Foreground" Value="#E8E8EA"/>
    </Style>
    <Style TargetType="ListBoxItem">
      <Setter Property="Foreground" Value="#E8E8EA"/>
      <Setter Property="Template"><Setter.Value>
        <ControlTemplate TargetType="ListBoxItem">
          <Border x:Name="Bd" Background="{TemplateBinding Background}" BorderBrush="{TemplateBinding BorderBrush}"
                  BorderThickness="{TemplateBinding BorderThickness}" Padding="{TemplateBinding Padding}" SnapsToDevicePixels="True">
            <ContentPresenter HorizontalAlignment="{TemplateBinding HorizontalContentAlignment}"
                              VerticalAlignment="{TemplateBinding VerticalContentAlignment}"/>
          </Border>
          <ControlTemplate.Triggers>
            <MultiTrigger>
              <MultiTrigger.Conditions>
                <Condition Property="IsMouseOver" Value="True"/>
                <Condition Property="Focusable" Value="True"/>
              </MultiTrigger.Conditions>
              <Setter TargetName="Bd" Property="Background" Value="#2C2C33"/>
            </MultiTrigger>
            <Trigger Property="IsSelected" Value="True"><Setter TargetName="Bd" Property="Background" Value="#27466B"/></Trigger>
          </ControlTemplate.Triggers>
        </ControlTemplate>
      </Setter.Value></Setter>
    </Style>
    <Style TargetType="GroupBox">
      <Setter Property="Foreground" Value="#E8E8EA"/>
      <Setter Property="Padding" Value="10,6,10,10"/>
      <Setter Property="Template"><Setter.Value>
        <ControlTemplate TargetType="GroupBox">
          <Border Background="#252529" BorderBrush="#36363C" BorderThickness="1" CornerRadius="6">
            <DockPanel>
              <ContentPresenter DockPanel.Dock="Top" ContentSource="Header" Margin="10,8,10,0"
                                TextElement.FontWeight="SemiBold" TextElement.FontSize="14" TextElement.Foreground="#F2F2F4"/>
              <ContentPresenter Margin="{TemplateBinding Padding}"/>
            </DockPanel>
          </Border>
        </ControlTemplate>
      </Setter.Value></Setter>
    </Style>
    <Style TargetType="ContextMenu">
      <Setter Property="Foreground" Value="#E8E8EA"/>
      <Setter Property="FontSize" Value="13"/>
      <Setter Property="Template"><Setter.Value>
        <ControlTemplate TargetType="ContextMenu">
          <Border Background="#2A2A30" BorderBrush="#4A4A52" BorderThickness="1" CornerRadius="5" Padding="3">
            <StackPanel IsItemsHost="True" KeyboardNavigation.DirectionalNavigation="Cycle"/>
          </Border>
        </ControlTemplate>
      </Setter.Value></Setter>
    </Style>
    <Style TargetType="MenuItem">
      <Setter Property="Foreground" Value="#E8E8EA"/>
      <Setter Property="Template"><Setter.Value>
        <ControlTemplate TargetType="MenuItem">
          <Border x:Name="Bd" Background="Transparent" CornerRadius="3" Padding="14,5,28,5">
            <ContentPresenter ContentSource="Header" RecognizesAccessKey="True"/>
          </Border>
          <ControlTemplate.Triggers>
            <Trigger Property="IsHighlighted" Value="True"><Setter TargetName="Bd" Property="Background" Value="#2F7BE0"/></Trigger>
            <Trigger Property="IsEnabled" Value="False"><Setter Property="Foreground" Value="#6C6C74"/></Trigger>
          </ControlTemplate.Triggers>
        </ControlTemplate>
      </Setter.Value></Setter>
    </Style>
    <Style x:Key="MenuSeparator" TargetType="Separator">
      <Setter Property="Template"><Setter.Value>
        <ControlTemplate TargetType="Separator"><Border Height="1" Background="#44444C" Margin="6,4"/></ControlTemplate>
      </Setter.Value></Setter>
    </Style>
    <Style TargetType="ToolTip">
      <Setter Property="Background" Value="#2A2A30"/>
      <Setter Property="Foreground" Value="#E8E8EA"/>
      <Setter Property="BorderBrush" Value="#4A4A52"/>
    </Style>
    <Style TargetType="TextBox">
      <Setter Property="Background" Value="#2A2A30"/>
      <Setter Property="Foreground" Value="#E8E8EA"/>
      <Setter Property="BorderBrush" Value="#4A4A52"/>
      <Setter Property="CaretBrush" Value="#E8E8EA"/>
      <Setter Property="SelectionBrush" Value="#2F7BE0"/>
      <Setter Property="Padding" Value="6,4"/>
    </Style>
'@

function New-Theme {
    return [Windows.Markup.XamlReader]::Parse(
        '<ResourceDictionary xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" ' +
        'xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">' + $ThemeStyles + '</ResourceDictionary>')
}

# Dark title bars (Windows 10 2004+ / 11). Harmless where unsupported.
try {
    Add-Type -Namespace TasModsAndCodes -Name Dwm -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("dwmapi.dll")]
public static extern int DwmSetWindowAttribute(System.IntPtr hwnd, int attribute, ref int value, int size);
'@
    $HasDwm = $true
} catch { $HasDwm = $false }

function Set-DarkTitleBar($w) {
    if (-not $HasDwm) { return }
    try {
        $hwnd = (New-Object System.Windows.Interop.WindowInteropHelper($w)).Handle
        $on = 1
        # 20 = DWMWA_USE_IMMERSIVE_DARK_MODE; older Windows 10 builds used 19.
        if ([TasModsAndCodes.Dwm]::DwmSetWindowAttribute($hwnd, 20, [ref]$on, 4) -ne 0) {
            [void][TasModsAndCodes.Dwm]::DwmSetWindowAttribute($hwnd, 19, [ref]$on, 4)
        }
    } catch { }
}

function New-DarkWindow([string]$title) {
    $w = New-Object System.Windows.Window
    $w.Title = $title
    $w.FontSize = 13
    $w.Background = New-Object System.Windows.Media.SolidColorBrush ([System.Windows.Media.Color]::FromRgb(0x1B, 0x1B, 0x1E))
    $w.Foreground = New-Object System.Windows.Media.SolidColorBrush ([System.Windows.Media.Color]::FromRgb(0xE8, 0xE8, 0xEA))
    $w.Resources.MergedDictionaries.Add((New-Theme))
    $w.ShowInTaskbar = $false
    $w.ResizeMode = 'NoResize'
    $w.SizeToContent = 'WidthAndHeight'
    $owner = $script:window
    if ($owner -and $owner.IsLoaded) { $w.Owner = $owner; $w.WindowStartupLocation = 'CenterOwner' }
    else { $w.WindowStartupLocation = 'CenterScreen' }
    $w.Add_SourceInitialized({ Set-DarkTitleBar $this })
    return $w
}

# Message box in the app's colours. Returns 'OK', 'Yes', 'No' or 'Cancel'.
function Show-Message([string]$text, [string]$icon = 'Information', [string]$buttons = 'OK') {
    $w = New-DarkWindow $AppTitle
    $names = switch ($buttons) { 'YesNo' { 'Yes', 'No' } 'YesNoCancel' { 'Yes', 'No', 'Cancel' } default { , 'OK' } }
    $script:DialogAnswer = $(if ($buttons -eq 'YesNoCancel') { 'Cancel' } elseif ($buttons -eq 'YesNo') { 'No' } else { 'OK' })
    $panel = New-Object System.Windows.Controls.DockPanel
    $panel.Margin = '20,18,20,14'
    $row = New-Object System.Windows.Controls.StackPanel
    $row.Orientation = 'Horizontal'
    $row.HorizontalAlignment = 'Right'
    $row.Margin = '0,18,0,0'
    [System.Windows.Controls.DockPanel]::SetDock($row, 'Bottom')
    $first = $true
    foreach ($n in $names) {
        $b = New-Object System.Windows.Controls.Button
        $b.Content = $n
        $b.Tag = $n
        $b.MinWidth = 84
        if ($first) { $b.Style = $w.FindResource('PrimaryButton'); $b.IsDefault = $true; $first = $false }
        if ($n -eq 'Cancel' -or ($n -eq 'No' -and $buttons -eq 'YesNo') -or $names.Count -eq 1) { $b.IsCancel = $true }
        $b.Add_Click({
            $script:DialogAnswer = [string]$this.Tag
            [System.Windows.Window]::GetWindow($this).Close()
        })
        [void]$row.Children.Add($b)
    }
    [void]$panel.Children.Add($row)
    $msg = New-Object System.Windows.Controls.TextBlock
    $msg.Text = $text
    $msg.TextWrapping = 'Wrap'
    $msg.MaxWidth = 520
    $msg.MinWidth = 300
    if ($icon -eq 'Error') {
        $msg.Foreground = New-Object System.Windows.Media.SolidColorBrush ([System.Windows.Media.Color]::FromRgb(0xF2, 0x8B, 0x8B))
    }
    [void]$panel.Children.Add($msg)
    $w.Content = $panel
    [void]$w.ShowDialog()
    return $script:DialogAnswer
}

# Text prompt in the app's colours. Returns the text, or $null if cancelled.
function Show-Input([string]$prompt, [string]$title, [string]$default) {
    $w = New-DarkWindow $title
    $script:DialogAnswer = $null
    $panel = New-Object System.Windows.Controls.StackPanel
    $panel.Margin = '20,18,20,14'
    $label = New-Object System.Windows.Controls.TextBlock
    $label.Text = $prompt
    $label.TextWrapping = 'Wrap'
    $label.MaxWidth = 440
    $label.Margin = '0,0,0,8'
    [void]$panel.Children.Add($label)
    $box = New-Object System.Windows.Controls.TextBox
    $box.Text = $default
    $box.Width = 440
    [void]$panel.Children.Add($box)
    $row = New-Object System.Windows.Controls.StackPanel
    $row.Orientation = 'Horizontal'
    $row.HorizontalAlignment = 'Right'
    $row.Margin = '0,16,0,0'
    $ok = New-Object System.Windows.Controls.Button
    $ok.Content = 'OK'
    $ok.MinWidth = 84
    $ok.IsDefault = $true
    $ok.Style = $w.FindResource('PrimaryButton')
    $ok.Tag = $box
    $ok.Add_Click({
        $script:DialogAnswer = $this.Tag.Text
        [System.Windows.Window]::GetWindow($this).Close()
    })
    $cancel = New-Object System.Windows.Controls.Button
    $cancel.Content = 'Cancel'
    $cancel.MinWidth = 84
    $cancel.IsCancel = $true
    [void]$row.Children.Add($ok)
    [void]$row.Children.Add($cancel)
    [void]$panel.Children.Add($row)
    $w.Content = $panel
    $w.Add_ContentRendered({ $b = $this.Content.Children[1]; $b.Focus(); $b.SelectAll() })
    [void]$w.ShowDialog()
    return $script:DialogAnswer
}

# ---- INI helpers ----------------------------------------------------------------------------------
function Read-Ini([string]$path) {
    $ini = New-Object 'System.Collections.Generic.Dictionary[string,object]'
    $section = ''
    $ini[$section] = New-Object 'System.Collections.Generic.Dictionary[string,string]'
    foreach ($raw in [IO.File]::ReadAllLines($path)) {
        $line = $raw.Trim()
        if ($line -eq '' -or $line.StartsWith(';') -or $line.StartsWith('#')) { continue }
        if ($line -match '^\[(.+)\]$') {
            $section = $Matches[1]
            if (-not $ini.ContainsKey($section)) { $ini[$section] = New-Object 'System.Collections.Generic.Dictionary[string,string]' }
            continue
        }
        $eq = $line.IndexOf('=')
        if ($eq -lt 1) { continue }
        $key = $line.Substring(0, $eq).Trim()
        $value = $line.Substring($eq + 1).Trim()
        if ($value.Length -ge 2 -and $value.StartsWith('"') -and $value.EndsWith('"')) { $value = $value.Substring(1, $value.Length - 2) }
        $ini[$section][$key] = $value
    }
    return $ini
}

function Get-IniValue($ini, [string]$section, [string]$key, [string]$default = '') {
    if ($ini.ContainsKey($section) -and $ini[$section].ContainsKey($key)) { return $ini[$section][$key] }
    return $default
}

function Get-IniCount($ini, [string]$section, [string]$key) {
    $n = 0
    [void][int]::TryParse((Get-IniValue $ini $section $key '0'), [ref]$n)
    return $n
}

function Write-TextFile([string]$path, [string]$text) {
    [IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding($false)))
}

# ---- WSL ----------------------------------------------------------------------------------------
# Settings (Settings... button): WSL distro, Hedge Mod Manager's mods folder, Linux build folder.
# Empty = work it out (default distro; mods folder from the mods in the game's list; no build folder).
$SettingsPath = Join-Path $env:APPDATA 'TasModsAndCodes\settings.ini'
$Settings = @{ Distro = ''; ModsFolder = ''; BuildFolder = '' }

function Read-Settings {
    if (-not (Test-Path -LiteralPath $SettingsPath)) { return }
    try {
        $ini = Read-Ini $SettingsPath
        foreach ($k in @($Settings.Keys)) { $Settings[$k] = Get-IniValue $ini 'Settings' $k }
    } catch { }
}

function Save-Settings {
    $dir = Split-Path $SettingsPath -Parent
    if (-not (Test-Path -LiteralPath $dir)) { [void](New-Item -ItemType Directory -Path $dir) }
    $text = "[Settings]`r`n"
    foreach ($k in 'Distro', 'ModsFolder', 'BuildFolder') { $text += "$k=`"$($Settings[$k])`"`r`n" }
    Write-TextFile $SettingsPath $text
}

function Get-WslArgs {
    if ($Settings.Distro) { return @('-d', $Settings.Distro) }
    return @()
}

function Get-WslDistros {
    try {
        # wsl.exe -l prints UTF-16: drop the NULs Windows PowerShell leaves in.
        return @(& wsl.exe -l -q 2>$null | ForEach-Object { ($_ -replace "`0", '').Trim() } | Where-Object { $_ })
    } catch { return @() }
}

function Get-DefaultDistro {
    try {
        $lxss = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Lxss'
        $id = (Get-ItemProperty -LiteralPath $lxss -Name DefaultDistribution -ErrorAction Stop).DefaultDistribution
        return (Get-ItemProperty -LiteralPath (Join-Path $lxss $id) -Name DistributionName -ErrorAction Stop).DistributionName
    } catch { return '' }
}

function Test-GameRunning {
    $a = Get-WslArgs
    & wsl.exe @a -e pgrep -x UnleashedRecomp > $null 2>&1
    return ($LASTEXITCODE -eq 0)
}

# Accepts a Linux path, a drive path (F:\...) or a \\wsl.localhost\<distro>\... path; returns a Linux path.
function ConvertTo-LinuxPathAny([string]$path) {
    $p = $path.Trim().Trim('"')
    if (-not $p) { return '' }
    if ($p.StartsWith('/')) { return $p.TrimEnd('/') }
    if ($p -match '^\\\\wsl(?:\.localhost|\$)\\[^\\]+(\\.*)?$') {
        $rest = if ($Matches[1]) { $Matches[1] } else { '\' }
        return ($rest.Replace('\', '/')).TrimEnd('/')
    }
    if ($p -match '^([a-zA-Z]):\\?(.*)$') {
        return ('/mnt/' + $Matches[1].ToLower() + '/' + $Matches[2].Replace('\', '/')).TrimEnd('/')
    }
    return $p.Replace('\', '/').TrimEnd('/')
}

# Works out where the game reads its mod list, like the game does (user/paths.cpp, mod/mod_loader.cpp):
# portable.txt next to the executable -> that folder; else ~/.config/UnleashedRecomp (or ~/.UnleashedRecomp
# without ~/.config). cpkredir.ini from there, else from the executable's folder; ModsDbIni inside it.
# Returns '' or an error message.
function Initialize-Paths {
    $a = Get-WslArgs
    try {
        $out = @(& wsl.exe @a -e sh -c 'echo $HOME; wslpath -w /; if [ -d $HOME/.config ]; then echo config; else echo noconfig; fi' 2>$null)
    } catch { $out = @() }
    $distroName = if ($Settings.Distro) { $Settings.Distro } else { 'the default WSL distro' }
    if ($out.Count -lt 3 -or -not $out[0].StartsWith('/')) {
        return "Couldn't reach WSL ($distroName). Check that it is installed and starts, or pick another distro in Settings."
    }
    $script:WslHome = $out[0].Trim()
    $script:WslRoot = $out[1].Trim().TrimEnd('\')
    $script:WslDistroLabel = if ($Settings.Distro) { $Settings.Distro } else { $WslRoot -replace '^.*\\', '' }
    $hasConfig = ($out[2].Trim() -eq 'config')

    $build = $Settings.BuildFolder
    $buildWin = if ($build) { ConvertTo-WindowsPath $build } else { '' }
    if ($build -and -not (Test-Path -LiteralPath $buildWin)) {
        return "The Linux build folder doesn't exist: $build"
    }
    $script:PortableNote = ''
    if ($build -and (Test-Path -LiteralPath (Join-Path $buildWin 'portable.txt'))) {
        $script:ConfigDirLinux = $build
        $script:PortableNote = 'portable install'
    } elseif ($hasConfig) {
        $script:ConfigDirLinux = "$WslHome/.config/UnleashedRecomp"
    } else {
        $script:ConfigDirLinux = "$WslHome/.UnleashedRecomp"
    }
    $script:ConfigDir = ConvertTo-WindowsPath $ConfigDirLinux
    $script:CpkRedirPath = Join-Path $ConfigDir 'cpkredir.ini'
    if (-not (Test-Path -LiteralPath $CpkRedirPath) -and $build -and (Test-Path -LiteralPath (Join-Path $buildWin 'cpkredir.ini'))) {
        $script:CpkRedirPath = Join-Path $buildWin 'cpkredir.ini'
    }
    $script:ModsDbLinux = "$ConfigDirLinux/ModsDB.ini"
    $script:CpkRedirNote = ''
    if (Test-Path -LiteralPath $CpkRedirPath) {
        $cpk = Read-Ini $CpkRedirPath
        $configured = Get-IniValue $cpk 'CPKREDIR' 'ModsDbIni'
        if ($configured) { $script:ModsDbLinux = $configured }
        if ((Get-IniValue $cpk 'CPKREDIR' 'Enabled' '1') -eq '0') {
            $script:CpkRedirNote = 'cpkredir.ini has Enabled=0: the game loads no mods or codes until it is set back to 1.'
        }
    }
    $script:ModsDbPath = ConvertTo-WindowsPath $ModsDbLinux
    $script:SetupsDir = Join-Path $ConfigDir 'TAS setups'

    # Mods folder: the setting, else the folder holding the mods already in the game's list.
    $script:ModsFolder = ''
    $script:ModsFolderSource = ''
    if ($Settings.ModsFolder) {
        $script:ModsFolder = $Settings.ModsFolder
        $script:ModsFolderSource = 'settings'
    } elseif (Test-Path -LiteralPath $ModsDbPath) {
        try {
            $db = Read-Ini $ModsDbPath
            if ($db.ContainsKey('Mods')) {
                foreach ($p in $db['Mods'].Values) {
                    $folder = Split-Path (Split-Path (ConvertTo-WindowsPath $p) -Parent) -Parent
                    if ($folder -and (Test-Path -LiteralPath $folder)) {
                        $script:ModsFolder = $folder
                        $script:ModsFolderSource = 'the game''s mod list'
                        break
                    }
                }
            }
        } catch { }
    }
    return ''
}

function ConvertTo-WindowsPath([string]$linuxPath) {
    if ($linuxPath -match '^/mnt/([a-zA-Z])(/.*)?$') {
        $rest = if ($Matches[2]) { $Matches[2] } else { '/' }
        return $Matches[1].ToUpper() + ':' + $rest.Replace('/', '\')
    }
    return $WslRoot + $linuxPath.Replace('/', '\')
}

function ConvertTo-LinuxPath([string]$windowsPath) {
    if ($windowsPath -match '^([a-zA-Z]):\\(.*)$') {
        return '/mnt/' + $Matches[1].ToLower() + '/' + $Matches[2].Replace('\', '/')
    }
    if (-not $WslRoot) { return $windowsPath.Replace('\', '/') }
    if ($windowsPath.StartsWith($WslRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        return $windowsPath.Substring($WslRoot.Length).Replace('\', '/')
    }
    return $windowsPath.Replace('\', '/')
}

# ---- Mods ---------------------------------------------------------------------------------------
# Keyed by the mod.ini's Linux path (what ModsDB.ini stores).
$Mods = New-Object 'System.Collections.Generic.Dictionary[string,object]'
$Ids = New-Object 'System.Collections.Generic.Dictionary[string,string]'

function Add-ModEntry([string]$linuxPath) {
    if ($Mods.ContainsKey($linuxPath)) { return $Mods[$linuxPath] }
    $winPath = ConvertTo-WindowsPath $linuxPath
    $folder = Split-Path (Split-Path $winPath -Parent) -Leaf
    $mod = [pscustomobject]@{
        Path = $linuxPath; WinPath = $winPath; Title = $folder; Author = ''; Version = ''; Description = ''
        Missing = $true; SchemaPath = ''
    }
    if (Test-Path -LiteralPath $winPath) {
        $mod.Missing = $false
        try {
            $ini = Read-Ini $winPath
            $title = Get-IniValue $ini 'Desc' 'Title'
            if ($title) { $mod.Title = $title }
            $mod.Author = Get-IniValue $ini 'Desc' 'Author'
            $mod.Version = Get-IniValue $ini 'Desc' 'Version'
            $mod.Description = (Get-IniValue $ini 'Desc' 'Description').Replace('\n', "`n")
            $schema = Get-IniValue $ini 'Main' 'ConfigSchemaFile'
            if ($schema) {
                $schemaPath = Join-Path (Split-Path $winPath -Parent) $schema
                if (Test-Path -LiteralPath $schemaPath -PathType Leaf) { $mod.SchemaPath = $schemaPath }
            }
        } catch { }
    }
    $Mods[$linuxPath] = $mod
    return $mod
}

function Test-ModsFolder {
    return ([bool]$ModsFolder -and (Test-Path -LiteralPath $ModsFolder -PathType Container))
}

function Find-Mods {
    if (-not (Test-ModsFolder)) { return }
    foreach ($dir in Get-ChildItem -LiteralPath $ModsFolder -Directory) {
        $ini = Join-Path $dir.FullName 'mod.ini'
        if (Test-Path -LiteralPath $ini) { [void](Add-ModEntry (ConvertTo-LinuxPath $ini)) }
    }
}

# Reads a ModsDB.ini (the live one or a saved setup) into: enabled mod paths in load order, codes.
function Import-ModsDb([string]$path) {
    $result = @{
        Enabled = New-Object 'System.Collections.Generic.List[string]'
        Codes = New-Object 'System.Collections.Generic.HashSet[string]'
    }
    if (-not (Test-Path -LiteralPath $path)) { return $result }
    $ini = Read-Ini $path
    if ($ini.ContainsKey('Mods')) {
        foreach ($pair in $ini['Mods'].GetEnumerator()) {
            if (-not $Ids.ContainsKey($pair.Value)) { $Ids[$pair.Value] = $pair.Key }
        }
    }
    $count = Get-IniCount $ini 'Main' 'ActiveModCount'
    for ($i = 0; $i -lt $count; $i++) {
        $id = Get-IniValue $ini 'Main' "ActiveMod$i"
        if (-not $id) { continue }
        $modPath = Get-IniValue $ini 'Mods' $id
        if (-not $modPath -or $result.Enabled.Contains($modPath)) { continue }
        [void](Add-ModEntry $modPath)
        $result.Enabled.Add($modPath)
    }
    $count = Get-IniCount $ini 'Codes' 'CodeCount'
    for ($i = 0; $i -lt $count; $i++) {
        $code = Get-IniValue $ini 'Codes' "Code$i"
        if ($code) { [void]$result.Codes.Add($code) }
    }
    return $result
}

# Hedge Mod Manager's active mods (Windows), in the game's load order.
function Get-HmmEnabled {
    $list = New-Object 'System.Collections.Generic.List[string]'
    if (-not (Test-ModsFolder)) { return ,$list }
    $hmmDb = Join-Path $ModsFolder 'ModsDB.ini'
    if (-not (Test-Path -LiteralPath $hmmDb)) { return ,$list }
    $ini = Read-Ini $hmmDb
    $count = Get-IniCount $ini 'Main' 'ActiveModCount'
    for ($i = 0; $i -lt $count; $i++) {
        $id = Get-IniValue $ini 'Main' "ActiveMod$i"
        $winPath = Get-IniValue $ini 'Mods' $id
        if (-not $winPath) { continue }
        $linuxPath = ConvertTo-LinuxPath $winPath
        [void](Add-ModEntry $linuxPath)
        if (-not $Ids.ContainsKey($linuxPath)) { $Ids[$linuxPath] = $id }
        if (-not $list.Contains($linuxPath)) { $list.Add($linuxPath) }
    }
    if ((Get-IniValue $ini 'Main' 'ReverseLoadOrder' '0') -eq '1') { $list.Reverse() }
    return ,$list
}

# Reuse Hedge Mod Manager's IDs for mods the Linux list doesn't have yet (keeps saves stable).
function Import-HmmIds {
    if (-not (Test-ModsFolder)) { return }
    $hmmDb = Join-Path $ModsFolder 'ModsDB.ini'
    if (-not (Test-Path -LiteralPath $hmmDb)) { return }
    $ini = Read-Ini $hmmDb
    if (-not $ini.ContainsKey('Mods')) { return }
    foreach ($pair in $ini['Mods'].GetEnumerator()) {
        $linuxPath = ConvertTo-LinuxPath $pair.Value
        if (-not $Ids.ContainsKey($linuxPath)) { $Ids[$linuxPath] = $pair.Key }
    }
}

# ---- Mod options (Hedge Mod Manager's "Configure") ----------------------------------------------
# The schema (ConfigSchemaFile, JSON) lists groups of options; each option's value is stored in the
# schema's IniFile (in practice the mod's own mod.ini) under [<group Name>] <element Name>=. Options
# are almost always IncludeDirN folders, so changing one changes what the mod loads.

function Get-IniValueAnyCase($ini, [string]$section, [string]$key) {
    foreach ($s in $ini.Keys) {
        if ($s -ieq $section) {
            foreach ($k in $ini[$s].Keys) { if ($k -ieq $key) { return $ini[$s][$k] } }
        }
    }
    return $null
}

function Join-Description($value) {
    if ($null -eq $value) { return '' }
    return ((@($value) | ForEach-Object { [string]$_ }) -join "`n").Trim()
}

function ConvertTo-ComparableValue([string]$value) {
    $v = $value.Trim().Replace('\', '/')
    while ($v.StartsWith('./')) { $v = $v.Substring(2) }
    return $v.TrimEnd('/').ToLowerInvariant()
}

# Reads a mod's options: groups of elements with their choices and current values.
function Get-ModOptions($mod) {
    if (-not $mod.SchemaPath) { return $null }
    $text = [IO.File]::ReadAllText($mod.SchemaPath)
    # Mod authors leave trailing commas, which Windows PowerShell's JSON reader rejects.
    $json = [regex]::Replace($text, ',(\s*[\]}])', '$1') | ConvertFrom-Json
    $iniName = if ($json.IniFile) { [string]$json.IniFile } else { 'mod.ini' }
    $iniPath = Join-Path (Split-Path $mod.WinPath -Parent) $iniName
    $ini = if (Test-Path -LiteralPath $iniPath) { Read-Ini $iniPath } else { $null }
    $includeDirCount = -1
    if ($iniPath -eq $mod.WinPath -and $ini) { $includeDirCount = Get-IniCount $ini 'Main' 'IncludeDirCount' }

    $groups = New-Object 'System.Collections.Generic.List[object]'
    foreach ($g in @($json.Groups)) {
        $elements = New-Object 'System.Collections.Generic.List[object]'
        foreach ($e in @($g.Elements)) {
            if (-not $e.Name) { continue }
            $type = [string]$e.Type
            $choices = New-Object 'System.Collections.Generic.List[object]'
            $enum = $null
            if ($type -and $json.Enums -and ($json.Enums.PSObject.Properties.Name -contains $type)) { $enum = $json.Enums.$type }
            foreach ($c in @($enum)) {
                if ($null -eq $c) { continue }
                $choices.Add(@{ Name = [string]$c.DisplayName; Value = [string]$c.Value; Description = (Join-Description $c.Description) })
            }
            $default = if ($null -ne $e.DefaultValue) { [string]$e.DefaultValue } else { '' }
            $current = if ($ini) { Get-IniValueAnyCase $ini $g.Name $e.Name } else { $null }
            $ignored = $false
            if ($includeDirCount -ge 0 -and $e.Name -match '^IncludeDir(\d+)$' -and [int]$Matches[1] -ge $includeDirCount) { $ignored = $true }
            $elements.Add(@{
                Section = [string]$g.Name; Key = [string]$e.Name; Type = $type
                Label = $(if ($e.DisplayName) { [string]$e.DisplayName } else { [string]$e.Name })
                Description = (Join-Description $e.Description)
                Choices = $choices; Default = $default
                Value = $(if ($null -ne $current) { $current } else { $default })
                Stored = ($null -ne $current); Ignored = $ignored; IncludeDirCount = $includeDirCount
            })
        }
        if ($elements.Count) {
            $groups.Add(@{ Title = $(if ($g.DisplayName) { [string]$g.DisplayName } else { [string]$g.Name }); Elements = $elements })
        }
    }
    return @{ IniPath = $iniPath; Groups = $groups }
}

function Find-Choice($element, [string]$value) {
    $want = ConvertTo-ComparableValue $value
    foreach ($c in $element.Choices) { if ((ConvertTo-ComparableValue $c.Value) -eq $want) { return $c } }
    return $null
}

function Get-ValueLabel($element, [string]$value) {
    $choice = Find-Choice $element $value
    if ($choice) { return $choice.Name }
    if ($element.Type -ieq 'bool') { return $value }
    if ($value -eq '') { return '(empty)' }
    return $value
}

# One line per option for the details box.
function Get-OptionSummary($mod) {
    try { $options = Get-ModOptions $mod } catch { return "Options: couldn't read $(Split-Path $mod.SchemaPath -Leaf): $($_.Exception.Message)" }
    if (-not $options) { return '' }
    $lines = @()
    foreach ($g in $options.Groups) {
        foreach ($e in $g.Elements) {
            $line = "  $($e.Label): $(Get-ValueLabel $e $e.Value)"
            if ($e.Ignored) { $line += '  (not loaded: IncludeDirCount is ' + $e.IncludeDirCount + ')' }
            $lines += $line
        }
    }
    return "Options:`n" + ($lines -join "`n")
}

# Changes [section] key="value" lines in place, keeping the rest of the file (encoding, line ends) as it is.
# Changes `Key = value` lines of one [section] of a TOML file in place (values already formatted:
# strings quoted, numbers bare), keeping every other line and the line ends as they are.
function Set-TomlValues([string]$path, [string]$section, $values) {
    $text = if (Test-Path -LiteralPath $path) { [IO.File]::ReadAllText($path) } else { '' }
    $nl = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
    $endsWithNewLine = ($text -eq '' -or $text.EndsWith("`n"))
    $lines = New-Object 'System.Collections.Generic.List[string]'
    foreach ($l in ($text -split "\r?\n")) { $lines.Add($l) }
    if ($endsWithNewLine -and $lines.Count -and $lines[$lines.Count - 1] -eq '') { $lines.RemoveAt($lines.Count - 1) }
    foreach ($key in $values.Keys) {
        $inSection = $false; $found = $false; $keyLine = -1; $lastLine = -1
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $t = $lines[$i].Trim()
            if ($t -match '^\[(.+)\]$') {
                if ($inSection) { break }
                if ($Matches[1].Trim() -eq $section) { $inSection = $true; $found = $true; $lastLine = $i }
                continue
            }
            if (-not $inSection -or $t -eq '' -or $t.StartsWith('#')) { continue }
            $lastLine = $i
            if ($t -match ('^' + [regex]::Escape($key) + '\s*=')) { $keyLine = $i }
        }
        $newLine = "$key = $($values[$key])"
        if ($keyLine -ge 0) { $lines[$keyLine] = $newLine }
        elseif ($found) { $lines.Insert($lastLine + 1, $newLine) }
        else {
            if ($lines.Count -and $lines[$lines.Count - 1].Trim() -ne '') { $lines.Add('') }
            $lines.Add("[$section]")
            $lines.Add($newLine)
        }
    }
    $out = $lines -join $nl
    if ($endsWithNewLine) { $out += $nl }
    [IO.File]::WriteAllText($path, $out, (New-Object System.Text.UTF8Encoding($false)))
}

function Set-IniValues([string]$path, $changes) {
    $bom = $false
    $text = ''
    if (Test-Path -LiteralPath $path) {
        $bytes = [IO.File]::ReadAllBytes($path)
        $bom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
        $start = if ($bom) { 3 } else { 0 }
        $text = (New-Object System.Text.UTF8Encoding($false)).GetString($bytes, $start, $bytes.Length - $start)
    }
    $nl = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
    $endsWithNewLine = ($text -eq '' -or $text.EndsWith("`n"))
    $lines = New-Object 'System.Collections.Generic.List[string]'
    foreach ($l in ($text -split "\r?\n")) { $lines.Add($l) }
    if ($endsWithNewLine -and $lines.Count -and $lines[$lines.Count - 1] -eq '') { $lines.RemoveAt($lines.Count - 1) }

    foreach ($c in $changes) {
        $inSection = $false
        $sectionFound = $false
        $keyLine = -1
        $lastLine = -1
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $t = $lines[$i].Trim()
            if ($t -match '^\[(.+)\]$') {
                if ($inSection) { break }
                if ($Matches[1] -ieq $c.Section) { $inSection = $true; $sectionFound = $true; $lastLine = $i }
                continue
            }
            if (-not $inSection -or $t -eq '' -or $t.StartsWith(';')) { continue }
            $lastLine = $i
            $eq = $t.IndexOf('=')
            if ($eq -gt 0 -and $t.Substring(0, $eq).Trim() -ieq $c.Key) { $keyLine = $i }
        }
        $newLine = "$($c.Key)=`"$($c.Value)`""
        if ($keyLine -ge 0) { $lines[$keyLine] = $newLine }
        elseif ($sectionFound) { $lines.Insert($lastLine + 1, $newLine) }
        else {
            if ($lines.Count -and $lines[$lines.Count - 1].Trim() -ne '') { $lines.Add('') }
            $lines.Add("[$($c.Section)]")
            $lines.Add($newLine)
        }
    }
    $out = $lines -join $nl
    if ($endsWithNewLine) { $out += $nl }
    [IO.File]::WriteAllText($path, $out, (New-Object System.Text.UTF8Encoding($bom)))
}

# ---- Codes --------------------------------------------------------------------------------------
$FallbackCodes = @(
    'AllowCancellingUnleash', 'DisableAutoSaveWarning', 'DisableBoostFilter', 'DisableDLCIcon',
    'DisableDPadMovement', 'DisableDWMRoundedCorners', 'DisableLowResolutionFontOnCustomUI',
    'EnableEventCollisionDebugView', 'EnableGIMipLevelDebugView', 'EnableObjectCollisionDebugView',
    'EnableStageCollisionDebugView', 'FixEggmanlandUsingEventGalleryTransition', 'FixUnleashOutOfControlDrain',
    'HomingAttackOnJump', 'HUDToggleKey', 'RestoreReadyGoSkip', 'SaveScoreAtCheckpoints', 'SkipIntroLogos',
    'UseAlternateTitle', 'UseArrowsForTimeOfDayTransition', 'UseOfficialTitleOnTitleBar')

$SupportedCodes = $FallbackCodes
if (Test-Path -LiteralPath $ConfigDefPath) {
    $found = @([regex]::Matches([IO.File]::ReadAllText($ConfigDefPath), 'CONFIG_DEFINE_HIDDEN\("Codes",\s*bool,\s*(\w+)') |
        ForEach-Object { $_.Groups[1].Value })
    if ($found.Count -gt 0) { $SupportedCodes = $found }
}

# Codes this TAS build adds (not in Codes.hmm), and descriptions Codes.hmm leaves out.
$ExtraCodeInfo = @{
    RestoreReadyGoSkip = @{ Name = 'Restore Ready GO Skip'; Category = 'Gameplay'
        Description = 'Brings back the glitch the title update removed: pause in an in-game cutscene and restart, and Sonic can move during Ready... GO!' }
    SkipIntroLogos = @{ Name = 'Boot Directly to Title'; Category = 'System'
        Description = 'Skips the logos at startup and goes straight to the title screen.' }
    EnableStageCollisionDebugView = @{ Name = 'Enable Stage Collision Debug View'; Category = 'Debug'
        Description = "Shows the stage's collision." }
    EnableEventCollisionDebugView = @{ Name = 'Enable Event Collision Debug View'; Category = 'Debug'
        Description = 'Shows event collision (trigger volumes).' }
    EnableObjectCollisionDebugView = @{ Name = 'Enable Object Collision Debug View'; Category = 'Debug'
        Description = "Shows objects' collision." }
    EnableGIMipLevelDebugView = @{ Name = 'Enable GI Mip Level Debug View'; Category = 'Debug'
        Description = 'Colours the world by the mip level of its global illumination textures.' }
}
# Name, category and description per code, from Hedge Mod Manager's Codes.hmm in the mods folder.
$CodeInfo = @{}
function Import-CodeInfo {
    $info = @{}
    $codesHmm = if ($ModsFolder) { Join-Path $ModsFolder 'Codes.hmm' } else { '' }
    if ($codesHmm -and (Test-Path -LiteralPath $codesHmm)) {
        $pattern = '(?s)(?:Patch|Code)\s+"([^"]*)"\s+id\s+"(\w+)"\s+in\s+"([^"]*)"(?:\s+by\s+"[^"]*")?(?:\s+does\s+(?:"([^"]*)"|/\*(.*?)\*/))?'
        foreach ($m in [regex]::Matches([IO.File]::ReadAllText($codesHmm), $pattern)) {
            $desc = if ($m.Groups[4].Success) { $m.Groups[4].Value } elseif ($m.Groups[5].Success) { $m.Groups[5].Value } else { '' }
            $desc = [regex]::Replace($desc.Trim(), '(\r?\n\s*){2,}', "`n")
            $info[$m.Groups[2].Value] = @{ Name = $m.Groups[1].Value; Category = $m.Groups[3].Value; Description = $desc }
        }
    }
    foreach ($code in $ExtraCodeInfo.Keys) {
        if (-not $info.ContainsKey($code)) { $info[$code] = $ExtraCodeInfo[$code].Clone() }
        elseif (-not $info[$code].Description) { $info[$code].Description = $ExtraCodeInfo[$code].Description }
    }
    $script:CodeInfo = $info
}
$CollisionViewNote = "Ticked, the view still starts hidden in libTAS without the TAS HUD (libTASGPU, libTASdozen) and shown with it (libTASGPUHUD, libTASdozenHUD). F11 shows or hides it.`n" +
    "Barely changes sync, but it can: a movie recorded with this on desynced by one frame at a stage load when played with it off. " +
    "To be safe, keep it exactly as it was when recording, even when encoding. To hide the views in an encode, press F11 in the movie instead: F11 doesn't affect sync."
$SyncNotes = @{
    EnableStageCollisionDebugView = $CollisionViewNote
    EnableEventCollisionDebugView = $CollisionViewNote
    EnableObjectCollisionDebugView = $CollisionViewNote
}
$CategoryOrder = @('Gameplay', 'Fixes', 'UI', 'Audio', 'Debug', 'System', 'Other')

# ---- State --------------------------------------------------------------------------------------
$State = @{
    Enabled = New-Object 'System.Collections.Generic.List[string]'
    Codes = New-Object 'System.Collections.Generic.HashSet[string]'
    SavedSignature = ''
    SetupSignatures = @{}
    Updating = $false
    PendingToggle = ''
    LastGameCheck = [datetime]::MinValue
    DragStart = $null
    DragPath = $null
    DropMarker = $null
    Config = $null
}

function Get-Signature($enabled, $codes) {
    return (($enabled -join "`n") + "`n--`n" + ((@($codes) | Sort-Object) -join "`n"))
}

function Get-ModId([string]$linuxPath, $used) {
    $id = $null
    if ($Ids.ContainsKey($linuxPath)) { $id = $Ids[$linuxPath] }
    if (-not $id -or $used.Contains($id)) {
        $id = [guid]::NewGuid().ToString()
        $Ids[$linuxPath] = $id
    }
    [void]$used.Add($id)
    return $id
}

function Get-ModsDbText {
    $used = New-Object 'System.Collections.Generic.HashSet[string]'
    $idFor = @{}
    $order = New-Object 'System.Collections.Generic.List[string]'
    foreach ($p in $State.Enabled) { $order.Add($p) }
    foreach ($p in ($Mods.Keys | Sort-Object)) { if (-not $order.Contains($p)) { $order.Add($p) } }
    foreach ($p in $order) { $idFor[$p] = Get-ModId $p $used }

    $sb = New-Object System.Text.StringBuilder
    [void]$sb.Append("; Written by TAS Mods and Codes. Movies don't store mods or codes: play them back with the same setup.`n")
    [void]$sb.Append("[Main]`n")
    for ($i = 0; $i -lt $State.Enabled.Count; $i++) {
        [void]$sb.Append("ActiveMod$i=`"$($idFor[$State.Enabled[$i]])`"`n")
    }
    [void]$sb.Append("ActiveModCount=$($State.Enabled.Count)`n`n[Mods]`n")
    foreach ($p in $order) { [void]$sb.Append("$($idFor[$p])=`"$p`"`n") }
    [void]$sb.Append("`n[Codes]`n")
    $codes = @($State.Codes | Sort-Object)
    for ($i = 0; $i -lt $codes.Count; $i++) { [void]$sb.Append("Code$i=`"$($codes[$i])`"`n") }
    [void]$sb.Append("CodeCount=$($codes.Count)`n")
    return $sb.ToString()
}

# ---- Window -------------------------------------------------------------------------------------
$xaml = @'
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Title="TAS Mods and Codes" Width="1150" Height="780" MinWidth="820" MinHeight="520"
        WindowStartupLocation="CenterScreen" FontSize="13" Background="#1B1B1E" Foreground="#E8E8EA">
  <Window.Resources>
THEME
  </Window.Resources>
  <DockPanel Margin="12">
    <Border DockPanel.Dock="Top" x:Name="WarningBanner" Background="#3A3016" BorderBrush="#8A6A1F" CornerRadius="5"
            BorderThickness="1" Padding="10,7" Margin="0,0,0,10" Visibility="Collapsed">
      <TextBlock x:Name="WarningText" TextWrapping="Wrap" Foreground="#F2D58A"/>
    </Border>
    <Grid DockPanel.Dock="Top" Margin="0,0,0,4">
      <Grid.ColumnDefinitions>
        <ColumnDefinition Width="Auto"/>
        <ColumnDefinition Width="260"/>
        <ColumnDefinition Width="Auto"/>
        <ColumnDefinition Width="*"/>
      </Grid.ColumnDefinitions>
      <TextBlock Text="Saved setups:" VerticalAlignment="Center" Margin="0,0,8,6"/>
      <ComboBox Grid.Column="1" x:Name="SetupBox" Margin="0,0,6,6" VerticalContentAlignment="Center"/>
      <StackPanel Grid.Column="2" Orientation="Horizontal">
        <Button x:Name="LoadSetupButton" Content="Load"/>
        <Button x:Name="SaveSetupButton" Content="Save current as..."/>
        <Button x:Name="DeleteSetupButton" Content="Delete"/>
      </StackPanel>
      <TextBlock Grid.Column="3" x:Name="MatchText" VerticalAlignment="Center" Margin="8,0,0,6"
                 Foreground="#74C47A" TextTrimming="CharacterEllipsis"/>
    </Grid>
    <DockPanel DockPanel.Dock="Bottom" Margin="0,10,0,0" LastChildFill="True">
      <StackPanel DockPanel.Dock="Right" Orientation="Horizontal" VerticalAlignment="Bottom">
        <Button x:Name="GraphicsButton" Content="Graphics..."/>
        <Button x:Name="SettingsButton" Content="Settings..." Margin="0,0,18,6"/>
        <Button x:Name="RevertButton" Content="Undo changes"/>
        <Button x:Name="SaveButton" Content="Save" MinWidth="90" Style="{StaticResource PrimaryButton}"/>
        <Button x:Name="CloseButton" Content="Close" MinWidth="70" Margin="0,0,0,6"/>
      </StackPanel>
      <StackPanel VerticalAlignment="Center" Margin="0,0,10,6">
        <TextBlock x:Name="StatusText" TextWrapping="Wrap"/>
        <TextBlock x:Name="FileText" TextWrapping="Wrap" Foreground="#8A8A92" FontSize="12" Margin="0,2,0,0"/>
      </StackPanel>
    </DockPanel>
    <Grid>
      <Grid.ColumnDefinitions>
        <ColumnDefinition Width="1.1*"/>
        <ColumnDefinition Width="12"/>
        <ColumnDefinition Width="1*"/>
      </Grid.ColumnDefinitions>
      <GroupBox Header="Mods">
        <DockPanel>
          <TextBlock DockPanel.Dock="Top" TextWrapping="Wrap" Foreground="#A0A0A8" Margin="0,0,0,8">Ticked mods load from the top down: the higher one wins when two mods change the same file. Drag mods to reorder them (drag one into "Not loaded" to turn it off), or double-click to tick and untick. Mods marked with a gear have options: right-click or Configure (the same options as in Hedge Mod Manager).</TextBlock>
          <WrapPanel DockPanel.Dock="Bottom" Margin="0,8,0,0">
            <Button x:Name="ConfigureButton" Content="Configure..." Style="{StaticResource PrimaryButton}"/>
            <Button x:Name="UpButton" Content="Move up"/>
            <Button x:Name="DownButton" Content="Move down"/>
            <Button x:Name="HmmButton" Content="Copy Hedge Mod Manager's mods"/>
            <Button x:Name="NoneButton" Content="Untick all"/>
            <Button x:Name="FolderButton" Content="Open mods folder"/>
          </WrapPanel>
          <Border DockPanel.Dock="Bottom" BorderBrush="#3A3A41" BorderThickness="1" Padding="9,7" Margin="0,8,0,0"
                  Background="#1F1F23" MinHeight="54" CornerRadius="4">
            <ScrollViewer MaxHeight="150" VerticalScrollBarVisibility="Auto">
              <TextBlock x:Name="ModInfo" TextWrapping="Wrap" Foreground="#C8C8CE"/>
            </ScrollViewer>
          </Border>
          <ListBox x:Name="ModList" HorizontalContentAlignment="Stretch" AllowDrop="True"
                   ScrollViewer.HorizontalScrollBarVisibility="Disabled"/>
        </DockPanel>
      </GroupBox>
      <GroupBox Grid.Column="2" Header="Codes">
        <DockPanel>
          <TextBlock DockPanel.Dock="Top" TextWrapping="Wrap" Foreground="#A0A0A8" Margin="0,0,0,8">These are the Linux build's own codes: Hedge Mod Manager's codes on Windows stay separate.</TextBlock>
          <ScrollViewer VerticalScrollBarVisibility="Auto" HorizontalScrollBarVisibility="Disabled">
            <StackPanel x:Name="CodePanel" Margin="0,0,8,0"/>
          </ScrollViewer>
        </DockPanel>
      </GroupBox>
    </Grid>
  </DockPanel>
</Window>
'@

$window = [Windows.Markup.XamlReader]::Parse($xaml.Replace('THEME', $ThemeStyles))
$window.Add_SourceInitialized({ Set-DarkTitleBar $this })
$ui = @{}
foreach ($name in 'WarningBanner', 'WarningText', 'SetupBox', 'LoadSetupButton', 'SaveSetupButton', 'DeleteSetupButton',
                  'MatchText', 'RevertButton', 'SaveButton', 'CloseButton', 'StatusText', 'FileText', 'UpButton',
                  'DownButton', 'HmmButton', 'NoneButton', 'FolderButton', 'ModInfo', 'ModList', 'CodePanel',
                  'ConfigureButton', 'SettingsButton', 'GraphicsButton') {
    $ui[$name] = $window.FindName($name)
}

$GreyBrush = New-Object System.Windows.Media.SolidColorBrush ([System.Windows.Media.Color]::FromRgb(0x98, 0x98, 0xA2))
$RedBrush = New-Object System.Windows.Media.SolidColorBrush ([System.Windows.Media.Color]::FromRgb(0xF0, 0x6A, 0x6A))
$OrangeBrush = New-Object System.Windows.Media.SolidColorBrush ([System.Windows.Media.Color]::FromRgb(0xE8, 0xA3, 0x3D))

function Set-Status([string]$text) { $ui.StatusText.Text = $text }

function Update-Warnings {
    $lines = @()
    if (Test-GameRunning) {
        $lines += 'The game is running. It reads mods and codes only when it starts: restart it to use what you save here.'
    }
    if ($CpkRedirNote) { $lines += $CpkRedirNote }
    if (-not $ModsFolder) { $lines += "Hedge Mod Manager's mods folder isn't set: press Settings... and pick it." }
    elseif (-not (Test-ModsFolder)) { $lines += "Mods folder not found: $ModsFolder. Press Settings... to change it." }
    $ui.WarningText.Text = $lines -join "`n"
    $ui.WarningBanner.Visibility = if ($lines.Count) { 'Visible' } else { 'Collapsed' }
}

# Title star, Save button and the "matches setup" line follow every change.
function Update-Summary {
    $sig = Get-Signature $State.Enabled $State.Codes
    $dirty = ($sig -ne $State.SavedSignature)
    $window.Title = if ($dirty) { "$AppTitle *" } else { $AppTitle }
    $ui.SaveButton.IsEnabled = $dirty
    $ui.RevertButton.IsEnabled = $dirty
    $same = @($State.SetupSignatures.Keys | Where-Object { $State.SetupSignatures[$_] -eq $sig } | Sort-Object)
    $ui.MatchText.Text = if ($same.Count) { 'Same as saved setup: ' + ($same -join ', ') } else { '' }
}

# ---- Mods list ----------------------------------------------------------------------------------
function Get-SelectedModPath {
    $item = $ui.ModList.SelectedItem
    if ($item -and $item.Tag) { return [string]$item.Tag }
    return $null
}

function Show-ModInfo {
    $path = Get-SelectedModPath
    if (-not $path) {
        $ui.ModInfo.Text = 'Select a mod to see its details.'
        $ui.ConfigureButton.IsEnabled = $false
        return
    }
    $mod = $Mods[$path]
    $ui.ConfigureButton.IsEnabled = [bool]$mod.SchemaPath
    $head = $mod.Title
    if ($mod.Version) { $head += "  (version $($mod.Version))" }
    if ($mod.Author) { $head += "  by $($mod.Author)" }
    $text = $head
    if ($mod.Missing) { $text += "`nmod.ini not found: $(ConvertTo-WindowsPath $path)" }
    elseif ($mod.Description) { $text += "`n" + $mod.Description }
    if ($mod.SchemaPath) { $text += "`n`n" + (Get-OptionSummary $mod) }
    $ui.ModInfo.Text = $text
}

function Switch-Mod([string]$path) {
    if ($State.Enabled.Contains($path)) { [void]$State.Enabled.Remove($path) } else { $State.Enabled.Add($path) }
    Update-ModList $path
}

function New-ModItem($mod, [int]$rank) {
    $item = New-Object System.Windows.Controls.ListBoxItem
    $item.Tag = $mod.Path
    $item.Padding = '4,3,4,3'
    # Room for the drag-and-drop insertion line (top or bottom) without the row moving.
    $item.BorderThickness = '0,1,0,1'
    $item.BorderBrush = [System.Windows.Media.Brushes]::Transparent
    $panel = New-Object System.Windows.Controls.DockPanel
    $check = New-Object System.Windows.Controls.CheckBox
    $check.IsChecked = ($rank -gt 0)
    $check.VerticalAlignment = 'Center'
    $check.Margin = '0,0,8,0'
    $check.Tag = $mod.Path
    $check.Add_Click({
        $State.PendingToggle = [string]$this.Tag
        # Rebuild after the click finishes: the list is about to be cleared.
        [void]$window.Dispatcher.BeginInvoke([action]{ Switch-Mod $State.PendingToggle })
    })
    [void]$panel.Children.Add($check)
    $text = New-Object System.Windows.Controls.TextBlock
    $text.TextTrimming = 'CharacterEllipsis'
    $text.VerticalAlignment = 'Center'
    if ($rank -gt 0) {
        $num = New-Object System.Windows.Documents.Run ("$rank.  ")
        $num.Foreground = $GreyBrush
        $text.Inlines.Add($num)
    }
    $title = New-Object System.Windows.Documents.Run ($mod.Title)
    if ($rank -gt 0) { $title.FontWeight = 'SemiBold' }
    $text.Inlines.Add($title)
    if ($mod.SchemaPath) {
        $gear = New-Object System.Windows.Documents.Run (' ' + [char]0x2699)
        $gear.Foreground = $GreyBrush
        $gear.ToolTip = 'Has options: select it and press Configure.'
        $text.Inlines.Add($gear)
    }
    if ($mod.Author) {
        $by = New-Object System.Windows.Documents.Run ("   by $($mod.Author)")
        $by.Foreground = $GreyBrush
        $text.Inlines.Add($by)
    }
    if ($mod.Missing) {
        $miss = New-Object System.Windows.Documents.Run ('   (mod.ini not found)')
        $miss.Foreground = $RedBrush
        $text.Inlines.Add($miss)
    }
    [void]$panel.Children.Add($text)
    $item.Content = $panel
    $item.Add_MouseDoubleClick({ Switch-Mod ([string]$this.Tag) })
    return $item
}

function New-HeaderItem([string]$text, [string]$uid) {
    $item = New-Object System.Windows.Controls.ListBoxItem
    $item.Uid = $uid
    $item.Focusable = $false
    $item.Padding = '4,8,4,2'
    $item.BorderThickness = '0,1,0,1'
    $item.BorderBrush = [System.Windows.Media.Brushes]::Transparent
    # Headers stay enabled so they work as drop targets, but clicking them selects nothing.
    $item.Add_PreviewMouseLeftButtonDown({ $_.Handled = $true })
    $label = New-Object System.Windows.Controls.TextBlock
    $label.Text = $text
    $label.FontWeight = 'Bold'
    $label.Foreground = $GreyBrush
    $item.Content = $label
    return $item
}

function Update-ModList([string]$selectPath) {
    if (-not $selectPath) { $selectPath = Get-SelectedModPath }
    $State.Updating = $true
    $ui.ModList.Items.Clear()
    $count = $State.Enabled.Count
    [void]$ui.ModList.Items.Add((New-HeaderItem ($(if ($count) { "Loaded ($count)" } else { 'Loaded (none: the game runs without mods)' })) 'loaded'))
    $rank = 1
    foreach ($p in $State.Enabled) { [void]$ui.ModList.Items.Add((New-ModItem $Mods[$p] $rank)); $rank++ }
    [void]$ui.ModList.Items.Add((New-HeaderItem 'Not loaded' 'unloaded'))
    foreach ($mod in ($Mods.Values | Where-Object { -not $State.Enabled.Contains($_.Path) } | Sort-Object Title)) {
        [void]$ui.ModList.Items.Add((New-ModItem $mod 0))
    }
    if ($selectPath) {
        foreach ($item in $ui.ModList.Items) {
            if ($item.Tag -eq $selectPath) { $ui.ModList.SelectedItem = $item; $ui.ModList.ScrollIntoView($item); break }
        }
    }
    $State.Updating = $false
    Show-ModInfo
    Update-Summary
}

# ---- Codes list ---------------------------------------------------------------------------------
function Update-CodeList {
    $ui.CodePanel.Children.Clear()
    $groups = @{}
    $all = New-Object 'System.Collections.Generic.List[string]'
    foreach ($c in $SupportedCodes) { $all.Add($c) }
    foreach ($c in $State.Codes) { if (-not $all.Contains($c)) { $all.Add($c) } }
    foreach ($code in $all) {
        $supported = $SupportedCodes -contains $code
        $cat = 'Other'
        if (-not $supported) { $cat = 'Not in this build' }
        elseif ($CodeInfo.ContainsKey($code) -and $CodeInfo[$code].Category) { $cat = $CodeInfo[$code].Category }
        if (-not $groups.ContainsKey($cat)) { $groups[$cat] = New-Object 'System.Collections.Generic.List[string]' }
        $groups[$cat].Add($code)
    }
    $cats = @($CategoryOrder | Where-Object { $groups.ContainsKey($_) })
    $cats += @($groups.Keys | Where-Object { $CategoryOrder -notcontains $_ -and $_ -ne 'Not in this build' } | Sort-Object)
    if ($groups.ContainsKey('Not in this build')) { $cats += 'Not in this build' }

    $first = $true
    foreach ($cat in $cats) {
        $header = New-Object System.Windows.Controls.TextBlock
        $header.Text = $cat
        $header.FontWeight = 'Bold'
        $header.FontSize = 14
        $header.Margin = $(if ($first) { '0,0,0,4' } else { '0,12,0,4' })
        $first = $false
        [void]$ui.CodePanel.Children.Add($header)
        foreach ($code in ($groups[$cat] | Sort-Object { if ($CodeInfo.ContainsKey($_)) { $CodeInfo[$_].Name } else { $_ } })) {
            $check = New-Object System.Windows.Controls.CheckBox
            $check.Tag = $code
            $check.Margin = '0,3,0,5'
            $check.IsChecked = $State.Codes.Contains($code)
            $panel = New-Object System.Windows.Controls.StackPanel
            $titleText = New-Object System.Windows.Controls.TextBlock
            $titleText.TextWrapping = 'Wrap'
            $name = if ($CodeInfo.ContainsKey($code)) { $CodeInfo[$code].Name } else { $code }
            $nameRun = New-Object System.Windows.Documents.Run ($name)
            $nameRun.FontWeight = 'SemiBold'
            $titleText.Inlines.Add($nameRun)
            if ($name -ne $code) {
                $idRun = New-Object System.Windows.Documents.Run ("   $code")
                $idRun.Foreground = $GreyBrush
                $idRun.FontSize = 11
                $titleText.Inlines.Add($idRun)
            }
            [void]$panel.Children.Add($titleText)
            $desc = ''
            if ($CodeInfo.ContainsKey($code)) { $desc = $CodeInfo[$code].Description }
            if (-not $supported) { $desc = 'This build does not know this code: the game ignores it.' }
            if ($desc) {
                $descText = New-Object System.Windows.Controls.TextBlock
                $descText.Text = $desc
                $descText.TextWrapping = 'Wrap'
                $descText.Foreground = $GreyBrush
                $descText.FontSize = 12
                [void]$panel.Children.Add($descText)
            }
            if ($SyncNotes.ContainsKey($code)) {
                $note = New-Object System.Windows.Controls.TextBlock
                $note.Text = $SyncNotes[$code]
                $note.TextWrapping = 'Wrap'
                $note.Foreground = $OrangeBrush
                $note.FontSize = 12
                [void]$panel.Children.Add($note)
            }
            $check.Content = $panel
            $check.Add_Click({
                $c = [string]$this.Tag
                if ($this.IsChecked) { [void]$State.Codes.Add($c) } else { [void]$State.Codes.Remove($c) }
                Update-Summary
            })
            [void]$ui.CodePanel.Children.Add($check)
        }
    }
}

# ---- Load / save --------------------------------------------------------------------------------
function Set-StateFrom($data) {
    $State.Enabled.Clear()
    foreach ($p in $data.Enabled) { $State.Enabled.Add($p) }
    $State.Codes.Clear()
    foreach ($c in $data.Codes) { [void]$State.Codes.Add($c) }
    Update-ModList
    Update-CodeList
}

function Import-Current {
    $data = Import-ModsDb $ModsDbPath
    $State.SavedSignature = Get-Signature $data.Enabled $data.Codes
    Set-StateFrom $data
}

function Update-SetupList([string]$select) {
    $ui.SetupBox.Items.Clear()
    $State.SetupSignatures = @{}
    if (Test-Path -LiteralPath $SetupsDir) {
        foreach ($f in (Get-ChildItem -LiteralPath $SetupsDir -Filter '*.ini' | Sort-Object Name)) {
            $name = [IO.Path]::GetFileNameWithoutExtension($f.Name)
            [void]$ui.SetupBox.Items.Add($name)
            try {
                $d = Import-ModsDb $f.FullName
                $State.SetupSignatures[$name] = Get-Signature $d.Enabled $d.Codes
            } catch { }
        }
    }
    if ($select -and $ui.SetupBox.Items.Contains($select)) { $ui.SetupBox.SelectedItem = $select }
    elseif ($ui.SetupBox.Items.Count) { $ui.SetupBox.SelectedIndex = 0 }
    $has = ($ui.SetupBox.Items.Count -gt 0)
    $ui.LoadSetupButton.IsEnabled = $has
    $ui.DeleteSetupButton.IsEnabled = $has
    Update-Summary
}

function Save-Current {
    try {
        if (-not (Test-Path -LiteralPath $ConfigDir)) { [void](New-Item -ItemType Directory -Path $ConfigDir) }
        if (-not (Test-Path -LiteralPath $CpkRedirPath)) {
            Write-TextFile $CpkRedirPath "[CPKREDIR]`nEnabled=1`nModsDbIni=`"$ModsDbLinux`"`nEnableSaveFileRedirection=0`nLogType=`"`"`n"
        }
        if (Test-Path -LiteralPath $ModsDbPath) { Copy-Item -LiteralPath $ModsDbPath -Destination "$ModsDbPath.bak" -Force }
        Write-TextFile $ModsDbPath (Get-ModsDbText)
        $State.SavedSignature = Get-Signature $State.Enabled $State.Codes
        Update-Summary
        Update-Warnings
        $msg = "Saved: $($State.Enabled.Count) mod(s), $($State.Codes.Count) code(s)."
        if ($ui.WarningBanner.Visibility -eq 'Visible' -and $ui.WarningText.Text.StartsWith('The game is running')) {
            $msg += ' Restart the game to use them.'
        } else {
            $msg += ' The game uses them the next time it starts.'
        }
        Set-Status $msg
        return $true
    } catch {
        Show-Message "Couldn't save $ModsDbPath`n`n$($_.Exception.Message)" 'Error' | Out-Null
        return $false
    }
}

# ---- Events -------------------------------------------------------------------------------------
$ui.ModList.Add_SelectionChanged({ if (-not $State.Updating) { Show-ModInfo } })

$ui.ModList.Add_KeyDown({
    if ($_.Key -eq 'Space') {
        $p = Get-SelectedModPath
        if ($p) { Switch-Mod $p; $_.Handled = $true }
    }
})

# ---- Drag and drop ------------------------------------------------------------------------------
function Find-Parent($element, [type]$type) {
    while ($element) {
        if ($type.IsInstanceOfType($element)) { return $element }
        if ($element -is [System.Windows.Media.Visual]) { $element = [System.Windows.Media.VisualTreeHelper]::GetParent($element) }
        elseif ($element -is [System.Windows.FrameworkContentElement]) { $element = $element.Parent }
        else { return $null }
    }
    return $null
}

function Clear-DropMarker {
    $m = $State.DropMarker
    if ($m) {
        $m.BorderThickness = '0,1,0,1'
        $m.BorderBrush = [System.Windows.Media.Brushes]::Transparent
        $m.ClearValue([System.Windows.Controls.Control]::BackgroundProperty)
    }
    $State.DropMarker = $null
}

# Where a drop at this point puts the mod: @{ Index = position in the load order, or -1 = not loaded;
# Item = row to mark; Below = line under the row instead of above it }.
function Get-DropTarget($e) {
    $item = Find-Parent $e.OriginalSource ([System.Windows.Controls.ListBoxItem])
    if (-not $item) {
        return @{ Index = -1; Item = $null; Below = $false }
    }
    if ($item.Uid -eq 'loaded') { return @{ Index = 0; Item = $item; Below = $true } }
    if ($item.Uid -eq 'unloaded') { return @{ Index = $State.Enabled.Count; Item = $item; Below = $false } }
    $i = $State.Enabled.IndexOf([string]$item.Tag)
    if ($i -lt 0) { return @{ Index = -1; Item = $item; Below = $false } }
    $below = ($e.GetPosition($item).Y -gt $item.ActualHeight / 2)
    return @{ Index = $(if ($below) { $i + 1 } else { $i }); Item = $item; Below = $below }
}

$DropBrush = New-Object System.Windows.Media.SolidColorBrush ([System.Windows.Media.Color]::FromRgb(0x4A, 0x96, 0xF5))
$UnloadBrush = New-Object System.Windows.Media.SolidColorBrush ([System.Windows.Media.Color]::FromRgb(0x4A, 0x25, 0x28))

$ui.ModList.Add_PreviewMouseLeftButtonDown({
    $State.DragPath = $null
    $src = $_.OriginalSource
    if ((Find-Parent $src ([System.Windows.Controls.Primitives.ToggleButton])) -or
        (Find-Parent $src ([System.Windows.Controls.Primitives.ScrollBar]))) { return }
    $item = Find-Parent $src ([System.Windows.Controls.ListBoxItem])
    if ($item -and $item.Tag) {
        $State.DragPath = [string]$item.Tag
        $State.DragStart = $_.GetPosition($ui.ModList)
    }
})

$ui.ModList.Add_PreviewMouseMove({
    if (-not $State.DragPath -or $_.LeftButton -ne 'Pressed') { return }
    $pos = $_.GetPosition($ui.ModList)
    if ([math]::Abs($pos.Y - $State.DragStart.Y) -lt [System.Windows.SystemParameters]::MinimumVerticalDragDistance -and
        [math]::Abs($pos.X - $State.DragStart.X) -lt [System.Windows.SystemParameters]::MinimumHorizontalDragDistance) { return }
    $path = $State.DragPath
    $State.DragPath = $null
    [void][System.Windows.DragDrop]::DoDragDrop($ui.ModList, $path, [System.Windows.DragDropEffects]::Move)
    Clear-DropMarker
})

$ui.ModList.Add_DragOver({
    $_.Handled = $true
    if (-not $_.Data.GetDataPresent([string])) { $_.Effects = 'None'; return }
    $_.Effects = 'Move'
    $target = Get-DropTarget $_
    Clear-DropMarker
    if ($target.Index -lt 0) {
        # Dropping among the unticked mods: tint the "Not loaded" header.
        foreach ($it in $ui.ModList.Items) { if ($it.Uid -eq 'unloaded') { $it.Background = $UnloadBrush; $State.DropMarker = $it } }
    } elseif ($target.Item) {
        $target.Item.BorderBrush = $DropBrush
        $target.Item.BorderThickness = $(if ($target.Below) { '0,0,0,2' } else { '0,2,0,0' })
        $State.DropMarker = $target.Item
    }
    # Scroll when dragging near the top or bottom edge.
    $y = $_.GetPosition($ui.ModList).Y
    $border = [System.Windows.Media.VisualTreeHelper]::GetChild($ui.ModList, 0)
    $scroll = if ($border -is [System.Windows.Controls.Border]) { $border.Child } else { $null }
    if ($scroll -is [System.Windows.Controls.ScrollViewer]) {
        if ($y -lt 24) { $scroll.LineUp() } elseif ($y -gt $ui.ModList.ActualHeight - 24) { $scroll.LineDown() }
    }
})

$ui.ModList.Add_DragLeave({ Clear-DropMarker })

$ui.ModList.Add_Drop({
    $_.Handled = $true
    Clear-DropMarker
    $path = [string]$_.Data.GetData([string])
    if (-not $path -or -not $Mods.ContainsKey($path)) { return }
    $target = Get-DropTarget $_
    $was = $State.Enabled.IndexOf($path)
    if ($target.Index -lt 0) {
        if ($was -lt 0) { return }
        $State.Enabled.RemoveAt($was)
    } else {
        $index = $target.Index
        if ($was -ge 0) {
            $State.Enabled.RemoveAt($was)
            if ($was -lt $index) { $index-- }
        }
        $State.Enabled.Insert($index, $path)
    }
    Update-ModList $path
})

# ---- Configure (mod options) --------------------------------------------------------------------
$configXaml = @'
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Title="Configure" Width="980" Height="600" MinWidth="700" MinHeight="380"
        WindowStartupLocation="CenterOwner" FontSize="13" Background="#1B1B1E" Foreground="#E8E8EA" ShowInTaskbar="False">
  <Window.Resources>
THEME
  </Window.Resources>
  <DockPanel Margin="12">
    <DockPanel DockPanel.Dock="Bottom" Margin="0,8,0,0">
      <StackPanel DockPanel.Dock="Right" Orientation="Horizontal" VerticalAlignment="Bottom">
        <Button x:Name="OkButton" Content="OK" MinWidth="84" IsDefault="True" Style="{StaticResource PrimaryButton}"/>
        <Button x:Name="CancelButton" Content="Cancel" MinWidth="84" IsCancel="True" Margin="0,0,0,6"/>
      </StackPanel>
      <TextBlock x:Name="NoteText" TextWrapping="Wrap" Foreground="#A0A0A8" VerticalAlignment="Center" Margin="0,0,12,6"/>
    </DockPanel>
    <Grid>
      <Grid.ColumnDefinitions>
        <ColumnDefinition Width="*"/>
        <ColumnDefinition Width="12"/>
        <ColumnDefinition Width="300"/>
      </Grid.ColumnDefinitions>
      <ScrollViewer VerticalScrollBarVisibility="Auto" HorizontalScrollBarVisibility="Disabled">
        <StackPanel x:Name="GroupPanel" Margin="0,0,6,0"/>
      </ScrollViewer>
      <GroupBox Grid.Column="2" Header="Description" Margin="0">
        <ScrollViewer VerticalScrollBarVisibility="Auto">
          <StackPanel>
            <TextBlock x:Name="DescText" TextWrapping="Wrap" Foreground="#D4D4D8"/>
            <TextBlock x:Name="DescWarning" TextWrapping="Wrap" Foreground="#E8A33D" Margin="0,10,0,0"/>
          </StackPanel>
        </ScrollViewer>
      </GroupBox>
    </Grid>
  </DockPanel>
</Window>
'@

function Show-OptionDescription($combo) {
    $c = $State.Config
    $e = $combo.Tag
    $text = $e.Label
    if ($e.Description) { $text += "`n`n" + $e.Description }
    $sel = $combo.SelectedItem
    if ($sel -and $sel.Tag -and $sel.Tag.Description -and $sel.Tag.Description -ne $e.Description) {
        $text += "`n`n" + $sel.Content + ":`n" + $sel.Tag.Description
    }
    $c.Desc.Text = $text
    $c.Warning.Text = if ($e.Ignored) {
        "The game doesn't load this option: the mod's mod.ini says IncludeDirCount=$($e.IncludeDirCount), so only IncludeDir0 to IncludeDir$($e.IncludeDirCount - 1) are used. Hedge Mod Manager has the same limit."
    } else { '' }
}

function Show-ConfigureDialog([string]$path) {
    $mod = $Mods[$path]
    try { $options = Get-ModOptions $mod } catch {
        Show-Message "Couldn't read the options of $($mod.Title):`n$($_.Exception.Message)" 'Error' | Out-Null
        return
    }
    if (-not $options -or -not $options.Groups.Count) { Show-Message "$($mod.Title) has no options." | Out-Null; return }

    $dlg = [Windows.Markup.XamlReader]::Parse($configXaml.Replace('THEME', $ThemeStyles))
    $dlg.Add_SourceInitialized({ Set-DarkTitleBar $this })
    $dlg.Owner = $window
    $dlg.Title = "Configure $($mod.Title)"
    $State.Config = @{
        Dialog = $dlg; Desc = $dlg.FindName('DescText'); Warning = $dlg.FindName('DescWarning')
        Combos = New-Object 'System.Collections.Generic.List[object]'
    }
    $dlg.FindName('NoteText').Text = "Saved into the mod's own folder, so Hedge Mod Manager on Windows shows the same options. " +
        'Options change what the mod loads: like mods, record and play back with the same ones.'
    $panel = $dlg.FindName('GroupPanel')

    foreach ($g in $options.Groups) {
        $box = New-Object System.Windows.Controls.GroupBox
        $box.Header = $g.Title
        $box.Margin = '0,0,0,10'
        $grid = New-Object System.Windows.Controls.Grid
        $col1 = New-Object System.Windows.Controls.ColumnDefinition
        $col1.Width = '*'
        $col2 = New-Object System.Windows.Controls.ColumnDefinition
        $col2.Width = '320'
        $grid.ColumnDefinitions.Add($col1)
        $grid.ColumnDefinitions.Add($col2)
        $row = 0
        foreach ($e in $g.Elements) {
            $rd = New-Object System.Windows.Controls.RowDefinition
            $rd.Height = 'Auto'
            $grid.RowDefinitions.Add($rd)
            $label = New-Object System.Windows.Controls.TextBlock
            $label.Text = $e.Label
            $label.TextWrapping = 'Wrap'
            $label.VerticalAlignment = 'Center'
            $label.Margin = '0,4,10,4'
            if ($e.Ignored) { $label.Foreground = $GreyBrush; $label.Text += '  (not loaded)' }
            [System.Windows.Controls.Grid]::SetRow($label, $row)
            [void]$grid.Children.Add($label)

            $combo = New-Object System.Windows.Controls.ComboBox
            $combo.Margin = '0,3,0,3'
            $combo.Tag = $e
            $choices = New-Object 'System.Collections.Generic.List[object]'
            foreach ($c in $e.Choices) { $choices.Add($c) }
            if ($e.Type -ieq 'bool' -and $choices.Count -eq 0) {
                $choices.Add(@{ Name = 'True'; Value = 'true'; Description = '' })
                $choices.Add(@{ Name = 'False'; Value = 'false'; Description = '' })
            }
            $selected = $null
            foreach ($c in $choices) {
                $ci = New-Object System.Windows.Controls.ComboBoxItem
                $ci.Content = $c.Name
                $ci.Tag = $c
                [void]$combo.Items.Add($ci)
                if (-not $selected -and (ConvertTo-ComparableValue $c.Value) -eq (ConvertTo-ComparableValue $e.Value)) { $selected = $ci }
            }
            if (-not $selected) {
                # A value the schema doesn't list (or a free-form option): keep it as it is.
                $ci = New-Object System.Windows.Controls.ComboBoxItem
                $ci.Content = "Current: " + $(if ($e.Value -eq '') { '(empty)' } else { $e.Value })
                $ci.Tag = @{ Name = $ci.Content; Value = $e.Value; Description = '' }
                [void]$combo.Items.Add($ci)
                $selected = $ci
            }
            $combo.SelectedItem = $selected
            $combo.Add_SelectionChanged({ Show-OptionDescription $this })
            $combo.Add_GotFocus({ Show-OptionDescription $this })
            $combo.Add_MouseEnter({ Show-OptionDescription $this })
            [System.Windows.Controls.Grid]::SetRow($combo, $row)
            [System.Windows.Controls.Grid]::SetColumn($combo, 1)
            [void]$grid.Children.Add($combo)
            $State.Config.Combos.Add($combo)
            $row++
        }
        $box.Content = $grid
        [void]$panel.Children.Add($box)
    }
    if ($State.Config.Combos.Count) { Show-OptionDescription $State.Config.Combos[0] }

    $dlg.FindName('OkButton').Add_Click({
        $changes = @()
        foreach ($combo in $State.Config.Combos) {
            $e = $combo.Tag
            $value = [string]$combo.SelectedItem.Tag.Value
            if ($value -ne $e.Value -or -not $e.Stored) {
                $changes += @{ Section = $e.Section; Key = $e.Key; Value = $value }
            }
        }
        try {
            if ($changes.Count) { Set-IniValues $State.Config.IniPath $changes }
            $State.Config.Saved = $changes.Count
            $State.Config.Dialog.DialogResult = $true
        } catch {
            Show-Message "Couldn't save the options to $($State.Config.IniPath):`n$($_.Exception.Message)" 'Error' | Out-Null
        }
    })
    $State.Config.IniPath = $options.IniPath
    $State.Config.Saved = 0
    if ($dlg.ShowDialog()) {
        Show-ModInfo
        Set-Status $(if ($State.Config.Saved) { "Saved $($State.Config.Saved) option(s) of $($mod.Title). The game uses them the next time it starts." } else { "No options of $($mod.Title) changed." })
    }
    $State.Config = $null
}

$ui.ConfigureButton.Add_Click({
    $p = Get-SelectedModPath
    if ($p) { Show-ConfigureDialog $p }
})

# ---- Right-click menu on mods -------------------------------------------------------------------
function Move-LoadedMod([string]$path, [int]$index) {
    $i = $State.Enabled.IndexOf($path)
    if ($i -lt 0) { return }
    $index = [math]::Max(0, [math]::Min($index, $State.Enabled.Count - 1))
    if ($index -eq $i) { return }
    $State.Enabled.RemoveAt($i)
    $State.Enabled.Insert($index, $path)
    Update-ModList $path
}

$ModMenu = New-Object System.Windows.Controls.ContextMenu
$ModMenu.Resources.MergedDictionaries.Add((New-Theme))
$MenuItems = @{}
function Add-MenuEntry([string]$key, [string]$header, [scriptblock]$onClick) {
    $mi = New-Object System.Windows.Controls.MenuItem
    $mi.Header = $header
    $mi.Add_Click($onClick)
    [void]$ModMenu.Items.Add($mi)
    $MenuItems[$key] = $mi
}
function Add-MenuSeparator {
    $sep = New-Object System.Windows.Controls.Separator
    $sep.Style = $ModMenu.FindResource('MenuSeparator')
    [void]$ModMenu.Items.Add($sep)
}
Add-MenuEntry 'Configure' 'Configure...' { $p = Get-SelectedModPath; if ($p) { Show-ConfigureDialog $p } }
$MenuItems.Configure.FontWeight = 'SemiBold'
Add-MenuSeparator
Add-MenuEntry 'Toggle' 'Load' { $p = Get-SelectedModPath; if ($p) { Switch-Mod $p } }
Add-MenuEntry 'Top' 'Move to top' { $p = Get-SelectedModPath; if ($p) { Move-LoadedMod $p 0 } }
Add-MenuEntry 'Up' 'Move up' { $p = Get-SelectedModPath; if ($p) { Move-LoadedMod $p ($State.Enabled.IndexOf($p) - 1) } }
Add-MenuEntry 'Down' 'Move down' { $p = Get-SelectedModPath; if ($p) { Move-LoadedMod $p ($State.Enabled.IndexOf($p) + 1) } }
Add-MenuEntry 'Bottom' 'Move to bottom' { $p = Get-SelectedModPath; if ($p) { Move-LoadedMod $p ($State.Enabled.Count - 1) } }
Add-MenuSeparator
Add-MenuEntry 'Folder' 'Open mod folder' {
    $p = Get-SelectedModPath
    if ($p) {
        $dir = Split-Path $Mods[$p].WinPath -Parent
        if (Test-Path -LiteralPath $dir) { Start-Process explorer.exe -ArgumentList "`"$dir`"" }
    }
}
$ui.ModList.ContextMenu = $ModMenu

$ui.ModList.Add_ContextMenuOpening({
    # Only on a mod row: select it and fit the menu to it.
    $item = Find-Parent $_.OriginalSource ([System.Windows.Controls.ListBoxItem])
    if (-not $item -or -not $item.Tag) { $_.Handled = $true; return }
    $ui.ModList.SelectedItem = $item
    $p = [string]$item.Tag
    $mod = $Mods[$p]
    $i = $State.Enabled.IndexOf($p)
    $loaded = ($i -ge 0)
    $MenuItems.Configure.IsEnabled = [bool]$mod.SchemaPath
    $MenuItems.Configure.Header = $(if ($mod.SchemaPath) { 'Configure...' } else { 'Configure... (no options)' })
    $MenuItems.Toggle.Header = $(if ($loaded) { "Don't load" } else { 'Load' })
    $MenuItems.Top.IsEnabled = ($loaded -and $i -gt 0)
    $MenuItems.Up.IsEnabled = ($loaded -and $i -gt 0)
    $MenuItems.Down.IsEnabled = ($loaded -and $i -lt $State.Enabled.Count - 1)
    $MenuItems.Bottom.IsEnabled = ($loaded -and $i -lt $State.Enabled.Count - 1)
    $MenuItems.Folder.IsEnabled = -not $mod.Missing
})

$ui.UpButton.Add_Click({
    $p = Get-SelectedModPath
    $i = if ($p) { $State.Enabled.IndexOf($p) } else { -1 }
    if ($i -gt 0) { $State.Enabled.RemoveAt($i); $State.Enabled.Insert($i - 1, $p); Update-ModList $p }
    elseif ($i -lt 0) { Set-Status 'Select a ticked mod to move it.' }
})

$ui.DownButton.Add_Click({
    $p = Get-SelectedModPath
    $i = if ($p) { $State.Enabled.IndexOf($p) } else { -1 }
    if ($i -ge 0 -and $i -lt $State.Enabled.Count - 1) { $State.Enabled.RemoveAt($i); $State.Enabled.Insert($i + 1, $p); Update-ModList $p }
    elseif ($i -lt 0) { Set-Status 'Select a ticked mod to move it.' }
})

$ui.HmmButton.Add_Click({
    try {
        $list = Get-HmmEnabled
        $State.Enabled.Clear()
        foreach ($p in $list) { $State.Enabled.Add($p) }
        Update-ModList
        Set-Status "Copied Hedge Mod Manager's $($list.Count) mod(s) and their order. Codes were left as they were. Not saved yet."
    } catch {
        Show-Message "Couldn't read Hedge Mod Manager's list:`n$($_.Exception.Message)" 'Error' | Out-Null
    }
})

$ui.NoneButton.Add_Click({ $State.Enabled.Clear(); Update-ModList })

$ui.FolderButton.Add_Click({ if (Test-ModsFolder) { Start-Process explorer.exe -ArgumentList "`"$ModsFolder`"" } })

$ui.LoadSetupButton.Add_Click({
    $name = [string]$ui.SetupBox.SelectedItem
    if (-not $name) { return }
    try {
        Set-StateFrom (Import-ModsDb (Join-Path $SetupsDir "$name.ini"))
        Set-Status "Loaded setup `"$name`". Not saved yet: press Save to use it."
    } catch {
        Show-Message "Couldn't load setup `"$name`":`n$($_.Exception.Message)" 'Error' | Out-Null
    }
})

$ui.SaveSetupButton.Add_Click({
    $default = [string]$ui.SetupBox.SelectedItem
    $name = Show-Input 'Name for this setup (for example the movie it belongs to):' 'Save setup' $default
    if ($null -eq $name) { return }
    foreach ($ch in [IO.Path]::GetInvalidFileNameChars()) { $name = $name.Replace([string]$ch, '') }
    $name = $name.Trim()
    if (-not $name) { return }
    $file = Join-Path $SetupsDir "$name.ini"
    if (Test-Path -LiteralPath $file) {
        if ((Show-Message "Replace the saved setup `"$name`"?" 'Question' 'YesNo') -ne 'Yes') { return }
    }
    try {
        if (-not (Test-Path -LiteralPath $SetupsDir)) { [void](New-Item -ItemType Directory -Path $SetupsDir) }
        Write-TextFile $file (Get-ModsDbText)
        Update-SetupList $name
        Set-Status "Saved setup `"$name`"."
    } catch {
        Show-Message "Couldn't save the setup:`n$($_.Exception.Message)" 'Error' | Out-Null
    }
})

$ui.DeleteSetupButton.Add_Click({
    $name = [string]$ui.SetupBox.SelectedItem
    if (-not $name) { return }
    if ((Show-Message "Delete the saved setup `"$name`"?" 'Question' 'YesNo') -ne 'Yes') { return }
    Remove-Item -LiteralPath (Join-Path $SetupsDir "$name.ini")
    Update-SetupList
    Set-Status "Deleted setup `"$name`"."
})

$ui.SaveButton.Add_Click({ [void](Save-Current) })
$ui.RevertButton.Add_Click({ Import-Current; Set-Status 'Back to what the game will load.' })
$ui.CloseButton.Add_Click({ $window.Close() })

$window.Add_Activated({
    # Re-check whether the game is running when the window comes back to the front (at most every 3 s).
    if (([datetime]::Now - $State.LastGameCheck).TotalSeconds -ge 3) {
        $State.LastGameCheck = [datetime]::Now
        Update-Warnings
    }
})

$window.Add_Closing({
    if ((Get-Signature $State.Enabled $State.Codes) -eq $State.SavedSignature) { return }
    $answer = Show-Message 'Save your changes before closing?' 'Question' 'YesNoCancel'
    if ($answer -eq 'Cancel') { $_.Cancel = $true }
    elseif ($answer -eq 'Yes' -and -not (Save-Current)) { $_.Cancel = $true }
})

# ---- Settings window ----------------------------------------------------------------------------
$settingsXaml = @'
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Title="Settings" Width="760" SizeToContent="Height" ResizeMode="NoResize"
        WindowStartupLocation="CenterOwner" FontSize="13" Background="#1B1B1E" Foreground="#E8E8EA" ShowInTaskbar="False">
  <Window.Resources>
THEME
  </Window.Resources>
  <StackPanel Margin="20,16,20,14">
    <TextBlock Text="WSL distro" FontWeight="SemiBold" FontSize="14"/>
    <TextBlock Foreground="#A0A0A8" TextWrapping="Wrap" Margin="0,2,0,6">The Linux system the game runs in (the one you start libTAS from).</TextBlock>
    <ComboBox x:Name="DistroBox" Width="320" HorizontalAlignment="Left" Margin="0,0,0,16"/>

    <TextBlock Text="Hedge Mod Manager's mods folder" FontWeight="SemiBold" FontSize="14"/>
    <TextBlock Foreground="#A0A0A8" TextWrapping="Wrap" Margin="0,2,0,6">The Windows folder with your mods (one folder per mod, each with a mod.ini). Hedge Mod Manager's mod list and code descriptions are read from it too. Leave empty to use the folder of the mods already in the game's list.</TextBlock>
    <DockPanel Margin="0,0,0,16">
      <Button x:Name="ModsBrowse" DockPanel.Dock="Right" Content="Browse..." Margin="6,0,0,0"/>
      <TextBox x:Name="ModsBox" VerticalContentAlignment="Center"/>
    </DockPanel>

    <TextBlock Text="Linux build folder (optional)" FontWeight="SemiBold" FontSize="14"/>
    <TextBlock Foreground="#A0A0A8" TextWrapping="Wrap" Margin="0,2,0,6">The folder with the Linux UnleashedRecomp executable, as a Linux path (/home/...) or a Windows one. Only needed for a portable install (a portable.txt next to the executable keeps its settings in that folder) or if the build folder has its own cpkredir.ini. Otherwise the game uses ~/.config/UnleashedRecomp.</TextBlock>
    <DockPanel Margin="0,0,0,16">
      <Button x:Name="BuildBrowse" DockPanel.Dock="Right" Content="Browse..." Margin="6,0,0,0"/>
      <TextBox x:Name="BuildBox" VerticalContentAlignment="Center"/>
    </DockPanel>

    <Border Background="#252529" BorderBrush="#36363C" BorderThickness="1" CornerRadius="5" Padding="10,8">
      <TextBlock x:Name="NowText" TextWrapping="Wrap" Foreground="#C8C8CE"/>
    </Border>
    <StackPanel Orientation="Horizontal" HorizontalAlignment="Right" Margin="0,16,0,0">
      <Button x:Name="OkButton" Content="OK" MinWidth="84" IsDefault="True" Style="{StaticResource PrimaryButton}"/>
      <Button x:Name="CancelButton" Content="Cancel" MinWidth="84" IsCancel="True" Margin="0,0,0,6"/>
    </StackPanel>
  </StackPanel>
</Window>
'@

function Get-PathSummary {
    if (-not $WslRoot -or -not $CpkRedirPath) { return "Not connected to WSL: pick a distro that starts." }
    $lines = @("Now: WSL $WslDistroLabel, home $WslHome")
    $lines += "The game's mod list: $ModsDbLinux" + $(if ($PortableNote) { " ($PortableNote)" } else { '' })
    $cpkLinux = ConvertTo-LinuxPath $CpkRedirPath
    $lines += "cpkredir.ini: $cpkLinux" + $(if (Test-Path -LiteralPath $CpkRedirPath) { '' } else { ' (created on the first Save)' })
    $lines += "Mods folder: " + $(if ($ModsFolder) { "$ModsFolder (from $ModsFolderSource)" } else { 'not set' })
    return $lines -join "`n"
}

function Select-Folder([string]$start, [string]$description) {
    Add-Type -AssemblyName System.Windows.Forms
    $fb = New-Object System.Windows.Forms.FolderBrowserDialog
    $fb.Description = $description
    $fb.ShowNewFolderButton = $false
    if ($start -and (Test-Path -LiteralPath $start)) { $fb.SelectedPath = $start }
    if ($fb.ShowDialog() -eq 'OK') { return $fb.SelectedPath }
    return $null
}

# Reloads everything from the current paths (after start-up or a settings change).
function Import-All {
    $Mods.Clear()
    $Ids.Clear()
    Import-CodeInfo
    Find-Mods
    $ui.HmmButton.IsEnabled = (Test-ModsFolder) -and (Test-Path -LiteralPath (Join-Path $ModsFolder 'ModsDB.ini'))
    $ui.FolderButton.IsEnabled = Test-ModsFolder
    $ui.FileText.Text = "Game's mod list (WSL $WslDistroLabel): $ModsDbLinux" + $(if ($PortableNote) { " ($PortableNote)" } else { '' })
    Import-Current
    try { Import-HmmIds } catch { }
    Update-SetupList
    $State.LastGameCheck = [datetime]::MinValue
    Update-Warnings
}

function Show-SettingsDialog {
    $dlg = [Windows.Markup.XamlReader]::Parse($settingsXaml.Replace('THEME', $ThemeStyles))
    $dlg.Add_SourceInitialized({ Set-DarkTitleBar $this })
    if ($window.IsLoaded) { $dlg.Owner = $window } else { $dlg.WindowStartupLocation = 'CenterScreen' }
    $s = @{ Dialog = $dlg; Applied = $false }
    foreach ($n in 'DistroBox', 'ModsBox', 'BuildBox', 'ModsBrowse', 'BuildBrowse', 'NowText', 'OkButton') { $s[$n] = $dlg.FindName($n) }
    $State.SettingsDialog = $s

    $default = Get-DefaultDistro
    $item = New-Object System.Windows.Controls.ComboBoxItem
    $item.Content = $(if ($default) { "Default ($default)" } else { 'Default' })
    $item.Tag = ''
    [void]$s.DistroBox.Items.Add($item)
    $s.DistroBox.SelectedItem = $item
    $names = @(Get-WslDistros)
    if ($Settings.Distro -and $names -notcontains $Settings.Distro) { $names += $Settings.Distro }
    foreach ($n in $names) {
        $item = New-Object System.Windows.Controls.ComboBoxItem
        $item.Content = $n
        $item.Tag = $n
        [void]$s.DistroBox.Items.Add($item)
        if ($n -eq $Settings.Distro) { $s.DistroBox.SelectedItem = $item }
    }
    $s.ModsBox.Text = $Settings.ModsFolder
    $s.BuildBox.Text = $Settings.BuildFolder
    $s.NowText.Text = Get-PathSummary

    $s.ModsBrowse.Add_Click({
        $d = $State.SettingsDialog
        $start = if ($d.ModsBox.Text) { $d.ModsBox.Text } else { $ModsFolder }
        $picked = Select-Folder $start "Hedge Mod Manager's mods folder (one folder per mod)"
        if ($picked) { $d.ModsBox.Text = $picked }
    })
    $s.BuildBrowse.Add_Click({
        $d = $State.SettingsDialog
        $start = if ($d.BuildBox.Text) { ConvertTo-WindowsPath (ConvertTo-LinuxPathAny $d.BuildBox.Text) } else { $WslRoot }
        $picked = Select-Folder $start 'Folder with the Linux UnleashedRecomp executable'
        if ($picked) { $d.BuildBox.Text = ConvertTo-LinuxPathAny $picked }
    })

    $s.OkButton.Add_Click({
        $d = $State.SettingsDialog
        $new = @{
            Distro = [string]$d.DistroBox.SelectedItem.Tag
            ModsFolder = $d.ModsBox.Text.Trim().Trim('"').TrimEnd('\')
            BuildFolder = ConvertTo-LinuxPathAny $d.BuildBox.Text
        }
        if ($new.ModsFolder -and -not (Test-Path -LiteralPath $new.ModsFolder -PathType Container)) {
            Show-Message "The mods folder doesn't exist:`n$($new.ModsFolder)" 'Error' | Out-Null
            return
        }
        # Changing settings reloads everything: offer to save unsaved changes first (to the current list).
        if ($window.IsLoaded -and (Get-Signature $State.Enabled $State.Codes) -ne $State.SavedSignature) {
            $answer = Show-Message 'Save your changes to the mod list before applying the new settings?' 'Question' 'YesNoCancel'
            if ($answer -eq 'Cancel') { return }
            if ($answer -eq 'Yes' -and -not (Save-Current)) { return }
        }
        $old = $Settings.Clone()
        foreach ($k in @($new.Keys)) { $Settings[$k] = $new[$k] }
        $err = Initialize-Paths
        if ($err) {
            foreach ($k in @($old.Keys)) { $Settings[$k] = $old[$k] }
            [void](Initialize-Paths)
            Show-Message $err 'Error' | Out-Null
            return
        }
        try { Save-Settings } catch {
            Show-Message "Couldn't save the settings to $SettingsPath`n$($_.Exception.Message)" 'Error' | Out-Null
        }
        $d.Applied = $true
        $d.Dialog.Close()
    })

    [void]$dlg.ShowDialog()
    $State.SettingsDialog = $null
    return $s.Applied
}

$ui.SettingsButton.Add_Click({
    if (Show-SettingsDialog) {
        Import-All
        Set-Status 'Settings saved. Everything was reloaded from the new places.'
    }
})

# ---- Graphics window (config.toml) --------------------------------------------------------------
# The game's own settings file, in its user folder. Values and ranges from user/config.cpp/config.h.
$graphicsXaml = @'
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Title="Graphics" Width="720" SizeToContent="Height" ResizeMode="NoResize"
        WindowStartupLocation="CenterOwner" FontSize="13" Background="#1B1B1E" Foreground="#E8E8EA" ShowInTaskbar="False">
  <Window.Resources>
THEME
    <Style x:Key="Hint" TargetType="TextBlock">
      <Setter Property="Foreground" Value="#A0A0A8"/>
      <Setter Property="FontSize" Value="12"/>
      <Setter Property="TextWrapping" Value="Wrap"/>
      <Setter Property="Margin" Value="0,3,0,12"/>
    </Style>
    <Style x:Key="Label" TargetType="TextBlock">
      <Setter Property="FontWeight" Value="SemiBold"/>
      <Setter Property="Margin" Value="0,6,12,0"/>
    </Style>
  </Window.Resources>
  <StackPanel Margin="20,16,20,14">
    <Border x:Name="RunningBanner" Background="#3A3016" BorderBrush="#8A6A1F" CornerRadius="5" BorderThickness="1"
            Padding="10,7" Margin="0,0,0,12" Visibility="Collapsed">
      <TextBlock TextWrapping="Wrap" Foreground="#F2D58A">The game is running. It saves its settings when it closes, which would undo changes made here: close it first.</TextBlock>
    </Border>
    <TextBlock Foreground="#A0A0A8" TextWrapping="Wrap" Margin="0,0,0,14">The Linux build's graphics settings (config.toml). They don't change sync: you can TAS with fast settings and encode the same movie with high ones.</TextBlock>
    <Grid>
      <Grid.ColumnDefinitions>
        <ColumnDefinition Width="170"/>
        <ColumnDefinition Width="*"/>
      </Grid.ColumnDefinitions>
      <Grid.RowDefinitions>
        <RowDefinition Height="Auto"/><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/>
        <RowDefinition Height="Auto"/><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/>
        <RowDefinition Height="Auto"/>
      </Grid.RowDefinitions>

      <TextBlock Grid.Row="0" Style="{StaticResource Label}" Text="Resolution"/>
      <StackPanel Grid.Row="0" Grid.Column="1">
        <StackPanel Orientation="Horizontal">
          <ComboBox x:Name="ResBox" Width="210"/>
          <TextBox x:Name="WidthBox" Width="70" Margin="12,0,0,0" VerticalContentAlignment="Center" HorizontalContentAlignment="Center"/>
          <TextBlock Text="x" VerticalAlignment="Center" Margin="8,0" Foreground="#A0A0A8"/>
          <TextBox x:Name="HeightBox" Width="70" VerticalContentAlignment="Center" HorizontalContentAlignment="Center"/>
        </StackPanel>
        <TextBlock Style="{StaticResource Hint}">The game window's size (16:9 presets).</TextBlock>
      </StackPanel>

      <TextBlock Grid.Row="1" Style="{StaticResource Label}" Text="Resolution scale"/>
      <StackPanel Grid.Row="1" Grid.Column="1">
        <StackPanel Orientation="Horizontal">
          <ComboBox x:Name="ScaleBox" Width="210"/>
          <TextBlock x:Name="RenderText" VerticalAlignment="Center" Margin="12,0,0,0" Foreground="#C8C8CE"/>
        </StackPanel>
        <TextBlock Style="{StaticResource Hint}">Renders the 3D at this times the window size (2 = supersampling, below 1 = faster).</TextBlock>
      </StackPanel>

      <TextBlock Grid.Row="2" Style="{StaticResource Label}" Text="FPS"/>
      <StackPanel Grid.Row="2" Grid.Column="1">
        <ComboBox x:Name="FpsBox" Width="210" HorizontalAlignment="Left"/>
        <TextBlock Style="{StaticResource Hint}">For playing without libTAS. In TAS mode the game runs at libTAS's frame rate whatever this says.</TextBlock>
      </StackPanel>

      <TextBlock Grid.Row="3" Style="{StaticResource Label}" Text="Anti-aliasing"/>
      <StackPanel Grid.Row="3" Grid.Column="1">
        <ComboBox x:Name="AaBox" Width="210" HorizontalAlignment="Left"/>
        <TextBlock Style="{StaticResource Hint}">MSAA. Costly on lavapipe (software rendering).</TextBlock>
      </StackPanel>

      <TextBlock Grid.Row="4" Style="{StaticResource Label}" Text="Shadow resolution"/>
      <StackPanel Grid.Row="4" Grid.Column="1">
        <ComboBox x:Name="ShadowBox" Width="210" HorizontalAlignment="Left"/>
        <TextBlock Style="{StaticResource Hint}">Resolution of real-time shadows. Original: the game picks it itself.</TextBlock>
      </StackPanel>

      <TextBlock Grid.Row="5" Style="{StaticResource Label}" Text="GI texture filtering"/>
      <StackPanel Grid.Row="5" Grid.Column="1">
        <ComboBox x:Name="GiBox" Width="210" HorizontalAlignment="Left"/>
        <TextBlock Style="{StaticResource Hint}">Filtering of the global illumination (baked lighting) textures. Bicubic is smoother.</TextBlock>
      </StackPanel>

      <TextBlock Grid.Row="6" Style="{StaticResource Label}" Text="Motion blur"/>
      <StackPanel Grid.Row="6" Grid.Column="1">
        <ComboBox x:Name="BlurBox" Width="210" HorizontalAlignment="Left"/>
        <TextBlock Style="{StaticResource Hint}">Enhanced adds a full-screen pass every frame (slower on lavapipe).</TextBlock>
      </StackPanel>
    </Grid>
    <TextBlock x:Name="FileNote" Foreground="#8A8A92" FontSize="12" TextWrapping="Wrap" Margin="0,2,0,0"/>
    <StackPanel Orientation="Horizontal" HorizontalAlignment="Right" Margin="0,14,0,0">
      <Button x:Name="OkButton" Content="Save" MinWidth="84" IsDefault="True" Style="{StaticResource PrimaryButton}"/>
      <Button x:Name="CancelButton" Content="Cancel" MinWidth="84" IsCancel="True" Margin="0,0,0,6"/>
    </StackPanel>
  </StackPanel>
</Window>
'@

$Invariant = [Globalization.CultureInfo]::InvariantCulture
$ResolutionPresets = @(
    @{ Label = '480p (854 x 480)'; W = 854; H = 480 },
    @{ Label = '720p (1280 x 720)'; W = 1280; H = 720 },
    @{ Label = '1080p (1920 x 1080)'; W = 1920; H = 1080 },
    @{ Label = '1440p (2560 x 1440)'; W = 2560; H = 1440 },
    @{ Label = '4K (3840 x 2160)'; W = 3840; H = 2160 },
    @{ Label = '8K (7680 x 4320)'; W = 7680; H = 4320 }
)

# Fills a combo with @{ Label; Value } choices and selects the one matching $current
# (adding "Current: x" when the file holds something else, so saving never changes it by accident).
function Set-ComboChoices($combo, $choices, [string]$current) {
    $combo.Items.Clear()
    $selected = $null
    foreach ($c in $choices) {
        $item = New-Object System.Windows.Controls.ComboBoxItem
        $item.Content = $c.Label
        $item.Tag = [string]$c.Value
        [void]$combo.Items.Add($item)
        if (-not $selected -and [string]$c.Value -eq $current) { $selected = $item }
    }
    if (-not $selected -and $current -ne '') {
        $item = New-Object System.Windows.Controls.ComboBoxItem
        $item.Content = "Current: $current"
        $item.Tag = $current
        [void]$combo.Items.Add($item)
        $selected = $item
    }
    if ($selected) { $combo.SelectedItem = $selected } elseif ($combo.Items.Count) { $combo.SelectedIndex = 0 }
}

function ConvertTo-Double([string]$text) {
    $d = 0.0
    if ([double]::TryParse($text, [Globalization.NumberStyles]::Float, $Invariant, [ref]$d)) { return $d }
    return $null
}

function Update-GraphicsResolution {
    $g = $State.Graphics
    if ($g.Updating) { return }
    $g.Updating = $true
    $w = 0; $h = 0
    $ok = [int]::TryParse($g.WidthBox.Text.Trim(), [ref]$w) -and [int]::TryParse($g.HeightBox.Text.Trim(), [ref]$h)
    $match = $null
    if ($ok) { foreach ($item in $g.ResBox.Items) { if ($item.Tag -eq "${w}x${h}") { $match = $item } } }
    $g.ResBox.SelectedItem = $(if ($match) { $match } else { $g.CustomItem })
    $scale = ConvertTo-Double ([string]$g.ScaleBox.SelectedItem.Tag)
    $g.RenderText.Text = if ($ok -and $scale) { "renders at {0} x {1}" -f [int][math]::Round($w * $scale), [int][math]::Round($h * $scale) } else { '' }
    $g.Updating = $false
}

function Show-GraphicsDialog {
    $tomlPath = Join-Path $ConfigDir 'config.toml'
    $ini = if (Test-Path -LiteralPath $tomlPath) { Read-Ini $tomlPath } else { $null }
    $cur = @{}
    foreach ($k in 'WindowWidth', 'WindowHeight', 'ResolutionScale', 'FPS', 'AntiAliasing', 'ShadowResolution', 'GITextureFiltering', 'MotionBlur') {
        $cur[$k] = if ($ini) { [string](Get-IniValue $ini 'Video' $k) } else { '' }
    }
    # The game's defaults for anything the file doesn't have yet.
    $defaults = @{ WindowWidth = '1280'; WindowHeight = '720'; ResolutionScale = '1'; FPS = '60'; AntiAliasing = '4x MSAA'
                   ShadowResolution = '4096'; GITextureFiltering = 'Bicubic'; MotionBlur = 'Original' }
    foreach ($k in @($defaults.Keys)) { if ($cur[$k] -eq '') { $cur[$k] = $defaults[$k] } }

    $dlg = [Windows.Markup.XamlReader]::Parse($graphicsXaml.Replace('THEME', $ThemeStyles))
    $dlg.Add_SourceInitialized({ Set-DarkTitleBar $this })
    $dlg.Owner = $window
    $g = @{ Dialog = $dlg; Path = $tomlPath; Current = $cur; Saved = $false; Updating = $false }
    foreach ($n in 'RunningBanner', 'ResBox', 'WidthBox', 'HeightBox', 'ScaleBox', 'RenderText', 'FpsBox', 'AaBox',
                   'ShadowBox', 'GiBox', 'BlurBox', 'FileNote', 'OkButton') { $g[$n] = $dlg.FindName($n) }
    $State.Graphics = $g

    if (Test-GameRunning) { $g.RunningBanner.Visibility = 'Visible' }
    $g.FileNote.Text = "File: $ConfigDirLinux/config.toml" +
        $(if ($ini) { ' (the previous version is kept as config.toml.bak)' } else { " (doesn't exist yet: it will be created)" })

    $g.Updating = $true
    foreach ($p in $ResolutionPresets) {
        $item = New-Object System.Windows.Controls.ComboBoxItem
        $item.Content = $p.Label
        $item.Tag = "$($p.W)x$($p.H)"
        [void]$g.ResBox.Items.Add($item)
    }
    $g.CustomItem = New-Object System.Windows.Controls.ComboBoxItem
    $g.CustomItem.Content = 'Custom'
    $g.CustomItem.Tag = ''
    [void]$g.ResBox.Items.Add($g.CustomItem)
    $g.WidthBox.Text = $cur.WindowWidth
    $g.HeightBox.Text = $cur.WindowHeight

    $curScale = ConvertTo-Double $cur.ResolutionScale
    $scaleChoices = foreach ($s in 0.25, 0.5, 0.75, 1, 1.25, 1.5, 1.75, 2) {
        @{ Label = $(if ($s -eq 1) { '1 (native)' } else { [string]::Format($Invariant, '{0}', $s) }); Value = [string]::Format($Invariant, '{0}', $s) }
    }
    $scaleCurrent = if ($null -ne $curScale) { [string]::Format($Invariant, '{0}', $curScale) } else { $cur.ResolutionScale }
    Set-ComboChoices $g.ScaleBox $scaleChoices $scaleCurrent

    $fpsChoices = @(foreach ($f in 30, 60, 120, 144, 165, 240) { @{ Label = "$f"; Value = "$f" } }) + @(@{ Label = 'Unlimited'; Value = '241' })
    Set-ComboChoices $g.FpsBox $fpsChoices $cur.FPS
    Set-ComboChoices $g.AaBox @(@{ Label = 'None'; Value = 'None' }, @{ Label = '2x MSAA'; Value = '2x MSAA' },
        @{ Label = '4x MSAA'; Value = '4x MSAA' }, @{ Label = '8x MSAA'; Value = '8x MSAA' }) $cur.AntiAliasing
    Set-ComboChoices $g.ShadowBox @(@{ Label = 'Original'; Value = 'Original' }, @{ Label = '512'; Value = '512' },
        @{ Label = '1024'; Value = '1024' }, @{ Label = '2048'; Value = '2048' }, @{ Label = '4096'; Value = '4096' },
        @{ Label = '8192'; Value = '8192' }) $cur.ShadowResolution
    Set-ComboChoices $g.GiBox @(@{ Label = 'Bilinear'; Value = 'Bilinear' }, @{ Label = 'Bicubic'; Value = 'Bicubic' }) $cur.GITextureFiltering
    Set-ComboChoices $g.BlurBox @(@{ Label = 'Off'; Value = 'Off' }, @{ Label = 'Original'; Value = 'Original' },
        @{ Label = 'Enhanced'; Value = 'Enhanced' }) $cur.MotionBlur
    $g.Updating = $false
    Update-GraphicsResolution

    $g.ResBox.Add_SelectionChanged({
        $g = $State.Graphics
        if ($g.Updating) { return }
        $tag = [string]$g.ResBox.SelectedItem.Tag
        if ($tag -match '^(\d+)x(\d+)$') {
            $g.Updating = $true
            $g.WidthBox.Text = $Matches[1]
            $g.HeightBox.Text = $Matches[2]
            $g.Updating = $false
        }
        Update-GraphicsResolution
    })
    $g.WidthBox.Add_TextChanged({ Update-GraphicsResolution })
    $g.HeightBox.Add_TextChanged({ Update-GraphicsResolution })
    $g.ScaleBox.Add_SelectionChanged({ Update-GraphicsResolution })

    $g.OkButton.Add_Click({
        $g = $State.Graphics
        $w = 0; $h = 0
        if (-not [int]::TryParse($g.WidthBox.Text.Trim(), [ref]$w) -or -not [int]::TryParse($g.HeightBox.Text.Trim(), [ref]$h) -or
            $w -lt 320 -or $h -lt 240 -or $w -gt 16384 -or $h -gt 16384) {
            Show-Message 'The resolution must be two whole numbers, from 320 x 240 up to 16384 x 16384.' 'Error' | Out-Null
            return
        }
        if (Test-GameRunning) {
            $g.RunningBanner.Visibility = 'Visible'
            Show-Message "The game is running. It saves its settings when it closes, which would undo these changes.`n`nClose the game, then press Save again." 'Error' | Out-Null
            return
        }
        $cur = $g.Current
        $values = [ordered]@{}
        if ("$w" -ne $cur.WindowWidth) { $values.WindowWidth = "$w" }
        if ("$h" -ne $cur.WindowHeight) { $values.WindowHeight = "$h" }
        $scale = [string]$g.ScaleBox.SelectedItem.Tag
        if ((ConvertTo-Double $scale) -ne (ConvertTo-Double $cur.ResolutionScale)) { $values.ResolutionScale = $scale }
        $fps = [string]$g.FpsBox.SelectedItem.Tag
        if ($fps -ne $cur.FPS) { $values.FPS = $fps }
        foreach ($pair in @(@('AntiAliasing', $g.AaBox), @('ShadowResolution', $g.ShadowBox),
                            @('GITextureFiltering', $g.GiBox), @('MotionBlur', $g.BlurBox))) {
            $v = [string]$pair[1].SelectedItem.Tag
            if ($v -ne $cur[$pair[0]]) { $values[$pair[0]] = "`"$v`"" }
        }
        try {
            if ($values.Count) {
                if (-not (Test-Path -LiteralPath $ConfigDir)) { [void](New-Item -ItemType Directory -Path $ConfigDir) }
                if (Test-Path -LiteralPath $g.Path) { Copy-Item -LiteralPath $g.Path -Destination "$($g.Path).bak" -Force }
                Set-TomlValues $g.Path 'Video' $values
            }
            $g.Saved = $values.Count
            $g.Dialog.Close()
        } catch {
            Show-Message "Couldn't save $($g.Path)`n`n$($_.Exception.Message)" 'Error' | Out-Null
        }
    })

    [void]$dlg.ShowDialog()
    $State.Graphics = $null
    return $g.Saved
}

$ui.GraphicsButton.Add_Click({
    $n = Show-GraphicsDialog
    if ($n) { Set-Status "Saved $n graphics setting(s). The game uses them the next time it starts." }
})

# ---- Start --------------------------------------------------------------------------------------
Read-Settings
$startError = Initialize-Paths
if ($startError) {
    if ($DryRun) { "Error: $startError"; exit 1 }
    # Let the user pick another distro / build folder; quit if they cancel.
    Show-Message $startError 'Error' | Out-Null
    if (-not (Show-SettingsDialog)) { exit 1 }
}
Import-All
Set-Status "Movies don't store mods or codes: play a movie back with the setup it was recorded with."
if (-not $ModsFolder -and -not $DryRun) {
    # First run without a mods folder: ask for it once the window is up.
    $window.Add_ContentRendered({
        if (-not $ModsFolder -and -not $State.AskedForModsFolder) {
            $State.AskedForModsFolder = $true
            if (Show-SettingsDialog) { Import-All }
        }
    })
}
if ($DryRun) {
    (Get-PathSummary)
    "Mods found: $($Mods.Count); codes known: $($SupportedCodes.Count) (descriptions for $($CodeInfo.Count)); setups: $($ui.SetupBox.Items.Count)"
    "HMM order: " + ((Get-HmmEnabled) -join ' | ')
    "Warning: " + $ui.WarningText.Text
    foreach ($p in $State.Enabled) { if ($Mods[$p].SchemaPath) { "== $($Mods[$p].Title)"; Get-OptionSummary $Mods[$p] } }
    Get-ModsDbText
    exit 0
}
[void]$window.ShowDialog()
