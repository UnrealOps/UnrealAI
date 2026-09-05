from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "check_automation_report.py"


class CheckAutomationReportTests(unittest.TestCase):
    def run_checker(self, report: dict[str, object], *arguments: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory(prefix="unrealai-report-test-") as temporary_directory:
            report_path = Path(temporary_directory) / "index.json"
            report_path.write_text(json.dumps(report), encoding="utf-8")
            return subprocess.run(
                [sys.executable, str(SCRIPT), str(report_path), *arguments],
                check=False,
                capture_output=True,
                text=True,
            )

    @staticmethod
    def passing_report(test_name: str = "UnrealAI.Contract") -> dict[str, object]:
        return {
            "succeeded": 1,
            "succeededWithWarnings": 0,
            "failed": 0,
            "notRun": 0,
            "inProcess": 0,
            "tests": [
                {
                    "fullTestPath": test_name,
                    "testDisplayName": "Contract",
                    "state": "Success",
                }
            ],
        }

    def test_accepts_a_successful_required_contract(self) -> None:
        result = self.run_checker(
            self.passing_report(),
            "--require",
            "UnrealAI.Contract",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("1 required contract tests present", result.stdout)

    def test_rejects_a_missing_required_contract(self) -> None:
        result = self.run_checker(
            self.passing_report(),
            "--require",
            "UnrealAI.Missing",
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing: UnrealAI.Missing", result.stderr)

    def test_rejects_a_failed_report_before_contract_matching(self) -> None:
        report = self.passing_report()
        report["succeeded"] = 0
        report["failed"] = 1
        report["tests"][0]["state"] = "Fail"  # type: ignore[index]
        result = self.run_checker(report, "--require", "UnrealAI.Contract")
        self.assertEqual(result.returncode, 1)
        self.assertIn("1 failed", result.stderr)


if __name__ == "__main__":
    unittest.main()
