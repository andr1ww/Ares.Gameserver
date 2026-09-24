#!/usr/bin/env python3
"""builds Ares.dll with zig (pip install ziglang), no visual studio needed

    python build.py
    python build.py --clean
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BUILD_DIR = ROOT / "build"
OUTPUT = ROOT / "Ares.dll"

TARGET = "x86_64-windows-gnu"

SOURCES = [
    "pch.cpp",
    "SDK/Basic.cpp",
    "SDK/CoreUObject_functions.cpp",
    "SDK/Engine_functions.cpp",
    "SDK/ShooterGame_functions.cpp",
    "SDK/UMG_functions.cpp",
    "SDK/ProceduralMeshComponent_functions.cpp",
    "SDK/NavigationSystem_functions.cpp",
    "Source/Shared.cpp",
    "Source/AresRuntime.cpp",
    "Source/Launch.cpp",
    "Source/Hooking/hde64.cpp",
    "Source/Hooking/Hooking.cpp",
    "Source/Modules/ConsoleGuard.cpp",
    "Source/Modules/Diagnostics.cpp",
    "Source/Modules/DeveloperConsole.cpp",
    "Source/Modules/LocalSettings.cpp",
    "Source/Modules/OfflineAuthority.cpp",
    "Source/Modules/PlatformEndpoint.cpp",
    "Source/Modules/ScreenCleanup.cpp",
    "Source/Modules/Freecam.cpp",
    "Source/Modules/VanguardBypass.cpp",
    "Source/Modules/Engine/World.cpp",
    "Source/Modules/Engine/NetDriver.cpp",
    "Source/Modules/ShooterGame/ShooterGameMode.cpp",
    "Source/Modules/ShooterGame/MatchFlow.cpp",
    "Source/Modules/ShooterGame/AresInventory.cpp",
    "Source/Modules/ShooterGame/WeaponGuards.cpp",
    "Source/Modules/ShooterGame/GameplayDiagnostics.cpp",
    "Source/Modules/ShooterGame/LocalEffects.cpp",
    "Source/Modules/ShooterGame/LocalDamage.cpp",
    "Source/Modules/ShooterGame/LocalSlowField.cpp",
    "Source/Modules/ShooterGame/LocalCharacterVisibility.cpp",
    "Source/Modules/ShooterGame/LocalSyncedTimers.cpp",
    "Source/Modules/ShooterGame/LocalForceModules.cpp",
    "Source/Modules/ShooterGame/LocalGameObjects.cpp",
    "Source/Modules/ShooterGame/LocalAbilityActivate.cpp",
    "Source/Modules/ShooterGame/LocalTeams.cpp",
    "Source/Modules/ShooterGame/LocalDeath.cpp",
    "Source/Modules/ShooterGame/LocalRadialDamage.cpp",
    "Source/Modules/ShooterGame/LocalBots.cpp",
    "Source/Modules/ShooterGame/MapSurvey.cpp",
    "Source/Modules/ShooterGame/TacticalMap.cpp",
    "Source/Modules/ShooterGame/RoundReferee.cpp",
    "Source/Modules/ShooterGame/LocalSkins.cpp",
    "Source/Modules/ShooterGame/CustomSkins.cpp",
    "Source/Modules/ShooterGame/BenjaminGeometry.cpp",
    "Source/Modules/ShooterGame/LocalDrop.cpp",
    "Source/Modules/ShooterGame/LocalInteractions.cpp",
    "Source/Modules/ShooterGame/Components/PlayerPurchaseablesComponent.cpp",
]

# sdk spams warnings
COMPILE_FLAGS = [
    "-std=c++23",
    "-O2",
    "-g",
    f"-I{ROOT}",
    "-DNDEBUG",
    "-Wno-pragma-pack",
    "-Wno-c++11-narrowing",
    "-Wno-elaborated-enum-class",
    "-Wno-implicitly-unsigned-literal",
    "-Wno-undefined-bool-conversion",
    "-Wno-return-type",
    "-Wno-invalid-offsetof",
    "-Wno-unused-variable",
    "-Wno-unused-parameter",
    "-Wno-microsoft-cast",
    "-Wno-deprecated-declarations",
]

LINK_FLAGS = [
    "-shared",
    "-lkernel32",
    "-luser32",
    "-lgdi32",
]


def zig() -> list[str]:
    return [sys.executable, "-m", "ziglang", "c++", "-target", TARGET]


def object_path(source: str) -> Path:
    return BUILD_DIR / (source.replace("/", "_").replace("\\", "_") + ".o")


def depfile_path(obj: Path) -> Path:
    return obj.with_suffix(obj.suffix + ".d")


def dependencies(obj: Path) -> list[Path]:
    """headers from the .d depfile"""
    dep = depfile_path(obj)
    if not dep.exists():
        return []

    text = dep.read_text(encoding="utf-8", errors="replace")
    text = text.replace("\\\n", " ").replace("\n", " ")
    # split on ": " not ":", drive letters
    parts = re.split(r":(?=\s)", text, maxsplit=1)
    if len(parts) != 2:
        return []
    return [Path(re.sub(r"\\([ #:\\])", r"\1", token))
            for token in re.findall(r"(?:\\.|[^\s])+", parts[1]) if token != "\\"]


def needs_rebuild(source: Path, obj: Path) -> bool:
    if not obj.exists():
        return True

    obj_time = obj.stat().st_mtime
    if obj_time < source.stat().st_mtime:
        return True

    deps = dependencies(obj)
    if not deps:
        return True
    for header in deps:
        try:
            if header.stat().st_mtime > obj_time:
                return True
        except OSError:
            return True
    return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--clean", action="store_true", help="remove object files first")
    args = parser.parse_args()

    try:
        version = subprocess.run(
            [sys.executable, "-m", "ziglang", "version"],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        print("zig toolchain missing. Install it with:  python -m pip install ziglang")
        return 1
    print(f"[*] zig {version}")

    if args.clean and BUILD_DIR.exists():
        for f in list(BUILD_DIR.glob("*.o")) + list(BUILD_DIR.glob("*.d")):
            f.unlink()
    BUILD_DIR.mkdir(exist_ok=True)

    objects: list[Path] = []
    compiled = 0
    started = time.time()

    for source in SOURCES:
        src_path = ROOT / source
        if not src_path.is_file():
            print(f"[!] missing source: {source}")
            return 1

        obj = object_path(source)
        objects.append(obj)

        if not needs_rebuild(src_path, obj):
            continue

        print(f"[*] cc {source}")
        result = subprocess.run(
            zig() + COMPILE_FLAGS + ["-MD", "-MF", str(depfile_path(obj)),
                                     "-c", str(src_path), "-o", str(obj)],
            cwd=ROOT,
        )
        if result.returncode != 0:
            print(f"[!] failed to compile {source}")
            return result.returncode
        compiled += 1

    up_to_date = (
        compiled == 0
        and OUTPUT.exists()
        and all(OUTPUT.stat().st_mtime >= o.stat().st_mtime for o in objects)
    )
    if up_to_date:
        print("[+] nothing to do; Ares.dll is up to date")
        return 0

    print("[*] link Ares.dll")
    result = subprocess.run(
        zig() + ["-o", str(OUTPUT)] + [str(o) for o in objects] + LINK_FLAGS,
        cwd=ROOT,
    )
    if result.returncode != 0:
        print("[!] link failed")
        return result.returncode

    size = OUTPUT.stat().st_size
    print(f"[+] {OUTPUT.name}: {size / 1024 / 1024:.1f} MB "
          f"({compiled} file(s) compiled in {time.time() - started:.0f}s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
