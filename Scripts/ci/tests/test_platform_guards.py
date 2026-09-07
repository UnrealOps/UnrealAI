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
        flags = ["/nologo", "/TP", "/Zs", "/X"] if msvc else (
            ["-x", "c++", "-std=c++17", "-fsyntax-only", "-nostdinc"]
        )
        define = "/D" if msvc else "-D"
        sources = sorted((REPOSITORY_ROOT / "Source").rglob("*.mm"))
        sources += sorted((REPOSITORY_ROOT / "Addons").rglob("*.mm"))
        self.assertTrue(sources, "Expected native Objective-C++ transport sources.")
        for platform in ("WINDOWS", "LINUX", "IOS"):
            for source in sources:
                with self.subTest(platform=platform, source=source.relative_to(REPOSITORY_ROOT)):
                    result = subprocess.run(
                        [compiler, *flags, f"{define}PLATFORM_MAC=0",
                         f"{define}PLATFORM_APPLE={int(platform == 'IOS')}",
                         f"{define}PLATFORM_{platform}=1", str(source)],
                        capture_output=True,
                        text=True,
                        check=False,
                    )
                    self.assertEqual(0, result.returncode, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
