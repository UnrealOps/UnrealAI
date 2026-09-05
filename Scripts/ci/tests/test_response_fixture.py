from __future__ import annotations

import importlib.util
import json
import unittest
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import ProxyHandler, Request, build_opener


SCRIPT = Path(__file__).resolve().parents[1] / "response_fixture.py"
SPEC = importlib.util.spec_from_file_location("response_fixture", SCRIPT)
FIXTURE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FIXTURE)


class ResponseFixtureTests(unittest.TestCase):
    def test_all_protocols_require_replay_metadata(self):
        for protocol in ("native", "chat", "anthropic", "gemini"):
            first = FIXTURE.response_for(protocol, {"tools": [{}], "store": False})
            self.assertTrue(FIXTURE.stream_frames(protocol, first))
            with self.assertRaises(ValueError):
                FIXTURE.response_for(protocol, {"tools": [{}], "store": False, "input": [{"type": "function_call_output"}]})
            second = FIXTURE.response_for(protocol, {"tools": [{}], "store": False,
                "input": [{"type": "function_call_output", "output": "fixture-signature"}]})
            self.assertIn("Fixture complete", json.dumps(second))

    def test_loopback_rejects_credentials_and_retries_once(self):
        # Explicitly disable developer HTTP proxy settings for a strictly local test.
        opener = build_opener(ProxyHandler({}))
        body = json.dumps({"store": False, "tools": [{}]}).encode()
        with FIXTURE.response_fixture() as port:
            url = f"http://127.0.0.1:{port}/retry/native/responses"
            with self.assertRaises(HTTPError) as failure:
                opener.open(Request(url, data=body), timeout=5)
            self.assertEqual(failure.exception.code, 503)
            failure.exception.close()
            with opener.open(Request(url, data=body), timeout=5) as response:
                self.assertEqual(json.load(response)["status"], "completed")
            with self.assertRaises(HTTPError) as credentials:
                opener.open(Request(url, data=body, headers={"Authorization": "fixture-forbidden"}), timeout=5)
            self.assertEqual(credentials.exception.code, 400)
            credentials.exception.close()

    def test_stream_has_native_terminal_marker(self):
        first = FIXTURE.response_for("native", {"tools": [{}], "store": False})
        frames = FIXTURE.stream_frames("native", first)
        self.assertEqual(frames[0]["type"], "response.created")
        self.assertEqual(frames[-1]["type"], "response.completed")
        self.assertEqual(frames[-1]["response"], first)


if __name__ == "__main__":
    unittest.main()
