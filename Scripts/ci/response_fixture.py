#!/usr/bin/env python3
"""Credential-free, loopback-only Responses fixtures for compiled Unreal contracts."""

from __future__ import annotations

import json
import threading
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def response_for(protocol: str, payload: dict) -> dict:
    """Validate a two-step tool exchange before returning a deterministic response."""
    encoded = json.dumps(payload)
    is_followup = any(marker in encoded for marker in ("function_call_output", "tool_result", "functionResponse", '"role": "tool"'))
    if not payload.get("tools"):
        raise ValueError("Expected typed tools")
    if is_followup and "fixture-signature" not in encoded:
        raise ValueError("Provider replay data was lost")
    if protocol == "native":
        if payload.get("store") is not False or payload.get("background"):
            raise ValueError("Expected local foreground responses")
        output = [{"id": "message_2", "type": "message", "role": "assistant", "status": "completed",
                   "content": [{"type": "output_text", "text": "Fixture complete ✓"}]}] if is_followup else [
            {"id": "reason_1", "type": "reasoning", "encrypted_content": "fixture-signature"},
            {"id": "tool_1", "type": "function_call", "status": "completed", "call_id": "call_1",
             "name": "get_level_name", "arguments": "{}"}]
        return {"id": "response_2" if is_followup else "response_1", "status": "completed", "model": "fixture-model",
                "output": output, "usage": {"input_tokens": 3, "output_tokens": 4, "total_tokens": 7}}
    if protocol == "chat":
        message = {"role": "assistant", "content": "Fixture complete ✓"} if is_followup else {
            "role": "assistant", "content": None, "reasoning_content": "fixture-signature",
            "tool_calls": [{"id": "call_1", "type": "function", "function": {"name": "get_level_name", "arguments": "{}"}}]}
        return {"id": "chat_fixture", "choices": [{"index": 0, "message": message,
                "finish_reason": "stop" if is_followup else "tool_calls"}]}
    if protocol == "anthropic":
        content = [{"type": "text", "text": "Fixture complete ✓"}] if is_followup else [
            {"type": "thinking", "thinking": "opaque", "signature": "fixture-signature"},
            {"type": "tool_use", "id": "call_1", "name": "get_level_name", "input": {}}]
        return {"id": "msg_fixture", "type": "message", "role": "assistant", "content": content,
                "stop_reason": "end_turn" if is_followup else "tool_use", "usage": {"input_tokens": 3, "output_tokens": 4}}
    if protocol == "gemini":
        parts = [{"text": "Fixture complete ✓"}] if is_followup else [
            {"functionCall": {"id": "call_1", "name": "get_level_name", "args": {}},
             "thoughtSignature": "fixture-signature"}]
        return {"responseId": "gem_fixture", "candidates": [{"index": 0, "content": {"role": "model", "parts": parts},
                "finishReason": "STOP", "groundingMetadata": {"fixture": True}}]}
    raise ValueError("Unknown fixture protocol")


def stream_frames(protocol: str, response: dict) -> list[dict | str]:
    if protocol == "native":
        frames = [{"type": "response.created", "response": {"id": response["id"], "status": "in_progress", "output": []}}]
        for index, item in enumerate(response["output"]):
            start = dict(item)
            if item["type"] == "function_call":
                start["arguments"] = ""
            elif item["type"] == "message":
                start["content"] = []
            frames.append({"type": "response.output_item.added", "output_index": index, "item": start})
            if item["type"] == "function_call":
                for delta in ("{", "}"):
                    frames.append({"type": "response.function_call_arguments.delta", "output_index": index, "delta": delta})
            elif item["type"] == "message":
                frames.append({"type": "response.output_text.delta", "output_index": index, "content_index": 0, "delta": "Fixture "})
            frames.append({"type": "response.output_item.done", "output_index": index, "item": item})
        return [*frames, {"type": f"response.{response['status']}", "response": response}]
    if protocol == "chat":
        choice = response["choices"][0]
        message = choice["message"]
        delta = dict(message)
        for index, call in enumerate(delta.get("tool_calls", [])):
            call["index"] = index
        return [{"id": response["id"], "choices": [{"index": 0, "delta": delta}]},
                {"choices": [{"index": 0, "delta": {}, "finish_reason": choice["finish_reason"]}]}, "[DONE]"]
    if protocol == "anthropic":
        start = {**response, "content": [], "stop_reason": None}
        frames = [{"type": "message_start", "message": start}]
        for index, block in enumerate(response["content"]):
            frames.extend([{"type": "content_block_start", "index": index, "content_block": block},
                           {"type": "content_block_stop", "index": index}])
        return [*frames, {"type": "message_delta", "delta": {"stop_reason": response["stop_reason"]},
                         "usage": response["usage"]}, {"type": "message_stop"}]
    return [response]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args: object) -> None:
        pass  # Never log headers or bodies.

    def reply(self, status: int, body: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        if status == 503:
            self.send_header("Retry-After", "0")
        self.end_headers()
        # Split byte boundaries, including UTF-8, independently of SSE event boundaries.
        for index in range(0, len(body), 31):
            self.wfile.write(body[index:index + 31])
        self.wfile.flush()

    def do_POST(self) -> None:
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > 1024 * 1024:
                raise ValueError("Invalid request size")
            if self.headers.get("Authorization") or self.headers.get("x-api-key") or self.headers.get("x-goog-api-key"):
                raise ValueError("Fixture requests must not contain credentials")
            payload = json.loads(self.rfile.read(length))
            _, scenario, protocol, *_rest = self.path.split("/")
            key = (scenario, protocol)
            with self.server.fixture_lock:
                count = self.server.fixture_counts.get(key, 0)
                self.server.fixture_counts[key] = count + 1
            if scenario.startswith("retry") and count == 0:
                self.reply(503, b'{"error":{"message":"fixture transient failure","type":"overloaded_error"}}', "application/json")
                return
            response = response_for(protocol, payload)
            if scenario == "incomplete":
                response["status"] = "incomplete"
                response["incomplete_details"] = {"reason": "max_output_tokens"}
            elif scenario == "failure":
                response["status"] = "failed"
                response["error"] = {"code": "fixture_failure", "message": "Expected fixture failure"}
            streaming = bool(payload.get("stream")) or ":streamGenerateContent" in self.path
            if streaming:
                frames = stream_frames(protocol, response)
                if scenario == "cutoff":
                    frames = frames[:1]  # Valid lifecycle frame followed by EOF, no terminal marker.
                encoded = "".join("data: " + (frame if isinstance(frame, str) else json.dumps(frame, ensure_ascii=False)) + "\n\n"
                                  for frame in frames).encode("utf-8")
                self.reply(200, encoded, "text/event-stream")
            else:
                self.reply(200, json.dumps(response, ensure_ascii=False).encode("utf-8"), "application/json")
        except (ValueError, KeyError, TypeError):
            self.reply(400, b'{"error":{"message":"Invalid fixture request"}}', "application/json")
        except (BrokenPipeError, ConnectionResetError):
            pass  # Cancellation deliberately closes the request.


@contextmanager
def response_fixture():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    server.fixture_counts = {}
    server.fixture_lock = threading.Lock()
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server.server_port
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
