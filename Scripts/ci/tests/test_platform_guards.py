"""Compile Mac-only translation units without Apple or Unreal headers."""
from __future__ import annotations

from pathlib import Path
import os
import shutil
import subprocess
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]


class PlatformGuardTests(unittest.TestCase):
    def test_mac_sources_compile_as_cpp_on_other_platforms(self) -> None:
        compiler = shutil.which(os.environ["CXX"]) if "CXX" in os.environ else (
            shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")
        )
        self.assertIsNotNone(compiler, "A C++ compiler is required for platform-guard validation.")
        msvc = Path(compiler).name.lower() in ("cl", "cl.exe")
        flags = ["/nologo", "/TP", "/Zs", "/X", "/W4", "/WX", "/we4668"] if msvc else (
            ["-x", "c++", "-std=c++17", "-fsyntax-only", "-nostdinc", "-Wundef", "-Werror"]
        )
        define = "/D" if msvc else "-D"
        undefine = "/U" if msvc else "-U"
        sources = sorted((REPOSITORY_ROOT / "Source").rglob("*.mm"))
        sources += sorted((REPOSITORY_ROOT / "Addons").rglob("*.mm"))
        self.assertTrue(sources, "Expected native Objective-C++ transport sources.")
        for platform in ("WINDOWS", "LINUX", "IOS"):
            for mac_defined in (False, True):
                mac_flag = f"{define}PLATFORM_MAC=0" if mac_defined else f"{undefine}PLATFORM_MAC"
                for source in sources:
                    with self.subTest(platform=platform, mac_defined=mac_defined,
                                      source=source.relative_to(REPOSITORY_ROOT)):
                        result = subprocess.run(
                            [compiler, *flags, mac_flag,
                             f"{define}PLATFORM_APPLE={int(platform == 'IOS')}",
                             f"{define}PLATFORM_{platform}=1", str(source)],
                            capture_output=True,
                            text=True,
                            check=False,
                        )
                        self.assertEqual(0, result.returncode, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
