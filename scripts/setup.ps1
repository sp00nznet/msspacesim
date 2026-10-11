<#
  setup.ps1 - the Quick start (README, Getting Started). Setup.cmd runs this.

  From a plain download to a shortcut in this folder:
    1. check the tools (Python 3 with capstone, CMake, Visual Studio 2022 C++
       build tools, git; ffmpeg is optional) and offer to install what is missing
    2. find your floppy images (original\, this folder, Downloads, or ask)
    3. fetch the pcrecomp toolkit into work\pcrecomp, at the commit this needs
    4. unpack the floppies into game\            (tools\unpack_floppies.py)
    5. lift the game and the install tools to C  (tools\lift.py all)
    6. build                                     (cmake)
    7. generate the star systems                 (tools\generate.py)
    8. write SPACESIM.INI and SETUP.INI          (tools\make_ini.py)
    9. put "Microsoft Space Simulator.lnk" in this folder

  Every step is skipped when its result is already there, so running it again
  after a failure carries on. Details go to setup.log. It runs exactly the
  commands README "Step by step" lists.

  Nothing is downloaded or bundled from the game: the files come from your own
  floppy images, and everything made from them stays in game\, work\ and build\.
#>
param([string]$Images = "", [switch]$Yes, [switch]$S10Data)
$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
Set-Location $Root
$Log = Join-Path $Root "setup.log"
"=== setup $(Get-Date -Format s)" | Out-File $Log -Encoding utf8

# The toolkit revision this game needs: pcrecomp with the changes listed in
# CHANGELOG.md. A commit, not a branch: branches move, this build should not.
$PcrecompUrl = "https://github.com/sp00nznet/pcrecomp.git"
$PcrecompRef = "357136a"   # work/msspacesim-integration: main + #85-#91

function Say($m) { Write-Host $m; $m | Out-File $Log -Append -Encoding utf8 }
function Fail($m) {
    Write-Host ""
    Write-Host $m -ForegroundColor Red
    Write-Host "Details are in $Log"
    exit 1
}
function Run($what, [scriptblock]$cmd) {
    Say "  $what"
    # native tools write progress to stderr; under "Stop" PowerShell 5.1 would
    # turn every such line into a terminating error
    $ErrorActionPreference = "Continue"
    $out = & $cmd 2>&1
    $out | Out-File $Log -Append -Encoding utf8
    if ($LASTEXITCODE -ne 0) { Fail "$what failed." }
}
function Ask($q) {
    if ($Yes) { return $true }
    $a = Read-Host "$q [y/N]"
    return $a -match '^(y|yes)$'
}
function Have($exe) { return [bool](Get-Command $exe -ErrorAction SilentlyContinue) }

# ---- 1. tools ---------------------------------------------------------------
Say "Checking tools..."
$need = @()
# `python` may be the Microsoft Store alias that only opens the Store; the
# py launcher is what a python.org install puts on PATH.
$py = if (Have "py") { "py" } elseif ((Have "python") -and ((& python --version 2>&1) -match "Python 3")) { "python" } else { $null }
if (-not $py) { $need += @{ name = "Python 3"; id = "Python.Python.3.12"; size = "about 30 MB"; why = "runs the lifter (tools\lift.py)" } }
if (-not (Have "cmake")) { $need += @{ name = "CMake"; id = "Kitware.CMake"; size = "about 35 MB"; why = "configures the build" } }
if (-not (Have "git")) { $need += @{ name = "Git"; id = "Git.Git"; size = "about 60 MB"; why = "fetches the pcrecomp toolkit" } }
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = if (Test-Path $vswhere) { & $vswhere -latest -version "[17.0,18.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath } else { $null }
if (-not $vs) { $need += @{ name = "Visual Studio 2022 Build Tools (C++)"; id = "Microsoft.VisualStudio.2022.BuildTools"; size = "about 2 GB"; why = "compiles the lifted C"; args = "--override `"--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended`"" } }

foreach ($n in $need) {
    Say "Missing: $($n.name) -- needed because it $($n.why). Download: $($n.size)."
    if (-not (Have "winget")) { Fail "Install $($n.name) yourself (winget is not available), then run Setup again." }
    if (-not (Ask "Install $($n.name) now with winget?")) { Fail "Setup needs $($n.name). Install it and run Setup again." }
    $wargs = @("install", "--id", $n.id, "-e", "--accept-package-agreements", "--accept-source-agreements")
    if ($n.args) { $wargs += $n.args }
    Run "installing $($n.name)" { winget @wargs }
}
if ($need.Count) {
    # a fresh install is on the machine PATH but not in this window's
    $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [Environment]::GetEnvironmentVariable("Path", "User")
    $py = if (Have "py") { "py" } else { "python" }
}
$pyargs = if ($py -eq "py") { @("-3") } else { @() }
& $py @pyargs -c "import capstone" 2>$null
if ($LASTEXITCODE -ne 0) {
    Say "Missing: the capstone Python package -- the disassembler under the lifter. Download: about 3 MB."
    if (-not (Ask "Install it now with pip?")) { Fail "Setup needs capstone: $py -m pip install capstone" }
    Run "installing capstone" { & $py @pyargs -m pip install --quiet capstone }
}
if (-not (Have "ffmpeg")) { Say "  (ffmpeg not found: everything works except --record. winget install Gyan.FFmpeg adds it.)" }

# ---- 2. the floppy images ---------------------------------------------------------
function Has-Images($dir) {
    if (-not $dir -or -not (Test-Path $dir)) { return $false }
    $ima = @(Get-ChildItem $dir -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '[123]\.ima$' })
    $zip = @(Get-ChildItem $dir -File -Filter *.zip -ErrorAction SilentlyContinue | Where-Object { $_.Name -match 'spacesim' })
    return ($ima.Count -ge 3) -or ($zip.Count -ge 1)
}
$game = Join-Path $Root "game"
if (Test-Path (Join-Path $game "SPACESIM.EXE")) {
    Say "The game is already unpacked in game\."
} else {
    $src = $null
    foreach ($c in @($Images, (Join-Path $Root "original"), $Root, (Join-Path $env:USERPROFILE "Downloads"))) {
        if (Has-Images $c) { $src = $c; break }
    }
    if (-not $src -and -not $Yes) {
        $ans = Read-Host "Where are your Space Simulator floppy images? (the folder with the three .IMA files, or msspacesim.zip)"
        if ($ans -and (Test-Path $ans)) { $src = $ans }
    }
    if (-not $src) { Fail "No floppy images found. Run Setup again with the folder that holds 'Microsoft Space Simulator 1.IMA' .. '3.IMA'." }
    $zip = @(Get-ChildItem $src -File -Filter *.zip -ErrorAction SilentlyContinue | Where-Object { $_.Name -match 'spacesim' })
    $from = if ((-not (Test-Path $src -PathType Leaf)) -and $zip.Count -and -not @(Get-ChildItem $src -Recurse -File | Where-Object { $_.Name -match '[123]\.ima$' }).Count) { $zip[0].FullName } else { $src }
    Run "unpacking the floppy images from $from" { & $py @pyargs tools\unpack_floppies.py $from game }
}

# ---- 3. the toolkit -----------------------------------------------------------------
if ($env:PCRECOMP_HOME -and (Test-Path "$env:PCRECOMP_HOME\tools\lift\lift16.py")) {
    $pcr = $env:PCRECOMP_HOME
} else {
    $pcr = Join-Path $Root "work\pcrecomp"
    if (-not (Test-Path "$pcr\tools\lift\lift16.py")) {
        Run "fetching pcrecomp into work\pcrecomp" { git clone --quiet $PcrecompUrl $pcr }
    }
    Run "checking out pcrecomp $PcrecompRef" { git -C $pcr fetch --quiet origin; git -C $pcr checkout --quiet $PcrecompRef }
}
$env:PCRECOMP_HOME = $pcr

# ---- 4. lift, build, generate, settings -----------------------------------------------
Run "lifting the game and the install tools to C" { & $py @pyargs tools\lift.py all }
Run "configuring the build" { cmake -B build -G "Visual Studio 17 2022" -A x64 "-DPCRECOMP_HOME=$pcr" }
Run "compiling (a few minutes: the lifted C is large)" { cmake --build build --config Release -- -m }
$gen = @("tools\generate.py")
if ($S10Data) { $gen += "--s10data" }
Run "generating the star systems, as the installer did" { & $py @pyargs @gen }
if (-not (Test-Path (Join-Path $game "SPACESIM.INI"))) {
    Run "writing SPACESIM.INI (what the 1994 SETUP program wrote)" { & $py @pyargs tools\make_ini.py }
}
if (-not (Test-Path (Join-Path $game "SETUP.INI"))) {
    # the registered name and product ID the installer stored; shown in About
    "$env:USERNAME`r`n00000-000-0000000`r`n" | Out-File (Join-Path $game "SETUP.INI") -Encoding ascii -NoNewline
}

# ---- 5. shortcut --------------------------------------------------------------------
$exe = Join-Path $Root "build\Release\spacesim.exe"
if (-not (Test-Path $exe)) { Fail "The build finished but $exe is missing." }
$shell = New-Object -ComObject WScript.Shell
$lnk = $shell.CreateShortcut((Join-Path $Root "Microsoft Space Simulator.lnk"))
$lnk.TargetPath = $exe
$lnk.WorkingDirectory = $Root
$lnk.Save()
Say "Done. Double-click 'Microsoft Space Simulator.lnk' to play. Alt+Enter toggles fullscreen."
