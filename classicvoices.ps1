# Build Classic Voices in your own GitHub account and download the app, on
# Windows. Paste this into PowerShell (Start menu, type PowerShell, Enter):
#
#   irm https://raw.githubusercontent.com/Levi-Gobin/classicvoices/main/classicvoices.ps1 | iex
#
# It signs you in to GitHub (in your browser), makes or updates your copy
# (fork) of Classic Voices, runs the build there, waits for it, and saves
# ClassicVoices.ipa in your Downloads folder. Then it opens Sideloadly if you
# have it, so you can install the app on your iPhone. Running it again later
# updates your copy and builds the newest version.
#
# Everything it prints is plain lines, so it reads well with a screen reader.

& {
    $ErrorActionPreference = 'Stop'
    $ProgressPreference = 'SilentlyContinue'
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

    $Upstream = 'Levi-Gobin/classicvoices'
    $Workflow = 'build.yml'
    $HomeDir = Join-Path $env:LOCALAPPDATA 'ClassicVoices'
    $Out = Join-Path $env:USERPROFILE 'Downloads'

    function Say($text) { Write-Host $text }
    function Fail($text) { throw "Stopped: $text" }

    try {
        # --- GitHub's command line tool, kept in %LOCALAPPDATA%\ClassicVoices ---
        $gh = (Get-Command gh -ErrorAction SilentlyContinue).Source
        if (-not $gh) {
            $gh = Join-Path $HomeDir 'gh\bin\gh.exe'
            if (-not (Test-Path $gh)) {
                Say "Downloading GitHub's command line tool (one time)."
                $arch = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'amd64' }
                $tag = (Invoke-RestMethod https://api.github.com/repos/cli/cli/releases/latest).tag_name.TrimStart('v')
                $zip = Join-Path $env:TEMP 'gh.zip'
                Invoke-WebRequest "https://github.com/cli/cli/releases/download/v$tag/gh_${tag}_windows_$arch.zip" -OutFile $zip -UseBasicParsing
                New-Item -ItemType Directory -Force $HomeDir | Out-Null
                if (Test-Path (Join-Path $HomeDir 'gh')) { Remove-Item -Recurse -Force (Join-Path $HomeDir 'gh') }
                Expand-Archive $zip (Join-Path $HomeDir 'gh')
                Remove-Item $zip
            }
        }

        # Native commands: stop on failure without PowerShell's red error text.
        function GH {
            $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
            $result = $input | & $gh @args 2>$null
            $script:ghOk = ($LASTEXITCODE -eq 0)
            $ErrorActionPreference = $prev
            $result
        }

        # --- Sign in -------------------------------------------------------------
        GH auth status | Out-Null
        if (-not $ghOk) {
            Say 'Signing in to GitHub. Your browser will open, and a one-time code is copied'
            Say 'to the clipboard: paste it on the page, then approve.'
            & $gh auth login --hostname github.com --git-protocol https --web --clipboard --scopes workflow
        }
        $me = GH api user --jq .login
        if (-not $ghOk -or -not $me) { Fail "couldn't sign in to GitHub." }
        Say "Signed in as $me."

        # --- Your copy of Classic Voices ----------------------------------------
        if ($me -ieq $Upstream.Split('/')[0]) {
            $repo = $Upstream
        } else {
            $repo = "$me/classicvoices"
            GH repo view $repo --json name | Out-Null
            if ($ghOk) {
                Say 'Updating your copy of Classic Voices.'
                GH repo sync $repo --source $Upstream | Out-Null
                if (-not $ghOk) { Fail "couldn't update $repo from $Upstream." }
            } else {
                Say 'Making your copy of Classic Voices.'
                GH repo fork $Upstream --clone=false --default-branch-only | Out-Null
                if (-not $ghOk) { Fail "couldn't make your copy." }
                for ($i = 0; $i -lt 30; $i++) {
                    GH repo view $repo --json name | Out-Null
                    if ($ghOk) { break }
                    Start-Sleep 2
                }
            }
        }

        # GitHub turns workflows off in new copies; turn this one on.
        GH api -X PUT "repos/$repo/actions/permissions" -F enabled=true -f allowed_actions=all | Out-Null
        GH workflow enable $Workflow -R $repo | Out-Null

        # The build is zipped with a password only this script knows, because
        # anyone signed in to GitHub can download a public repository's build files.
        $chars = [char[]]'ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789'
        $bytes = New-Object byte[] 24
        [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
        $password = -join ($bytes | ForEach-Object { $chars[$_ % $chars.Length] })
        GH secret set IPA_PASSWORD -R $repo --body $password | Out-Null
        if (-not $ghOk) { Fail "couldn't set the build password on $repo." }

        # --- Build ---------------------------------------------------------------
        $started = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        GH workflow run $Workflow -R $repo | Out-Null
        if (-not $ghOk) { Fail "couldn't start the build. Open https://github.com/$repo/actions, turn workflows on if asked, and run this again." }
        Say "Build started. It takes about half an hour; you'll hear each step as it finishes."
        $run = $null
        for ($i = 0; $i -lt 60 -and -not $run; $i++) {
            $runs = GH run list -R $repo -w $Workflow -e workflow_dispatch -L 5 --json 'databaseId,createdAt'
            if ($ghOk -and $runs) {
                $run = (-join $runs | ConvertFrom-Json) | Where-Object { $_.createdAt -ge $started } |
                    Sort-Object createdAt | Select-Object -Last 1 -ExpandProperty databaseId
            }
            if (-not $run) { Start-Sleep 5 }
        }
        if (-not $run) { Fail "the build didn't appear. See https://github.com/$repo/actions." }

        # One line per finished step, read from GitHub every 30 seconds.
        $names = @{ 'Fetch firmware from Apple' = 'Firmware downloaded from Apple'; 'Generate the fast engines' = 'Engines generated';
                    'Build the app' = 'App built'; 'Run actions/upload-artifact@v4' = 'Upload finished' }
        $seen = @{}
        while ($true) {
            $json = GH run view $run -R $repo --json 'status,conclusion,jobs'
            if ($ghOk -and $json) {
                $view = -join $json | ConvertFrom-Json
                foreach ($step in @($view.jobs | ForEach-Object { $_.steps })) {
                    if ($step.status -eq 'completed' -and $names.ContainsKey($step.name) -and -not $seen.ContainsKey($step.name)) {
                        $line = $names[$step.name]
                        if ($step.conclusion -ne 'success') { $line += " ($($step.conclusion))" }
                        Say "  $line"; $seen[$step.name] = $true
                    }
                }
                if ($view.status -eq 'completed') { $conclusion = $view.conclusion; break }
            }
            Start-Sleep 30
        }
        if ($conclusion -ne 'success') { Fail "the build didn't finish ($conclusion). Details: https://github.com/$repo/actions/runs/$run" }

        # --- Download --------------------------------------------------------------
        Say 'Downloading the app.'
        $tmp = Join-Path $env:TEMP ("ClassicVoices-" + [guid]::NewGuid())
        New-Item -ItemType Directory $tmp | Out-Null
        GH run download $run -R $repo -n ClassicVoices -D $tmp | Out-Null
        if (-not $ghOk) { Fail "couldn't download the build." }
        # Windows' own tar opens password-protected zips.
        Push-Location $tmp
        & tar.exe -xf ClassicVoices-ipa.zip --passphrase $password
        $tarOk = ($LASTEXITCODE -eq 0)
        Pop-Location
        if (-not $tarOk) { Fail "couldn't open the download." }
        $ipa = Join-Path $Out 'ClassicVoices.ipa'
        Move-Item -Force (Join-Path $tmp 'ClassicVoices.ipa') $ipa
        Remove-Item -Recurse -Force $tmp
        Say "Saved $ipa"

        # --- Install ---------------------------------------------------------------
        Set-Clipboard -Value $ipa
        $sideloadly = @("$env:ProgramFiles\Sideloadly\sideloadly.exe", "${env:ProgramFiles(x86)}\Sideloadly\sideloadly.exe",
                        "$env:LOCALAPPDATA\Sideloadly\sideloadly.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
        if (-not $sideloadly) {  # or wherever its Start menu shortcut points
            $sideloadly = @("$env:ProgramData\Microsoft\Windows\Start Menu", "$env:APPDATA\Microsoft\Windows\Start Menu") |
                Where-Object { Test-Path $_ } | ForEach-Object { Get-ChildItem $_ -Recurse -Filter 'Sideloadly*.lnk' -ErrorAction SilentlyContinue } |
                Select-Object -First 1 -ExpandProperty FullName
        }
        if ($sideloadly) {
            Start-Process $sideloadly
            Say 'Sideloadly is open. Connect your iPhone, choose ClassicVoices.ipa from your'
            Say 'Downloads folder (its full path is on the clipboard), and press Start.'
        } else {
            Say 'Install it on your iPhone with Sideloadly (sideloadly.io), AltStore or SideStore:'
            Say 'choose ClassicVoices.ipa from your Downloads folder.'
        }
        Say 'Then open Classic Voices once, and its voices appear in VoiceOver and Spoken Content.'
    } catch {
        Write-Host $_.Exception.Message
    }
}
