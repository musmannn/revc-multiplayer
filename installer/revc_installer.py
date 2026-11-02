#!/usr/bin/env python3
"""reVC multiplayer installer for Windows, Linux and macOS.

Gets the reVC source (downloads it if needed), installs the build tools it can,
builds reVC with multiplayer, then puts everything into your GTA Vice City
folder (or a copy of it):
  - reVC's own game files (gamefiles/), after backing up any file they replace,
  - the reVC executable (and its DLLs on Windows),
  - revc-relay and revc-fakeclient, plus a start-relay script,
  - INSTALLATION.md, and a menu/desktop shortcut.

It can also install just the relay server, for hosting on a server or VPS.

Release downloads (built by .github/workflows/release.yml) carry a prebuilt
reVC in a "payload" folder next to this script, or inside the frozen
executable. Then nothing is compiled: the installer only checks a few system
libraries and copies files. Pass --from-source to build anyway.

Run it with a window (needs Tkinter, which comes with Python on Windows and macOS;
on Linux install python3-tk):
    python3 installer/revc_installer.py
or in a terminal, for servers or SSH sessions:
    python3 installer/revc_installer.py --cli --game "/path/to/GTA Vice City"
    python3 installer/revc_installer.py --cli --relay-only
Run with --help for all options. Only the Python standard library is used.
"""

import argparse
import os
import platform
import queue
import shlex
import shutil
import subprocess
import sys
import threading
import urllib.request
import zipfile
from pathlib import Path

REPO_URL = "https://github.com/musmannn/revc-multiplayer.git"
BRANCH = "main"
GAME_NAME = "Grand Theft Auto Vice City"

SYSTEM = platform.system()
IS_WIN = SYSTEM == "Windows"
IS_MAC = SYSTEM == "Darwin"
IS_LINUX = SYSTEM == "Linux"
EXE = ".exe" if IS_WIN else ""

GLFW_BASE = "glfw-3.4.bin.WIN64"
GLFW_URL = "https://github.com/glfw/glfw/releases/download/3.4/%s.zip" % GLFW_BASE
VS_BUILD_TOOLS = "Microsoft.VisualStudio.2022.BuildTools"

# Linux packages per package manager: the install command, the build tools and
# the libraries the game links against (the relay needs only the tools).
LINUX_PACKAGES = {
	"apt-get": (["apt-get", "install", "-y"],
		["build-essential", "cmake", "git", "pkg-config"],
		["libglfw3-dev", "libopenal-dev", "libmpg123-dev", "libgl-dev", "libx11-dev"]),
	"dnf": (["dnf", "install", "-y"],
		["gcc-c++", "make", "cmake", "git", "pkgconf-pkg-config"],
		["glfw-devel", "openal-soft-devel", "mpg123-devel", "mesa-libGL-devel", "libX11-devel"]),
	"pacman": (["pacman", "-S", "--needed", "--noconfirm"],
		["base-devel", "cmake", "git", "pkgconf"],
		["glfw", "openal", "mpg123", "mesa", "libx11"]),
	"zypper": (["zypper", "--non-interactive", "install"],
		["gcc-c++", "make", "cmake", "git", "pkg-config"],
		["libglfw-devel", "openal-soft-devel", "mpg123-devel", "Mesa-libGL-devel", "libX11-devel"]),
}
LINUX_PKGCONFIG = ["glfw3", "openal", "libmpg123", "gl", "x11"]
MAC_FORMULAS_TOOLS = ["cmake", "git"]
MAC_FORMULAS_LIBS = ["glfw", "openal-soft", "mpg123"]
# What the prebuilt game needs at run time, per package manager.
LINUX_RUNTIME = {
	"apt-get": ["libglfw3", "libopenal1", "libmpg123-0"],
	"dnf": ["glfw", "openal-soft", "mpg123-libs"],
	"pacman": ["glfw", "openal", "mpg123"],
	"zypper": ["libglfw3", "libopenal1", "libmpg123-0"],
}
TOOLS = ["revc-relay", "revc-server", "revc-fakeclient"]


class InstallError(Exception):
	pass


class Cancelled(InstallError):
	pass


# --- Finding things ----------------------------------------------------------

def find_ci(base, rel):
	"""base/rel, matching each part of rel case-insensitively against what's there.
	Parts that don't exist yet keep rel's spelling."""
	path = Path(base)
	for part in Path(rel).parts:
		exact = path / part
		if not exact.exists() and path.is_dir():
			try:
				exact = next((p for p in path.iterdir() if p.name.lower() == part.lower()), exact)
			except OSError:
				pass
		path = exact
	return path


def is_game_dir(d):
	"""A folder is a GTA Vice City install if it has data/gta_vc.dat and models/gta3.img."""
	if not d:
		return False
	d = Path(d)
	return find_ci(d, "data/gta_vc.dat").is_file() and find_ci(d, "models/gta3.img").is_file()


def is_checkout(d):
	return bool(d) and (Path(d) / "mp" / "CMakeLists.txt").is_file() and (Path(d) / "gamefiles").is_dir()


def default_repo():
	here = Path(__file__).resolve().parent.parent
	return here if is_checkout(here) else Path.home() / "reVC"


def payload_dir():
	"""The prebuilt reVC that comes with a release download, or None."""
	base = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent))
	payload = base / "payload"
	return payload if (payload / "gamefiles").is_dir() else None


def payload_version():
	payload = payload_dir()
	try:
		return (payload / "VERSION").read_text().strip() if payload else ""
	except OSError:
		return ""


def downloads_dir():
	if IS_WIN:
		try:
			import winreg
			key = r"Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders"
			with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key) as k:
				value = winreg.QueryValueEx(k, "{374DE290-123F-4565-9164-39C4925E467B}")[0]
				return Path(os.path.expandvars(value))
		except OSError:
			pass
	elif IS_LINUX and shutil.which("xdg-user-dir"):
		try:
			out = subprocess.run(["xdg-user-dir", "DOWNLOAD"], capture_output=True, text=True, timeout=5).stdout.strip()
			if out and Path(out) != Path.home():
				return Path(out)
		except (OSError, subprocess.SubprocessError):
			pass
	return Path.home() / "Downloads"


def steam_roots():
	home = Path.home()
	if IS_WIN:
		return [Path(os.environ.get(v, "")) / "Steam" for v in ("ProgramFiles(x86)", "ProgramFiles") if os.environ.get(v)]
	if IS_MAC:
		return [home / "Library/Application Support/Steam"]
	return [home / ".steam/steam", home / ".local/share/Steam", home / ".var/app/com.valvesoftware.Steam/.local/share/Steam"]


def find_game_dirs():
	"""GTA Vice City folders in Downloads (up to 5 folders deep), then the usual Steam places."""
	found = []

	def add(d):
		if is_game_dir(d) and all(not os.path.samefile(d, f) for f in found):
			found.append(Path(d))

	base = downloads_dir()
	if base.is_dir():
		for dirpath, dirnames, filenames in os.walk(base):
			here = Path(dirpath)
			if here.name.lower() == "data" and any(f.lower() == "gta_vc.dat" for f in filenames):
				add(here.parent)
			depth = len(here.relative_to(base).parts)
			dirnames[:] = [] if depth >= 5 else [d for d in dirnames if not d.startswith(".")]
	for root in steam_roots():
		add(root / "steamapps" / "common" / GAME_NAME)
	return found


def is_inside(child, parent):
	try:
		Path(child).resolve().relative_to(Path(parent).resolve())
		return True
	except ValueError:
		return False


def default_dest(relay_only):
	return Path.home() / "Games" / ("reVC-Relay" if relay_only else "reVC-Multiplayer")


def quote_cmd(cmd):
	return subprocess.list2cmdline(cmd) if IS_WIN else " ".join(shlex.quote(str(c)) for c in cmd)


# --- The installer -------------------------------------------------------------

class Options:
	def __init__(self, repo, game=None, dest=None, relay_only=False, renderer="d3d9",
			update=False, skip_build=False, install_deps=True, shortcut=True, gui=False, prebuilt=False):
		self.repo = Path(repo).expanduser()
		self.game = Path(game).expanduser() if game else None
		# None installs into the game folder itself.
		self.dest = Path(dest).expanduser() if dest else None
		self.relay_only = relay_only
		self.renderer = renderer
		self.update = update
		self.skip_build = skip_build
		self.install_deps = install_deps
		self.shortcut = shortcut
		self.gui = gui
		# Install the build in payload_dir() instead of building.
		self.prebuilt = prebuilt


class Installer:
	"""Does the work. Reports through log(kind, text), kind being one of
	step, info, warn, cmd, out, progress ("done/total") or error."""

	def __init__(self, opts, log):
		self.o = opts
		self.log = log
		self.repo = opts.repo.resolve()
		self.payload = payload_dir() if opts.prebuilt else None
		if opts.prebuilt and not self.payload:
			raise InstallError("this installer has no included build. Build from source instead.")
		self.proc = None
		self.cancelled = False
		self.result = None

	# Plumbing

	def step(self, msg):
		self.log("step", msg)

	def info(self, msg):
		self.log("info", msg)

	def cancel(self):
		self.cancelled = True
		proc = self.proc
		if proc and proc.poll() is None:
			proc.terminate()

	def check_cancel(self):
		if self.cancelled:
			raise Cancelled("Cancelled.")

	def run(self, cmd, cwd=None, env=None):
		"""Runs a command, streaming its output to the log. Stops on failure."""
		self.check_cancel()
		cmd = [str(c) for c in cmd]
		self.log("cmd", quote_cmd(cmd))
		flags = subprocess.CREATE_NO_WINDOW if IS_WIN else 0
		try:
			self.proc = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE,
				stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, creationflags=flags)
		except OSError as e:
			raise InstallError("couldn't run %s: %s" % (cmd[0], e))
		for raw in self.proc.stdout:
			self.log("out", raw.decode("utf-8", "replace").rstrip())
		code = self.proc.wait()
		self.proc = None
		self.check_cancel()
		if code != 0:
			raise InstallError("%s failed (exit code %d)." % (Path(cmd[0]).name, code))

	def output(self, cmd):
		"""Runs a quick command and returns its stdout, or "" if it fails."""
		try:
			flags = subprocess.CREATE_NO_WINDOW if IS_WIN else 0
			r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, creationflags=flags)
			return r.stdout.strip() if r.returncode == 0 else ""
		except OSError:
			return ""

	def copy_tree(self, src, dst):
		"""Copies src's contents over dst, reporting progress."""
		files = []
		for dirpath, _, filenames in os.walk(src):
			files += [Path(dirpath) / f for f in filenames]
		total = len(files)
		for i, f in enumerate(files):
			self.check_cancel()
			target = Path(dst) / f.relative_to(src)
			target.parent.mkdir(parents=True, exist_ok=True)
			shutil.copy2(f, target)
			if i % 100 == 0 or i == total - 1:
				self.log("progress", "%d/%d" % (i + 1, total))

	# Entry point

	def install(self):
		try:
			if self.o.relay_only:
				self.install_relay()
			else:
				self.install_game()
		except PermissionError as e:
			raise InstallError("no permission to write %s.\nPick a folder you own (for example \"Into a new folder\"), "
				"or run the installer as administrator." % (e.filename or e))
		except shutil.Error as e:
			raise InstallError("copying failed: %s" % e)
		except OSError as e:
			raise InstallError(str(e))

	def install_game(self):
		o = self.o
		self.step("Checking your GTA Vice City folder")
		if not o.game:
			raise InstallError("choose your GTA Vice City folder (the one with data and models inside).")
		game = o.game.resolve()
		if not is_game_dir(game):
			raise InstallError("%s doesn't look like GTA Vice City (no data/gta_vc.dat or models/gta3.img)." % game)
		dest = o.dest.resolve() if o.dest else game
		if dest != game and is_inside(dest, game):
			raise InstallError("the new folder can't be inside the game folder. Pick a folder somewhere else.")
		self.info("Game files: %s" % game)
		self.info("Install to: %s%s" % (dest, "  (the game folder itself)" if dest == game else ""))

		if self.payload:
			self.info("Using the included build%s." % (" (%s)" % payload_version() if payload_version() else ""))
			self.check_runtime()
		elif o.skip_build:
			if not is_checkout(self.repo):
				raise InstallError("%s isn't a reVC checkout, so there's no build to reuse." % self.repo)
		else:
			self.check_tools(game=True)
			self.get_code(game=True)
			self.build_game()
		files = self.game_binaries()
		missing = [str(f) for f in files["game"] if not f.exists()]
		if missing:
			raise InstallError("no build found (%s). Install without \"skip build\"." % ", ".join(missing))

		self.step("Copying game files")
		dest.mkdir(parents=True, exist_ok=True)
		if dest == game:
			self.info("Installing into the game folder itself.")
		elif is_game_dir(dest):
			self.info("Already there, skipping. (Delete %s for a fresh copy.)" % dest)
		else:
			self.info("From %s (a few GB, give it a minute)" % game)
			self.copy_tree(game, dest)

		self.step("Adding reVC files")
		gamefiles = (self.payload or self.repo) / "gamefiles"
		backup = dest / "reVC-backup"
		if not backup.exists():
			saved = 0
			for dirpath, _, filenames in os.walk(gamefiles):
				for name in filenames:
					rel = (Path(dirpath) / name).relative_to(gamefiles)
					existing = find_ci(dest, rel)
					if existing.is_file():
						(backup / rel).parent.mkdir(parents=True, exist_ok=True)
						shutil.copy2(existing, backup / rel)
						saved += 1
			backup.mkdir(exist_ok=True)
			self.info("Backed up %d original files that reVC replaces to %s" % (saved, backup))
		for dirpath, _, filenames in os.walk(gamefiles):
			for name in filenames:
				src = Path(dirpath) / name
				target = find_ci(dest, src.relative_to(gamefiles))
				target.parent.mkdir(parents=True, exist_ok=True)
				shutil.copy2(src, target)

		for f in files["game"] + files["extra"]:
			if not f.exists():
				self.log("warn", "Skipping %s (not built)." % f.name)
				continue
			target = dest / f.name
			if f.name.endswith(".app.zip"):
				# macOS app bundles travel zipped so their symlinks and signatures survive.
				shutil.rmtree(dest / f.name[:-4], ignore_errors=True)
				self.run(["ditto", "-x", "-k", f, dest])
			elif f.is_dir():
				shutil.rmtree(target, ignore_errors=True)
				shutil.copytree(f, target, symlinks=True)
			else:
				shutil.copy2(f, target)
			self.info("Copied %s" % f.name)
		if IS_MAC:
			subprocess.run(["xattr", "-dr", "com.apple.quarantine", str(dest / "reVC.app")], capture_output=True)
		self.add_extras(dest)
		if o.shortcut:
			self.make_shortcut(dest)

		self.step("Done")
		self.info("Play:         %s" % self.play_hint(dest))
		self.info("Host a relay: %s" % self.relay_hint(dest))
		self.info("Guide:        %s" % (dest / "INSTALLATION.md"))
		self.result = dest

	def install_relay(self):
		o = self.o
		dest = (o.dest or default_dest(True)).resolve()
		self.info("Install to: %s" % dest)
		if not self.payload and not o.skip_build:
			self.check_tools(game=False)
			self.get_code(game=False)
			self.build_relay()
		tools = self.relay_binaries()
		if not tools[0].exists():
			raise InstallError("no build found at %s. Install without \"skip build\"." % tools[0])
		self.step("Installing the relay")
		dest.mkdir(parents=True, exist_ok=True)
		for f in tools:
			if f.exists():
				shutil.copy2(f, dest / f.name)
				self.info("Copied %s" % f.name)
		self.add_extras(dest)
		self.step("Done")
		self.info("Start it:  %s" % self.relay_hint(dest))
		self.info("Then open UDP port 7777 in the firewall. See INSTALLATION.md, \"Hosting a relay\".")
		self.result = dest

	# Build tools

	def check_tools(self, game):
		self.step("Checking build tools")
		if IS_LINUX:
			self.check_linux(game)
		elif IS_MAC:
			self.check_mac(game)
		elif IS_WIN:
			self.check_windows()
		else:
			self.log("warn", "Unknown system %s; trying anyway." % SYSTEM)

	def linux_missing(self, game):
		missing = [t for t in ("cmake", "git", "make") if not shutil.which(t)]
		if not any(shutil.which(c) for c in ("c++", "g++", "clang++")):
			missing.append("a C++ compiler")
		if game:
			if not shutil.which("pkg-config"):
				missing.append("pkg-config")
			else:
				missing += [m for m in LINUX_PKGCONFIG if subprocess.run(["pkg-config", "--exists", m]).returncode != 0]
		return missing

	def check_linux(self, game):
		missing = self.linux_missing(game)
		if not missing:
			self.info("All found.")
			return
		self.info("Missing: %s" % ", ".join(missing))
		manager = next((m for m in LINUX_PACKAGES if shutil.which(m)), None)
		if not manager:
			raise InstallError("install CMake, Git, a C++ compiler and the GLFW, OpenAL and mpg123 development "
				"packages with your package manager, then run this again.")
		cmd, tools, libs = LINUX_PACKAGES[manager]
		self.install_linux_packages(manager, tools + (libs if game else []))
		still = self.linux_missing(game)
		if still:
			self.log("warn", "Still not found: %s. Trying the build anyway." % ", ".join(still))

	def install_linux_packages(self, manager, packages):
		script = quote_cmd(LINUX_PACKAGES[manager][0] + packages)
		if manager == "apt-get":
			script = "apt-get update && " + script
		if not self.o.install_deps:
			raise InstallError("install them with:\n  sudo sh -c '%s'\nthen run this again." % script)
		if os.geteuid() == 0:
			self.run(["sh", "-c", script])
		elif self.o.gui:
			if not shutil.which("pkexec"):
				raise InstallError("install them in a terminal with:\n  sudo sh -c '%s'\nthen run this again." % script)
			self.info("Installing packages (your system asks for your password)")
			self.run(["pkexec", "sh", "-c", script])
		else:
			self.info("Installing packages with sudo (it may ask for your password)")
			self.run(["sudo", "sh", "-c", script])

	def check_runtime(self):
		"""The included build links against GLFW, OpenAL and mpg123 on Linux and macOS."""
		if IS_WIN:
			return
		self.step("Checking libraries")
		if IS_MAC:
			self.brew_install(self.find_brew(), MAC_FORMULAS_LIBS)
			return
		missing = self.ldd_missing()
		if not missing:
			self.info("All found.")
			return
		self.info("Missing: %s" % ", ".join(missing))
		manager = next((m for m in LINUX_RUNTIME if shutil.which(m)), None)
		if not manager:
			raise InstallError("install GLFW 3, OpenAL and mpg123 with your package manager, then run this again.")
		self.install_linux_packages(manager, LINUX_RUNTIME[manager])
		still = self.ldd_missing()
		if still:
			self.log("warn", "Still not found: %s. reVC may not start." % ", ".join(still))

	def ldd_missing(self):
		out = self.output(["ldd", self.payload / "bin" / "reVC"])
		return [line.split()[0] for line in out.splitlines() if "not found" in line]

	def find_brew(self):
		brew = shutil.which("brew") or next((b for b in ("/opt/homebrew/bin/brew", "/usr/local/bin/brew") if os.path.exists(b)), None)
		if not brew:
			raise InstallError("Homebrew is needed. Install it from https://brew.sh, then run this again.")
		# Apps started from Finder get a bare PATH; CMake needs to see Homebrew's tools.
		os.environ["PATH"] = str(Path(brew).parent) + os.pathsep + os.environ.get("PATH", "")
		return brew

	def brew_install(self, brew, formulas):
		for formula in formulas:
			if self.output([brew, "list", "--versions", formula]):
				self.info("%s: installed" % formula)
			elif self.o.install_deps:
				self.info("%s: installing" % formula)
				self.run([brew, "install", formula])
			else:
				raise InstallError("%s is missing. Install it with: brew install %s" % (formula, formula))

	def check_mac(self, game):
		if not self.output(["xcode-select", "-p"]):
			subprocess.run(["xcode-select", "--install"], capture_output=True)
			raise InstallError("the Xcode Command Line Tools are needed. Finish the install window that just "
				"opened, then run this again.")
		self.brew_install(self.find_brew(), MAC_FORMULAS_TOOLS + (MAC_FORMULAS_LIBS if game else []))

	def check_windows(self):
		vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
		vs_query = ["-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64"]
		vs_path = self.output([vswhere] + vs_query + ["-property", "installationPath"]) if vswhere.exists() else ""
		if not vs_path:
			if not (self.o.install_deps and shutil.which("winget")):
				raise InstallError("Visual Studio with C++ is needed. Install Visual Studio Community "
					"(https://visualstudio.microsoft.com/vs/community/) with the \"Desktop development with C++\" "
					"workload, then run this again.")
			self.info("Installing Visual Studio Build Tools with C++ (a few GB; Windows asks for permission)")
			self.run(["winget", "install", "--id", VS_BUILD_TOOLS, "-e", "--source", "winget",
				"--accept-package-agreements", "--accept-source-agreements",
				"--override", "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"])
			vs_path = self.output([vswhere] + vs_query + ["-property", "installationPath"])
			if not vs_path:
				raise InstallError("Visual Studio's C++ tools still weren't found. Restart the PC and run this again.")
		vs_version = self.output([vswhere] + vs_query + ["-property", "installationVersion"])
		msbuild = self.output([vswhere, "-latest", "-products", "*", "-requires", "Microsoft.Component.MSBuild",
			"-find", r"MSBuild\**\Bin\MSBuild.exe"]).splitlines()
		if not msbuild:
			raise InstallError("MSBuild.exe not found in %s." % vs_path)
		major = int(vs_version.split(".")[0])
		if major < 16:
			raise InstallError("Visual Studio 2019 or newer is needed (found %s)." % vs_version)
		self.premake_action = {16: "vs2019", 17: "vs2022"}.get(major, "vs2026")
		self.msbuild = msbuild[0]
		self.info("Visual Studio %s (%s)" % (vs_version, self.premake_action))

		self.cmake = shutil.which("cmake") or str(Path(vs_path) / r"Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe")
		if not os.path.exists(self.cmake):
			raise InstallError("CMake not found. Install it from https://cmake.org/download/ or add \"C++ CMake "
				"tools for Windows\" in the Visual Studio Installer.")
		self.info("CMake: %s" % self.cmake)

		if not shutil.which("git"):
			git_dir = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Git" / "cmd"
			if not (git_dir / "git.exe").exists() and self.o.install_deps and shutil.which("winget"):
				self.info("Installing Git")
				self.run(["winget", "install", "--id", "Git.Git", "-e", "--source", "winget",
					"--accept-package-agreements", "--accept-source-agreements"])
			if (git_dir / "git.exe").exists():
				os.environ["PATH"] = str(git_dir) + os.pathsep + os.environ.get("PATH", "")

	# Source code

	def get_code(self, game):
		repo = self.repo
		if not is_checkout(repo):
			if (repo / "CMakeLists.txt").exists():
				raise InstallError("%s has no multiplayer code. Switch it to the %s branch:\n  git -C \"%s\" checkout %s"
					% (repo, BRANCH, repo, BRANCH))
			if repo.exists() and any(repo.iterdir()):
				raise InstallError("%s isn't empty and isn't a reVC checkout. Pick your reVC folder, or an empty "
					"folder to download into." % repo)
			if not shutil.which("git"):
				raise InstallError("Git is needed to download reVC. Install it from https://git-scm.com, then run this again.")
			self.step("Downloading reVC")
			repo.parent.mkdir(parents=True, exist_ok=True)
			self.run(["git", "clone", "--branch", BRANCH, REPO_URL, repo])
		elif self.o.update:
			if not (repo / ".git").exists():
				raise InstallError("%s isn't a Git checkout, so it can't be updated." % repo)
			self.step("Updating reVC")
			self.run(["git", "-C", repo, "pull", "--ff-only"])
		if game:
			librw = repo / "vendor" / "librw" / "CMakeLists.txt"
			if (repo / ".git").exists() and shutil.which("git"):
				self.step("Fetching submodules")
				self.run(["git", "-C", repo, "submodule", "update", "--init", "--recursive", "vendor/librw"])
			if not librw.exists():
				raise InstallError("vendor/librw is empty. Download the code with Git (a ZIP from GitHub leaves it out):\n"
					"  git clone --recursive -b %s %s" % (BRANCH, REPO_URL))

	# Building

	def jobs(self):
		return str(os.cpu_count() or 2)

	def build_game(self):
		self.step("Building reVC (this takes a few minutes)")
		if IS_WIN:
			self.build_game_windows()
			return
		bdir = self.repo / "build" / ("macos" if IS_MAC else "linux")
		self.run(["cmake", "-S", self.repo, "-B", bdir, "-DCMAKE_BUILD_TYPE=Release", "-DREVC_VENDORED_LIBRW=ON",
			"-DLIBRW_PLATFORM=GL3", "-DLIBRW_GL3_GFXLIB=GLFW", "-DREVC_AUDIO=OAL"])
		self.run(["cmake", "--build", bdir, "--parallel", self.jobs()])

	def win_platform(self):
		return "win-amd64-librw_gl3_glfw-oal" if self.o.renderer == "gl3" else "win-amd64-librw_d3d9-oal"

	def build_game_windows(self):
		bdir = self.repo / "build" / "windows"
		bdir.mkdir(parents=True, exist_ok=True)
		glfw_dir = bdir / GLFW_BASE
		if self.o.renderer == "gl3":
			if not glfw_dir.exists():
				self.info("Downloading GLFW")
				zip_path = bdir / (GLFW_BASE + ".zip")
				urllib.request.urlretrieve(GLFW_URL, zip_path)
				with zipfile.ZipFile(zip_path) as z:
					z.extractall(bdir)
			# premake links GLFW from lib-vcYYYY matching the VS version; newer VS can use the newest GLFW ships.
			lib = glfw_dir / ("lib-" + self.premake_action.replace("vs", "vc"))
			if not lib.exists():
				newest = sorted(p for p in glfw_dir.glob("lib-vc*") if p.is_dir())[-1]
				shutil.copytree(newest, lib)
		self.run([self.repo / "premake5.exe", self.premake_action, "--with-librw", "--no-full-paths",
			"--glfwdir64=%s" % glfw_dir], cwd=self.repo)
		self.run([self.msbuild, "-m", r"build\reVC.sln", "/p:Configuration=Release",
			"/p:Platform=%s" % self.win_platform(), "/v:minimal"], cwd=self.repo)
		self.step("Building revc-relay, revc-server and revc-fakeclient")
		mp = bdir / "mp"
		self.run([self.cmake, "-S", self.repo / "mp", "-B", mp])
		self.run([self.cmake, "--build", mp, "--config", "Release", "--target"] + TOOLS)

	def build_relay(self):
		self.step("Building revc-relay, revc-server and revc-fakeclient")
		cmake = getattr(self, "cmake", "cmake")
		bdir = self.repo / "build" / "relay"
		self.run([cmake, "-S", self.repo / "mp", "-B", bdir] + ([] if IS_WIN else ["-DCMAKE_BUILD_TYPE=Release"]))
		self.run([cmake, "--build", bdir, "--config", "Release", "--target"] + TOOLS + ["--parallel", self.jobs()])

	def game_binaries(self):
		"""{"game": what must exist to install, "extra": nice to have}."""
		if self.payload:
			b = self.payload / "bin"
			tools = [b / (t + EXE) for t in TOOLS]
			if IS_WIN:
				r = b / self.o.renderer
				extra = sorted(f for f in r.iterdir() if f.name != "reVC.exe") if r.is_dir() else []
				return {"game": [r / "reVC.exe"], "extra": extra + sorted(b.glob("*.dll")) + tools}
			return {"game": [b / ("reVC.app.zip" if IS_MAC else "reVC")], "extra": tools}
		repo = self.repo
		if IS_WIN:
			tools = repo / "build" / "windows" / "mp" / "Release"
			game = [repo / "bin" / self.win_platform() / "Release" / "reVC.exe"]
			extra = [repo / "vendor/mpg123/dist/Win64/libmpg123-0.dll", repo / "vendor/openal-soft/dist/Win64/OpenAL32.dll"]
		else:
			bdir = repo / "build" / ("macos" if IS_MAC else "linux")
			tools = bdir / "mp"
			game = [bdir / "src" / ("reVC.app" if IS_MAC else "reVC")]
			extra = []
		return {"game": game, "extra": extra + [tools / (t + EXE) for t in TOOLS]}

	def relay_binaries(self):
		if self.payload:
			bdir = self.payload / "bin"
		else:
			bdir = self.repo / "build" / "relay"
			if IS_WIN:
				bdir = bdir / "Release"
		return [bdir / (t + EXE) for t in TOOLS]

	# Finishing touches

	def add_extras(self, dest):
		"""INSTALLATION.md and a double-clickable relay starter."""
		guide = (self.payload or self.repo) / "INSTALLATION.md"
		if guide.exists():
			shutil.copy2(guide, dest / "INSTALLATION.md")
		if IS_WIN:
			(dest / "start-relay.bat").write_text(
				"@echo off\r\nrem Starts the reVC multiplayer relay on UDP port 7777. Close this window to stop it.\r\n"
				"cd /d \"%~dp0\"\r\nrevc-relay.exe --port 7777\r\npause\r\n")
		else:
			script = dest / ("start-relay.command" if IS_MAC else "start-relay.sh")
			script.write_text("#!/bin/sh\n# Starts the reVC multiplayer relay. Press Ctrl+C to stop it.\n"
				"cd \"$(dirname \"$0\")\" && exec ./revc-relay --port \"${1:-7777}\"\n")
			script.chmod(0o755)
			for f in ["reVC"] + TOOLS:
				if (dest / f).is_file():
					(dest / f).chmod(0o755)

	def make_shortcut(self, dest):
		if IS_LINUX:
			icon = self.payload / "revc-icon.jpg" if self.payload else self.repo / "res" / "images" / "logo_256.jpg"
			if icon.exists():
				shutil.copy2(icon, dest / "revc-icon.jpg")
			apps = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")) / "applications"
			apps.mkdir(parents=True, exist_ok=True)
			desktop = apps / "revc-multiplayer.desktop"
			desktop.write_text("[Desktop Entry]\nType=Application\nName=reVC Multiplayer\n"
				"Comment=GTA Vice City with multiplayer\nExec=\"%s\"\nPath=%s\nIcon=%s\nTerminal=false\nCategories=Game;\n"
				% (dest / "reVC", dest, dest / "revc-icon.jpg"))
			desktop.chmod(0o755)
			self.info("Added reVC Multiplayer to your applications menu.")
		elif IS_WIN:
			env = dict(os.environ, REVC_TARGET=str(dest / "reVC.exe"), REVC_DIR=str(dest))
			ps = ("$l = Join-Path ([Environment]::GetFolderPath('Desktop')) 'reVC Multiplayer.lnk';"
				"$s = (New-Object -ComObject WScript.Shell).CreateShortcut($l);"
				"$s.TargetPath = $env:REVC_TARGET; $s.WorkingDirectory = $env:REVC_DIR; $s.Save()")
			try:
				self.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", ps], env=env)
				self.info("Added a reVC Multiplayer shortcut to your desktop.")
			except InstallError as e:
				self.log("warn", "Couldn't make a desktop shortcut: %s" % e)

	def play_hint(self, dest):
		if IS_WIN:
			return "\"%s\"" % (dest / "reVC.exe")
		if IS_MAC:
			return "open \"%s\"" % (dest / "reVC.app")
		return "cd \"%s\" && ./reVC" % dest

	def relay_hint(self, dest):
		return "\"%s\"" % (dest / ("start-relay.bat" if IS_WIN else "start-relay.command" if IS_MAC else "start-relay.sh"))


def launch_game(dest):
	dest = Path(dest)
	if IS_MAC:
		subprocess.Popen(["open", str(dest / "reVC.app")])
	else:
		subprocess.Popen([str(dest / ("reVC" + EXE))], cwd=str(dest))


def open_folder(path):
	if IS_WIN:
		os.startfile(str(path))
	else:
		subprocess.Popen(["open" if IS_MAC else "xdg-open", str(path)])


# --- Terminal mode -------------------------------------------------------------

def run_cli(args):
	color = sys.stdout.isatty() and not IS_WIN

	def paint(code, text):
		return "\033[%sm%s\033[0m" % (code, text) if color else text

	def log(kind, text):
		if kind == "step":
			print("\n" + paint("1;34", "==> " + text))
		elif kind == "info":
			print("    " + text)
		elif kind == "warn":
			print(paint("33", "    Warning: " + text))
		elif kind == "cmd":
			print(paint("2", "$ " + text))
		elif kind == "progress":
			done, total = text.split("/")
			end = "\n" if done == total else "\r"
			print("    copied %s of %s files" % (done, total), end=end, flush=True)
		else:
			print(text)

	game = args.game
	if not args.relay_only and not game:
		found = find_game_dirs()
		if not found:
			print("No GTA Vice City folder found in Downloads or Steam. Pass it with --game \"/path/to/GTA Vice City\".")
			return 1
		game = found[0]
		print("Using the GTA Vice City folder at %s (pass --game to pick another)." % game)
	opts = Options(args.repo, game=game, dest=args.dest, relay_only=args.relay_only, renderer=args.renderer,
		update=args.update, skip_build=args.skip_build, install_deps=not args.no_deps, shortcut=not args.no_shortcut,
		prebuilt=payload_dir() is not None and not args.from_source)
	installer = Installer(opts, log)
	try:
		installer.install()
	except KeyboardInterrupt:
		installer.cancel()
		print("\nCancelled.")
		return 130
	except InstallError as e:
		sys.stdout.flush()
		print("\n" + paint("1;31", "Error: ") + str(e), file=sys.stderr)
		return 1
	return 0


# --- Window mode ---------------------------------------------------------------

def run_gui(args):
	import tkinter as tk
	from tkinter import filedialog, messagebox, ttk

	root = tk.Tk()
	payload = payload_dir()
	version = payload_version()
	root.title("reVC Multiplayer Installer" + (" %s" % version if version else ""))
	root.minsize(720, 640)
	events = queue.Queue()
	state = {"installer": None, "relay": None, "dest": None}

	repo_var = tk.StringVar(value=str(args.repo))
	game_var = tk.StringVar(value=str(args.game or ""))
	mode_var = tk.StringVar(value="relay" if args.relay_only else "game")
	where_var = tk.StringVar(value="copy" if args.dest else "inplace")
	dest_var = tk.StringVar(value=str(args.dest or default_dest(args.relay_only)))
	renderer_var = tk.StringVar(value=args.renderer)
	update_var = tk.BooleanVar(value=args.update)
	deps_var = tk.BooleanVar(value=not args.no_deps)
	shortcut_var = tk.BooleanVar(value=not args.no_shortcut)
	skip_var = tk.BooleanVar(value=args.skip_build)
	source_var = tk.StringVar(value="source" if args.from_source or not payload else "prebuilt")

	frame = ttk.Frame(root, padding=12)
	frame.pack(fill="both", expand=True)
	frame.columnconfigure(1, weight=1)
	row = 0

	ttk.Label(frame, text="reVC Multiplayer", font=("TkDefaultFont", 16, "bold")).grid(row=row, column=0, columnspan=3, sticky="w")
	row += 1
	ttk.Label(frame, text="Installs reVC with multiplayer on top of your own copy of GTA Vice City.").grid(
		row=row, column=0, columnspan=3, sticky="w", pady=(0, 10))
	row += 1

	def browse(var, title):
		start = var.get() if os.path.isdir(var.get()) else str(Path.home())
		chosen = filedialog.askdirectory(title=title, initialdir=start, mustexist=False)
		if chosen:
			var.set(chosen)

	def labelled_path(label, var, title, hint):
		nonlocal row
		ttk.Label(frame, text=label).grid(row=row, column=0, sticky="w", padx=(0, 8))
		entry = ttk.Entry(frame, textvariable=var)
		entry.grid(row=row, column=1, sticky="ew")
		button = ttk.Button(frame, text="Browse…", command=lambda: browse(var, title))
		button.grid(row=row, column=2, padx=(6, 0))
		row += 1
		note = ttk.Label(frame, text=hint, foreground="gray")
		note.grid(row=row, column=1, sticky="w", pady=(0, 6))
		row += 1
		return entry, button, note

	ttk.Label(frame, text="Install").grid(row=row, column=0, sticky="w")
	modes = ttk.Frame(frame)
	modes.grid(row=row, column=1, columnspan=2, sticky="w")
	ttk.Radiobutton(modes, text="Game + multiplayer (to play)", value="game", variable=mode_var).pack(side="left")
	ttk.Radiobutton(modes, text="Relay server only (to host on a server)", value="relay", variable=mode_var).pack(side="left", padx=(12, 0))
	row += 1
	ttk.Frame(frame, height=8).grid(row=row, column=0)
	row += 1

	if payload:
		ttk.Label(frame, text="Get reVC").grid(row=row, column=0, sticky="w")
		sources = ttk.Frame(frame)
		sources.grid(row=row, column=1, columnspan=2, sticky="w")
		ttk.Radiobutton(sources, text="Use the included build%s (recommended)" % (" " + version if version else ""),
			value="prebuilt", variable=source_var).pack(side="left")
		ttk.Radiobutton(sources, text="Build from source code", value="source", variable=source_var).pack(side="left", padx=(12, 0))
		row += 1
		ttk.Frame(frame, height=8).grid(row=row, column=0)
		row += 1

	repo_entry, repo_button, repo_note = labelled_path("reVC source code", repo_var, "Where the reVC code is (or should be downloaded to)", "")
	game_entry, game_button, game_note = labelled_path("GTA Vice City folder", game_var, "Your GTA Vice City folder", "")
	find_button = ttk.Button(frame, text="Find it for me")
	find_button.grid(row=row - 1, column=2, padx=(6, 0), sticky="e")

	ttk.Label(frame, text="Put reVC").grid(row=row, column=0, sticky="nw")
	where = ttk.Frame(frame)
	where.grid(row=row, column=1, columnspan=2, sticky="ew")
	where.columnconfigure(1, weight=1)
	inplace_radio = ttk.Radiobutton(where, text="Into the game folder (original files that get replaced are backed up)",
		value="inplace", variable=where_var)
	inplace_radio.grid(row=0, column=0, columnspan=3, sticky="w")
	copy_radio = ttk.Radiobutton(where, text="Into a new folder:", value="copy", variable=where_var)
	copy_radio.grid(row=1, column=0, sticky="w")
	dest_entry = ttk.Entry(where, textvariable=dest_var)
	dest_entry.grid(row=1, column=1, sticky="ew", padx=(6, 0))
	dest_button = ttk.Button(where, text="Browse…", command=lambda: browse(dest_var, "Install into"))
	dest_button.grid(row=1, column=2, padx=(6, 0))
	row += 1
	ttk.Frame(frame, height=8).grid(row=row, column=0)
	row += 1

	options = ttk.Frame(frame)
	options.grid(row=row, column=1, columnspan=2, sticky="w")
	ttk.Label(frame, text="Options").grid(row=row, column=0, sticky="nw")
	renderer_box = None
	if IS_WIN:
		r = ttk.Frame(options)
		r.pack(anchor="w")
		ttk.Label(r, text="Renderer:").pack(side="left")
		renderer_box = ttk.Combobox(r, textvariable=renderer_var, values=["d3d9", "gl3"], state="readonly", width=8)
		renderer_box.pack(side="left", padx=6)
		ttk.Label(r, text="d3d9 suits most PCs; try gl3 (OpenGL) if you see graphics problems", foreground="gray").pack(side="left")
	deps_text = {"Windows": "Install missing tools (Visual Studio Build Tools, Git) with winget",
		"Darwin": "Install missing tools and libraries with Homebrew"}.get(SYSTEM, "Install missing packages (asks for your password)")
	ttk.Checkbutton(options, text=deps_text, variable=deps_var).pack(anchor="w")
	update_check = ttk.Checkbutton(options, text="Update to the latest code first (git pull)", variable=update_var)
	update_check.pack(anchor="w")
	shortcut_check = ttk.Checkbutton(options, text="Add a shortcut to the desktop / applications menu", variable=shortcut_var)
	if not IS_MAC:
		shortcut_check.pack(anchor="w")
	skip_check = ttk.Checkbutton(options, text="Skip the build (reuse the last one)", variable=skip_var)
	skip_check.pack(anchor="w")
	row += 1

	buttons = ttk.Frame(frame)
	buttons.grid(row=row, column=0, columnspan=3, sticky="ew", pady=(10, 6))
	install_button = ttk.Button(buttons, text="Install")
	install_button.pack(side="left")
	cancel_button = ttk.Button(buttons, text="Cancel", state="disabled")
	cancel_button.pack(side="left", padx=6)
	folder_button = ttk.Button(buttons, text="Open folder", state="disabled")
	folder_button.pack(side="right")
	relay_button = ttk.Button(buttons, text="Start relay", state="disabled")
	relay_button.pack(side="right", padx=6)
	play_button = ttk.Button(buttons, text="Play", state="disabled")
	play_button.pack(side="right")
	row += 1

	progress = ttk.Progressbar(frame, mode="indeterminate")
	progress.grid(row=row, column=0, columnspan=3, sticky="ew")
	row += 1
	status = ttk.Label(frame, text="Ready.")
	status.grid(row=row, column=0, columnspan=3, sticky="w", pady=(4, 4))
	row += 1

	log_frame = ttk.Frame(frame)
	log_frame.grid(row=row, column=0, columnspan=3, sticky="nsew")
	frame.rowconfigure(row, weight=1)
	log_text = tk.Text(log_frame, height=14, wrap="word", font=("TkFixedFont", 9), state="disabled")
	scroll = ttk.Scrollbar(log_frame, command=log_text.yview)
	log_text.configure(yscrollcommand=scroll.set)
	log_text.pack(side="left", fill="both", expand=True)
	scroll.pack(side="right", fill="y")
	log_text.tag_configure("step", foreground="#1f5fbf", font=("TkFixedFont", 9, "bold"))
	log_text.tag_configure("error", foreground="#c0392b", font=("TkFixedFont", 9, "bold"))
	log_text.tag_configure("warn", foreground="#b9770e")
	log_text.tag_configure("cmd", foreground="gray")
	log_text.tag_configure("ok", foreground="#1e8449", font=("TkFixedFont", 9, "bold"))

	def append(text, tag=None):
		log_text.configure(state="normal")
		log_text.insert("end", text + "\n", tag)
		if int(log_text.index("end-1c").split(".")[0]) > 5000:
			log_text.delete("1.0", "1000.0")
		log_text.see("end")
		log_text.configure(state="disabled")

	# Live hints under the folder fields.

	def refresh(*_):
		relay = mode_var.get() == "relay"
		prebuilt = payload is not None and source_var.get() == "prebuilt"
		busy = state["installer"] is not None
		for w in (repo_entry, repo_button, update_check, skip_check):
			w.configure(state="disabled" if prebuilt or busy else "normal")
		repo = repo_var.get().strip()
		if prebuilt:
			repo_note.configure(text="Not needed: the included build is used.", foreground="gray")
		elif is_checkout(repo):
			repo_note.configure(text="✓ reVC code found here.", foreground="#1e8449")
		elif repo and os.path.exists(os.path.join(repo, "CMakeLists.txt")):
			repo_note.configure(text="✗ This reVC checkout has no multiplayer code (wrong branch).", foreground="#c0392b")
		else:
			repo_note.configure(text="reVC will be downloaded here (needs Git).", foreground="gray")
		game = game_var.get().strip()
		if relay:
			game_note.configure(text="Not needed for the relay.", foreground="gray")
		elif is_game_dir(game):
			game_note.configure(text="✓ Looks like GTA Vice City.", foreground="#1e8449")
		elif game:
			game_note.configure(text="✗ No data/gta_vc.dat or models/gta3.img in this folder.", foreground="#c0392b")
		else:
			game_note.configure(text="The folder with data, models and Audio inside.", foreground="gray")
		game_state = "disabled" if relay or busy else "normal"
		for w in (game_entry, game_button, find_button, inplace_radio):
			w.configure(state=game_state)
		copy_radio.configure(state="disabled" if relay or busy else "normal")
		copying = relay or where_var.get() == "copy"
		for w in (dest_entry, dest_button):
			w.configure(state="normal" if copying and not busy else "disabled")
		if renderer_box is not None:
			renderer_box.configure(state="disabled" if relay or busy else "readonly")

	def mode_changed(*_):
		if mode_var.get() == "relay" and dest_var.get() == str(default_dest(False)):
			dest_var.set(str(default_dest(True)))
		elif mode_var.get() == "game" and dest_var.get() == str(default_dest(True)):
			dest_var.set(str(default_dest(False)))
		refresh()

	for var in (repo_var, game_var, where_var, source_var):
		var.trace_add("write", refresh)
	mode_var.trace_add("write", mode_changed)

	def find_game():
		status.configure(text="Looking in Downloads and Steam…")
		root.update_idletasks()
		found = find_game_dirs()
		if not found:
			status.configure(text="No GTA Vice City folder found. Use Browse… to pick it.")
			return
		game_var.set(str(found[0]))
		status.configure(text="Found %s%s" % (found[0], " (and %d more)" % (len(found) - 1) if len(found) > 1 else ""))
	find_button.configure(command=find_game)

	# Running the install on a worker thread; it talks to the window through events.

	def worker(installer):
		try:
			installer.install()
			events.put(("finished", installer.result))
		except InstallError as e:
			events.put(("failed", str(e)))
		except Exception as e:  # report anything unexpected instead of dying silently
			events.put(("failed", "unexpected error: %r" % e))

	def start():
		relay = mode_var.get() == "relay"
		prebuilt = payload is not None and source_var.get() == "prebuilt"
		if not prebuilt and not repo_var.get().strip():
			messagebox.showerror("reVC", "Choose a folder for the reVC source code.")
			return
		if not relay and not is_game_dir(game_var.get().strip()):
			messagebox.showerror("reVC", "Choose your GTA Vice City folder first (the one with data and models inside).")
			return
		dest = dest_var.get().strip() if relay or where_var.get() == "copy" else None
		if dest is not None and not dest:
			messagebox.showerror("reVC", "Choose where to install.")
			return
		if not relay and dest is None and not messagebox.askokcancel("reVC",
				"reVC will be installed into your game folder:\n\n%s\n\nFiles it replaces are backed up to "
				"reVC-backup inside it. Continue?" % game_var.get().strip()):
			return
		opts = Options(repo_var.get().strip(), game=game_var.get().strip() or None, dest=dest, relay_only=relay,
			renderer=renderer_var.get(), update=update_var.get(), skip_build=skip_var.get(),
			install_deps=deps_var.get(), shortcut=shortcut_var.get(), gui=True, prebuilt=prebuilt)
		installer = Installer(opts, lambda kind, text: events.put((kind, text)))
		state["installer"] = installer
		log_text.configure(state="normal")
		log_text.delete("1.0", "end")
		log_text.configure(state="disabled")
		install_button.configure(state="disabled")
		cancel_button.configure(state="normal")
		for b in (play_button, relay_button, folder_button):
			b.configure(state="disabled")
		progress.configure(mode="indeterminate")
		progress.start(12)
		status.configure(text="Working…")
		refresh()
		threading.Thread(target=worker, args=(installer,), daemon=True).start()

	def cancel():
		if state["installer"]:
			state["installer"].cancel()
			status.configure(text="Cancelling…")

	def finish(ok, payload):
		state["installer"] = None
		progress.stop()
		progress.configure(mode="determinate", value=100 if ok else 0)
		install_button.configure(state="normal")
		cancel_button.configure(state="disabled")
		refresh()
		if ok:
			state["dest"] = Path(payload)
			relay_only = mode_var.get() == "relay"
			status.configure(text="Installed to %s" % payload)
			append("\nAll done! See INSTALLATION.md in the install folder for how to host and join.", "ok")
			play_button.configure(state="disabled" if relay_only else "normal")
			relay_button.configure(state="normal")
			folder_button.configure(state="normal")
		else:
			status.configure(text="Failed. See the log below.")
			append("\nError: " + payload, "error")

	def pump():
		try:
			while True:
				kind, text = events.get_nowait()
				if kind == "progress":
					done, total = (int(x) for x in text.split("/"))
					progress.stop()
					progress.configure(mode="determinate", maximum=total, value=done)
					status.configure(text="Copying game files: %d of %d" % (done, total))
					if done == total:
						progress.configure(mode="indeterminate")
						progress.start(12)
				elif kind == "step":
					append("\n==> " + text, "step")
					status.configure(text=text + "…")
				elif kind == "info":
					append("    " + text)
				elif kind == "warn":
					append("    Warning: " + text, "warn")
				elif kind == "cmd":
					append("$ " + text, "cmd")
				elif kind == "out":
					append(text)
				elif kind == "relay":
					append("[relay] " + text, "cmd")
				elif kind == "relay-stopped":
					state["relay"] = None
					relay_button.configure(text="Start relay")
					append("[relay] stopped", "warn")
				elif kind in ("finished", "failed"):
					finish(kind == "finished", text)
		except queue.Empty:
			pass
		root.after(80, pump)

	# The relay runs inside the installer window so its log is visible and Stop works everywhere.

	def toggle_relay():
		proc = state["relay"]
		if proc:
			proc.terminate()
			return
		exe = state["dest"] / ("revc-relay" + EXE)
		if not exe.exists():
			messagebox.showerror("reVC", "revc-relay wasn't installed.")
			return
		flags = subprocess.CREATE_NO_WINDOW if IS_WIN else 0
		proc = subprocess.Popen([str(exe), "--port", "7777"], cwd=str(state["dest"]), stdout=subprocess.PIPE,
			stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, creationflags=flags)
		state["relay"] = proc
		relay_button.configure(text="Stop relay")
		append("\n[relay] running on UDP port 7777. In the game use 127.0.0.1:7777 on this PC, or this PC's "
			"address from other machines.", "ok")

		def read():
			for raw in proc.stdout:
				events.put(("relay", raw.decode("utf-8", "replace").rstrip()))
			proc.wait()
			events.put(("relay-stopped", ""))
		threading.Thread(target=read, daemon=True).start()

	def play():
		try:
			launch_game(state["dest"])
		except OSError as e:
			messagebox.showerror("reVC", "Couldn't start reVC: %s" % e)

	def on_close():
		if state["installer"] and not messagebox.askokcancel("reVC", "The install is still running. Stop it and quit?"):
			return
		if state["installer"]:
			state["installer"].cancel()
		if state["relay"]:
			state["relay"].terminate()
		root.destroy()

	install_button.configure(command=start)
	cancel_button.configure(command=cancel)
	play_button.configure(command=play)
	relay_button.configure(command=toggle_relay)
	folder_button.configure(command=lambda: open_folder(state["dest"]))
	root.protocol("WM_DELETE_WINDOW", on_close)

	if not args.game and not args.relay_only:
		found = find_game_dirs()
		if found:
			game_var.set(str(found[0]))
	refresh()
	pump()
	if args.smoke_test:
		root.after(500, root.destroy)
	root.mainloop()
	return 0


def main():
	parser = argparse.ArgumentParser(description="Build and install reVC multiplayer. Opens a window unless --cli is given.")
	parser.add_argument("--cli", action="store_true", help="Run in the terminal instead of opening a window.")
	parser.add_argument("--repo", default=str(default_repo()),
		help="reVC source folder. Downloaded from GitHub if it isn't a checkout yet. Default: %(default)s")
	parser.add_argument("--game", help="Your GTA Vice City folder. Default: searched for in Downloads and Steam.")
	parser.add_argument("--dest", help="Install into a copy of the game in this folder, instead of into the game folder itself. "
		"With --relay-only: where to put the relay (default %s)." % default_dest(True))
	parser.add_argument("--relay-only", action="store_true", help="Build and install only revc-relay and revc-fakeclient (no game needed).")
	parser.add_argument("--renderer", choices=["d3d9", "gl3"], default="d3d9", help="Windows only: Direct3D 9 (default) or OpenGL.")
	parser.add_argument("--update", action="store_true", help="git pull the latest code before building.")
	parser.add_argument("--skip-build", action="store_true", help="Reuse the last build instead of building.")
	parser.add_argument("--no-deps", action="store_true", help="Don't install missing tools or libraries; just say what's missing.")
	parser.add_argument("--no-shortcut", action="store_true", help="Don't add a desktop or applications-menu shortcut.")
	parser.add_argument("--from-source", action="store_true",
		help="Build from source even though this download includes a prebuilt reVC.")
	parser.add_argument("--smoke-test", action="store_true", help=argparse.SUPPRESS)
	args = parser.parse_args()

	if sys.version_info < (3, 7):
		print("Python 3.7 or newer is needed.")
		return 1
	if not args.cli:
		if IS_LINUX and not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
			print("No display found, running in the terminal.")
			args.cli = True
		else:
			try:
				import tkinter  # noqa: F401
			except ImportError:
				print("Tkinter isn't installed, so running in the terminal. For the window, install it "
					"(e.g. sudo apt install python3-tk, or brew install python-tk).")
				args.cli = True
	return run_cli(args) if args.cli else run_gui(args)


if __name__ == "__main__":
	sys.exit(main())
