#!/usr/bin/env python3
"""Fail CI when an Unreal automation JSON report is missing or unsuccessful."""

from __future__ import annotations

import json
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: check_automation_report.py <index.json>", file=sys.stderr)
        return 2

    report_path = Path(sys.argv[1])
    try:
        report = json.loads(report_path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        print(f"Unable to read Unreal automation report {report_path}: {exc}", file=sys.stderr)
        return 1

    tests = report.get("tests")
    if not isinstance(tests, list) or not tests:
        print("The Unreal automation report contains no tests.", file=sys.stderr)
        return 1

    failed_tests = [test for test in tests if test.get("state") == "Fail"]
    failed_count = int(report.get("failed", len(failed_tests)))
    incomplete_count = int(report.get("notRun", 0)) + int(report.get("inProcess", 0))

    if failed_count or failed_tests or incomplete_count:
        print(
            f"Unreal automation failed: {failed_count} failed, "
            f"{incomplete_count} incomplete, {len(tests)} total.",
            file=sys.stderr,
        )
        for test in failed_tests:
            print(f"  - {test.get('fullTestPath', test.get('testDisplayName', 'Unknown test'))}", file=sys.stderr)
        return 1

    succeeded = int(report.get("succeeded", 0))
    warnings = int(report.get("succeededWithWarnings", 0))
    print(f"Unreal automation passed: {succeeded} succeeded, {warnings} with warnings, {len(tests)} total.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
