#!/usr/bin/env python3
"""Report the committed CMake preset to use for the current OS.

The committed CMakePresets.json (at the repo root) is now the source of truth
for build presets. This helper no longer generates CMakeUserPresets.json; it is
a thin OS-detection convenience that maps the current OS to the right preset
name and prints the canonical build commands.

Usage:  python switch_build.py
"""

import platform

PRESETS = {
    "Windows": "conan-windows-release",
    "Linux": "conan-linux-release",
}


def main() -> None:
    os_name = platform.system()
    preset = PRESETS.get(os_name)
    if preset is None:
        raise SystemExit(f"Unsupported OS: {os_name}")

    print(f"OS:     {os_name}")
    print(f"Preset: {preset}")
    print()
    print("One-shot build:")
    print(f"  python build.py --preset {preset}")
    print()
    print("Manual steps:")
    print("  conan export external/ffmpeg")
    print("  conan install . --build=missing")
    print(f"  cmake --preset {preset}")
    print(f"  cmake --build --preset {preset}")
    print(f"  ctest --preset {preset}")


if __name__ == "__main__":
    main()
