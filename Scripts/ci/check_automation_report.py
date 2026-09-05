#!/usr/bin/env python3
"""Fail CI when an Unreal automation JSON report is missing or unsuccessful."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("report", type=Path)
    parser.add_argument(
        "--require",
        action="append",
        default=[],
        metavar="FULL_TEST_PATH",
        help="Require this exact Unreal automation test to appear and succeed.",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()

    report_path = arguments.report
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

    tests_by_path = {
        test.get("fullTestPath"): test
        for test in tests
        if isinstance(test.get("fullTestPath"), str)
    }
    missing_tests = sorted(set(arguments.require) - set(tests_by_path))
    unsuccessful_tests = sorted(
        test_name
        for test_name in arguments.require
        if test_name in tests_by_path and tests_by_path[test_name].get("state") != "Success"
    )
    if missing_tests or unsuccessful_tests:
        print("Unreal automation report does not satisfy its required contract:", file=sys.stderr)
        for test_name in missing_tests:
            print(f"  - missing: {test_name}", file=sys.stderr)
        for test_name in unsuccessful_tests:
            print(
                f"  - not successful: {test_name} ({tests_by_path[test_name].get('state', 'Unknown')})",
                file=sys.stderr,
            )
        return 1

    succeeded = int(report.get("succeeded", 0))
    warnings = int(report.get("succeededWithWarnings", 0))
    required = len(set(arguments.require))
    print(
        f"Unreal automation passed: {succeeded} succeeded, {warnings} with warnings, "
        f"{len(tests)} total, {required} required contract tests present."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
