"""Exercise trusted release validators outside the checkout they inspect."""
from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]


class ReleaseValidatorStagingTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        self.checkout = root / "checkout"
        self.checkout.mkdir()
        files = subprocess.check_output(
            ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
            cwd=REPOSITORY_ROOT,
        )
        for name in files.decode("utf-8").split("\0"):
            if not name or not (REPOSITORY_ROOT / name).is_file():
                continue
            destination = self.checkout / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPOSITORY_ROOT / name, destination)
        subprocess.run(["git", "init", "--quiet"], cwd=self.checkout, check=True)

        self.trusted_scripts = root / "runner" / "trusted-scripts"
        self.trusted_scripts.mkdir(parents=True)
        workflow = (REPOSITORY_ROOT / ".github/workflows/release.yml").read_text()
        staged_scripts = re.search(r"for script in ([^;]+); do", workflow)
        self.assertIsNotNone(staged_scripts)
        for name in staged_scripts.group(1).split():
            shutil.copy2(REPOSITORY_ROOT / "Scripts/ci" / name, self.trusted_scripts / name)

    def run_validator(self, name: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(self.trusted_scripts / name)],
            cwd=self.checkout,
            env={**os.environ, "UNREALAI_REPOSITORY_ROOT": str(self.checkout)},
            capture_output=True,
            text=True,
            check=False,
        )

    def test_staged_validators_accept_the_release_checkout(self) -> None:
        for name in ("validate_plugin.py", "validate_skills.py", "validate_release.py"):
            with self.subTest(validator=name):
                result = self.run_validator(name)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_staged_validator_rejects_dependency_in_the_release_checkout(self) -> None:
        rules = self.checkout / "Source/UnrealAI/UnrealAI.Build.cs"
        rules.write_text(rules.read_text().replace(
            '"Core",', '"Core", "ConsumerRuntime",', 1,
        ))
        result = self.run_validator("validate_plugin.py")
        self.assertNotEqual(0, result.returncode)
        self.assertIn("unapproved dependencies ['ConsumerRuntime']", result.stderr)


if __name__ == "__main__":
    unittest.main()
