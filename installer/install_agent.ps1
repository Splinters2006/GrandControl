# GrandControl one-click installer + updater.
# Grandparents run install_agent.bat once to install, and again any time
# you want to push an update to their machine.
#
# BEFORE sending: set $RelayHost and $RelayPort below.

$ErrorActionPreference = "Stop"

# EDIT THESE BEFORE SENDING
$RelayHost   = "94.110.194.197"
$RelayPort   = "8443"
$GitHubRepo  = "Splinters2006/GrandControl"
$AssetName   = "GrandControlAgent.exe"

$InstallDir  = Join-Path $env:LOCALAPPDATA "GrandControl"
$AgentDest   = Join-Path $InstallDir "GrandControlAgent.exe"
$ConfigDest  = Join-Path $InstallDir "config.ini"
$TaskName    = "GrandControl Support"

Add-Type -AssemblyName System.Windows.Forms | Out-Null
Add-Type -AssemblyName System.Drawing       | Out-Null

function Show-Info($msg) {
    [System.Windows.Forms.MessageBox]::Show(
        $msg, "GrandControl Setup",
        [System.Windows.Forms.MessageBoxButtons]::OK,
        [System.Windows.Forms.MessageBoxIcon]::Information) | Out-Null
}
function Show-Error($msg) {
    [System.Windows.Forms.MessageBox]::Show(
        $msg, "GrandControl Setup - Error",
        [System.Windows.Forms.MessageBoxButtons]::OK,
        [System.Windows.Forms.MessageBoxIcon]::Error) | Out-Null
}

$form               = New-Object System.Windows.Forms.Form
$form.Text          = "GrandControl Setup"
$form.Size          = New-Object System.Drawing.Size(440, 150)
$form.StartPosition = "CenterScreen"
$form.FormBorderStyle = "FixedDialog"
$form.MaximizeBox   = $false
$form.MinimizeBox   = $false
$form.TopMost       = $true

$label              = New-Object System.Windows.Forms.Label
$label.Location     = New-Object System.Drawing.Point(20, 18)
$label.Size         = New-Object System.Drawing.Size(390, 22)
$label.Text         = "Starting..."
$form.Controls.Add($label)

$bar                = New-Object System.Windows.Forms.ProgressBar
$bar.Location       = New-Object System.Drawing.Point(20, 50)
$bar.Size           = New-Object System.Drawing.Size(390, 22)
$bar.Style          = "Continuous"
$bar.Minimum        = 0
$bar.Maximum        = 100
$form.Controls.Add($bar)

$sub                = New-Object System.Windows.Forms.Label
$sub.Location       = New-Object System.Drawing.Point(20, 82)
$sub.Size           = New-Object System.Drawing.Size(390, 18)
$sub.ForeColor      = [System.Drawing.Color]::Gray
$sub.Font           = New-Object System.Drawing.Font("Segoe UI", 8)
$sub.Text           = ""
$form.Controls.Add($sub)

$form.Show()
$form.Refresh()

function Set-Progress($pct, $msg, $detail = "") {
    $bar.Value  = [Math]::Min($pct, 100)
    $label.Text = $msg
    $sub.Text   = $detail
    $form.Refresh()
}

$isUpdate = (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) -ne $null

if ($isUpdate) {
    $form.Text = "GrandControl Update"
    Set-Progress 0 "Checking for updates..." ""
} else {
    Set-Progress 0 "Preparing installation..." ""
}

try {
    # Step 1 - stop agent if running
    if ($isUpdate) {
        Set-Progress 10 "Stopping current version..." "This only takes a moment."
        Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 800
    } else {
        Set-Progress 10 "Creating install folder..." ""
        New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    }

    # Step 2 - fetch latest from GitHub
    Set-Progress 25 "Checking for latest version on GitHub..." $GitHubRepo
    $downloaded = $false
    try {
        $apiUrl  = "https://api.github.com/repos/$GitHubRepo/releases/latest"
        $headers = @{ "User-Agent" = "GrandControl-Installer" }
        $release = Invoke-RestMethod -Uri $apiUrl -Headers $headers -TimeoutSec 15
        $asset   = $release.assets | Where-Object { $_.name -eq $AssetName } | Select-Object -First 1

        if ($asset) {
            $version = $release.tag_name
            Set-Progress 40 "Downloading version $version..." "From GitHub Releases"
            $wc = New-Object System.Net.WebClient
            $wc.Headers.Add("User-Agent", "GrandControl-Installer")
            $tmpPath = $AgentDest + ".tmp"
            $wc.DownloadFile($asset.browser_download_url, $tmpPath)
            if (Test-Path $AgentDest) { Remove-Item -Force $AgentDest }
            Move-Item -Force $tmpPath $AgentDest
            $downloaded = $true
            Set-Progress 65 "Downloaded version $version." ""
        }
    } catch {
        Set-Progress 40 "Could not reach GitHub." "Trying local copy..."
    }

    # Step 3 - fall back to local exe
    if (-not $downloaded) {
        $localExe = Join-Path $PSScriptRoot $AssetName
        if (Test-Path $localExe) {
            Set-Progress 65 "Using bundled copy of the support tool." ""
            New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
            Copy-Item -Force $localExe $AgentDest
        } elseif (-not (Test-Path $AgentDest)) {
            throw "Could not download $AssetName from GitHub and no local copy was found next to this script."
        } else {
            Set-Progress 65 "Keeping existing installed version." ""
        }
    }

    # Step 4 - write config as base64
    Set-Progress 75 "Writing configuration..." ""
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    $plainConfig = "relay_host=$RelayHost`nrelay_port=$RelayPort`n"
    $encoded = [Convert]::ToBase64String([System.Text.Encoding]::ASCII.GetBytes($plainConfig))
    Set-Content -Encoding ASCII -Path $ConfigDest -Value $encoded

    # Step 5 - register scheduled task
    Set-Progress 85 "Registering startup task..." ""
    $action    = New-ScheduledTaskAction -Execute $AgentDest
    $trigger   = New-ScheduledTaskTrigger -AtLogOn
    $principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel LeastPrivilege
    $settings  = New-ScheduledTaskSettingsSet `
                   -AllowStartIfOnBatteries `
                   -DontStopIfGoingOnBatteries `
                   -RestartCount 99 `
                   -RestartInterval (New-TimeSpan -Minutes 1) `
                   -ExecutionTimeLimit (New-TimeSpan -Hours 0)

    Register-ScheduledTask -TaskName $TaskName `
        -Action $action -Trigger $trigger -Principal $principal -Settings $settings `
        -Description "GrandControl remote support helper." `
        -Force | Out-Null

    # Step 6 - start
    Set-Progress 95 "Starting support tool..." ""
    Start-ScheduledTask -TaskName $TaskName

    Set-Progress 100 "Done!" ""
    $form.Close()

    if ($isUpdate) {
        Show-Info "Update complete!`n`nThe support tool has been updated and is running again."
    } else {
        Show-Info "Setup complete!`n`nThe support tool is now running and will start automatically every time this computer turns on.`n`nYou don't need to do anything else."
    }

} catch {
    $form.Close()
    Show-Error "$(if ($isUpdate) { 'Update' } else { 'Setup' }) failed:`n`n$($_.Exception.Message)`n`nPlease send this message to the person who asked you to run this."
    exit 1
}
