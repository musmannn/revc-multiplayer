<#
reVC multiplayer installer for Windows.

Builds reVC from this checkout, then sets up a playable game folder:
  1. copies your GTA Vice City files (found in Downloads by default),
  2. adds reVC's own game files (gamefiles\) on top,
  3. puts reVC.exe, its DLLs, revc-relay.exe and revc-fakeclient.exe in that folder,
  4. adds a "reVC Multiplayer" shortcut to the desktop.
Your original GTA folder is never changed.

Run it with installer\install-windows.bat, or:
  powershell -ExecutionPolicy Bypass -File installer\install-windows.ps1 [-Source DIR] [-Dest DIR] [-Renderer d3d9|gl3] [-SkipBuild]
#>

param(
	# Your GTA Vice City folder (the one with data\ and models\). Default: searched for in Downloads.
	[string]$Source,
	# Where to install.
	[string]$Dest = (Join-Path $env:USERPROFILE "Games\reVC-Multiplayer"),
	# d3d9 (Direct3D 9) suits most PCs; gl3 uses OpenGL.
	[ValidateSet("d3d9", "gl3")][string]$Renderer = "d3d9",
	# Reuse the last build instead of rebuilding.
	[switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"	# Invoke-WebRequest is very slow with the progress bar

$Repo = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Repo "build\windows"
$Platform = if ($Renderer -eq "gl3") { "win-amd64-librw_gl3_glfw-oal" } else { "win-amd64-librw_d3d9-oal" }
$GlfwBase = "glfw-3.4.bin.WIN64"
$GlfwUrl = "https://github.com/glfw/glfw/releases/download/3.4/$GlfwBase.zip"

function Step($msg) { Write-Host ""; Write-Host "==> $msg" -ForegroundColor Cyan }
function Info($msg) { Write-Host "    $msg" }
function Fail($msg) { Write-Host ""; Write-Host "Error: $msg" -ForegroundColor Red; exit 1 }

# Runs a native command and stops if it fails.
function Run($exe, [string[]]$arguments) {
	& $exe @arguments
	if ($LASTEXITCODE -ne 0) { Fail "$(Split-Path -Leaf $exe) failed (exit code $LASTEXITCODE)." }
}

# A folder is a GTA Vice City install if it has these two files.
function Test-GameDir($dir) {
	(Test-Path -LiteralPath (Join-Path $dir "data\gta_vc.dat")) -and
	(Test-Path -LiteralPath (Join-Path $dir "models\gta3.img"))
}

# Copies a folder's contents over another. robocopy exit codes below 8 mean success.
function Copy-Tree($from, $to) {
	robocopy $from $to /E /R:1 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
	if ($LASTEXITCODE -ge 8) { Fail "copying $from to $to failed (robocopy exit code $LASTEXITCODE)." }
	$global:LASTEXITCODE = 0
}

# --- Find the game files --------------------------------------------------

Step "Looking for your GTA Vice City files"
if (-not $Source) {
	$downloads = Join-Path $env:USERPROFILE "Downloads"
	try {
		$downloads = (New-Object -ComObject Shell.Application).NameSpace("shell:Downloads").Self.Path
	} catch {}

	$candidates = @(Get-ChildItem -LiteralPath $downloads -Filter "gta_vc.dat" -Recurse -Depth 5 -File -ErrorAction SilentlyContinue |
		Where-Object { $_.Directory.Name -eq "data" } |
		ForEach-Object { $_.Directory.Parent.FullName } |
		Where-Object { Test-GameDir $_ })

	if ($candidates.Count -eq 0) {
		Fail "no GTA Vice City folder found in $downloads.
Put your game folder (the one containing data\ and models\) in Downloads,
or point at it with: install-windows.bat -Source ""C:\path\to\GTA Vice City"""
	}
	$Source = $candidates[0]
	if ($candidates.Count -gt 1) {
		Info "Found more than one game folder:"
		$candidates | ForEach-Object { Info "  $_" }
		Info "Using the first. Pass -Source to pick another."
	}
}
if (-not (Test-Path -LiteralPath $Source)) { Fail "folder not found: $Source" }
$Source = (Resolve-Path -LiteralPath $Source).Path
if (-not (Test-GameDir $Source)) { Fail "$Source doesn't look like GTA Vice City (no data\gta_vc.dat or models\gta3.img)." }
Info "Game files: $Source"
Info "Install to: $Dest"
Info "Renderer:   $Renderer"

# --- Build ----------------------------------------------------------------

$Exe = Join-Path $Repo "bin\$Platform\Release\reVC.exe"
$ToolsDir = Join-Path $BuildDir "mp\Release"

if (-not $SkipBuild) {
	Step "Checking build tools"
	$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
	if (-not (Test-Path $vswhere)) {
		Fail "Visual Studio is needed. Install Visual Studio Community (https://visualstudio.microsoft.com/vs/community/)
with the ""Desktop development with C++"" workload, then run this again."
	}
	$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
	$vsVersion = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion | Select-Object -First 1
	if (-not $vsPath) {
		Fail "Visual Studio is installed without C++. Open the Visual Studio Installer, choose Modify,
tick ""Desktop development with C++"", then run this again."
	}
	$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
	if (-not $msbuild) { Fail "MSBuild.exe not found in $vsPath." }

	$vsMajor = [int]($vsVersion.Split(".")[0])
	$premakeAction = switch ($vsMajor) {
		16 { "vs2019" }
		17 { "vs2022" }
		default { if ($vsMajor -ge 18) { "vs2026" } else { Fail "Visual Studio 2019 or newer is needed (found $vsVersion)." } }
	}
	Info "Visual Studio $vsVersion ($premakeAction)"

	$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
	if (-not $cmake) {
		$cmake = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
		if (-not (Test-Path $cmake)) { Fail "CMake not found. Install it from https://cmake.org/download/ or add ""C++ CMake tools for Windows"" in the Visual Studio Installer." }
	}
	Info "CMake: $cmake"

	Step "Fetching submodules"
	$git = Get-Command git -ErrorAction SilentlyContinue
	if ($git -and (Test-Path (Join-Path $Repo ".git"))) {
		Run $git.Source @("-C", $Repo, "submodule", "update", "--init", "--recursive")
	}
	if (-not (Test-Path (Join-Path $Repo "vendor\librw\CMakeLists.txt"))) {
		Fail "vendor\librw is empty. Install Git (https://git-scm.com) and clone the repo with:
git clone --recursive https://github.com/musmannn/revc-multiplayer.git"
	}

	New-Item -ItemType Directory -Force $BuildDir | Out-Null
	$glfwDir = Join-Path $BuildDir $GlfwBase
	if ($Renderer -eq "gl3" -and -not (Test-Path $glfwDir)) {
		Step "Downloading GLFW"
		$zip = Join-Path $BuildDir "$GlfwBase.zip"
		Invoke-WebRequest -Uri $GlfwUrl -OutFile $zip -UseBasicParsing
		Expand-Archive -LiteralPath $zip -DestinationPath $BuildDir -Force
	}
	# premake links glfw from lib-vcYYYY matching the VS version; newer VS can use the newest one GLFW ships.
	$glfwLib = Join-Path $glfwDir ("lib-" + $premakeAction.Replace("vs", "vc"))
	if ($Renderer -eq "gl3" -and -not (Test-Path $glfwLib)) {
		$newest = Get-ChildItem -LiteralPath $glfwDir -Directory -Filter "lib-vc*" | Sort-Object Name | Select-Object -Last 1
		Copy-Item -Recurse $newest.FullName $glfwLib
	}

	Step "Building reVC (this takes a few minutes)"
	Push-Location $Repo
	try {
		Run (Join-Path $Repo "premake5.exe") @($premakeAction, "--with-librw", "--no-full-paths", "--glfwdir64=$glfwDir")
		Run $msbuild @("-m", "build\reVC.sln", "/p:Configuration=Release", "/p:Platform=$Platform", "/v:minimal")
	} finally {
		Pop-Location
	}

	Step "Building revc-relay and revc-fakeclient"
	$mpBuild = Join-Path $BuildDir "mp"
	Run $cmake @("-S", (Join-Path $Repo "mp"), "-B", $mpBuild)
	Run $cmake @("--build", $mpBuild, "--config", "Release", "--target", "revc-relay", "revc-fakeclient")
}

if (-not (Test-Path $Exe)) { Fail "no build found at $Exe. Run without -SkipBuild." }

# --- Install --------------------------------------------------------------

Step "Copying game files"
New-Item -ItemType Directory -Force $Dest | Out-Null
$Dest = (Resolve-Path -LiteralPath $Dest).Path
if ($Source -eq $Dest) {
	Info "Source and destination are the same folder; installing in place."
} elseif (Test-GameDir $Dest) {
	Info "Already there, skipping. (Delete $Dest for a fresh copy.)"
} else {
	Info "From $Source (a few GB, give it a minute)"
	Copy-Tree $Source $Dest
}

Step "Adding reVC files"
Copy-Tree (Join-Path $Repo "gamefiles") $Dest
Copy-Item -Force $Exe $Dest
Copy-Item -Force (Join-Path $Repo "vendor\mpg123\dist\Win64\libmpg123-0.dll") $Dest
Copy-Item -Force (Join-Path $Repo "vendor\openal-soft\dist\Win64\OpenAL32.dll") $Dest
foreach ($tool in "revc-relay.exe", "revc-fakeclient.exe") {
	$path = Join-Path $ToolsDir $tool
	if (Test-Path $path) { Copy-Item -Force $path $Dest } else { Info "Skipping $tool (not built)." }
}
Copy-Item -Force (Join-Path $Repo "INSTALLATION.md") (Join-Path $Dest "INSTALLATION.md")

$shortcutPath = Join-Path ([Environment]::GetFolderPath("Desktop")) "reVC Multiplayer.lnk"
$shortcut = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcutPath)
$shortcut.TargetPath = Join-Path $Dest "reVC.exe"
$shortcut.WorkingDirectory = $Dest
$shortcut.Save()

Step "Done"
Info "Play:          the ""reVC Multiplayer"" shortcut on your desktop, or $Dest\reVC.exe"
Info "Host a relay:  ""$Dest\revc-relay.exe"" --port 7777"
Info "Setup guide:   $Dest\INSTALLATION.md"
