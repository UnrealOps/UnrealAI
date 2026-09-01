# UnrealAI Blueprint API Reference

Use this reference with the Blueprint-exposed declarations in the current checkout. The public headers are authoritative if a node changes.

## One-shot async flow

Use this pattern for one request:

1. Add `Make Simple Chat Request` and enter the prompt. Leave `Model` empty to use the provider default.
2. Add `Create Chat Completion (UnrealAI)` and connect the request value.
3. Set `Provider Name` to `OpenAI`, `XAI`, a custom profile name, or `None` for the default profile.
4. Connect the async node's `Completed` execution path and `Response` value to `Get First Choice Content`.
5. Branch on `Has Content` before consuming the returned string.
6. Connect `Failed`, break the supplied `UnrealAIError`, and display or handle `Message` without exposing secrets.

The README illustration at `Documentation/Images/blueprint-chat-completion.png` shows the compact success path. It is an illustration, not a literal editor capture; add the failure branch in production graphs.

## Actor component flow

Use this pattern for repeated actor-owned prompts:

1. Add an `UnrealAIChatComponent` to the Actor Blueprint.
2. Set `Provider Name`, optional `Model`, optional `System Prompt`, and optional temperature in the Details panel.
3. Bind `On Chat Completed` and `On Chat Failed` before sending a request.
4. Call `Send Prompt` for a single user prompt.
5. Call `Send Messages` when supplying a prepared array of role-aware messages.
6. Use `Get First Choice Content` on successful responses and inspect `UnrealAIError.Message` on failures.

The component creates and retains its internal client. Do not create a second client for the same component flow.

## Node catalog

| Blueprint display name | Purpose | Important outputs |
| --- | --- | --- |
| `Make Chat Message` | Create a role and content pair | `UnrealAIChatMessage` |
| `Make Simple Chat Request` | Create a one-user-message request | `UnrealAIChatRequest` |
| `Create Chat Completion (UnrealAI)` | Send an asynchronous request | `Completed`, `Failed`, `Response`, `Error` |
| `Get First Choice Content` | Safely read the first assistant text | returned string, `Has Content` |
| `Make Json Object Response Format` | Request a JSON object | JSON string |
| `Make Strict Json Schema Response Format` | Request strict schema-constrained JSON | JSON string or empty string for invalid schema input |
| `Resolve Provider Config` | Resolve a settings profile | success boolean, provider config, error |
| `Reload Project Env File` | Reload local project-root `.env` values during editor testing | success boolean, variables loaded, status message |

`UnrealAIChatComponent` exposes `Send Prompt`, `Send Messages`, `On Chat Completed`, and `On Chat Failed`.

## Messages and advanced fields

`UnrealAIChatMessage` supports `System`, `Developer`, `User`, `Assistant`, and `Tool` roles. Use `Content` for ordinary text.

Use advanced JSON pins only when needed:

- `Content Json` replaces string content with a JSON value, including multimodal arrays.
- `Additional Fields Json` merges fields into one message object.
- `Response Format Json` sets the request's `response_format` object.
- `Additional Parameters Json` merges fields into the request root.

Invalid JSON fails request construction. Prefer the provided response-format helper nodes over manually typed JSON.

## Configuration and secrets

Provider profiles live under **Project Settings → Plugins → UnrealAI**. The built-in profiles use:

| Profile name | Default model | API key environment variable |
| --- | --- | --- |
| `OpenAI` | `gpt-5.6-luna` | `OPENAI_API_KEY` |
| `XAI` | `grok-4.3` | `XAI_API_KEY` |

For local editor development, copy `.env.example` to `.env` in the consuming project root. Process environment variables take precedence. Never put a real key into a Blueprint asset, screenshot, source-controlled setting, or packaged client.

## Blueprint verification

- Compile the Blueprint after placing or reconnecting nodes.
- Confirm `Completed` handles a response with content and a response with no choices.
- Confirm `Failed` handles configuration errors and provider errors.
- Keep automated plugin tests offline; the repository tests cover Blueprint reflection, request helpers, default profiles, and first-choice response handling without API keys.
- Exercise the native automation suite on each supported host through `Scripts/ci/run_unreal_ci.py` when changing the plugin itself.

The runtime graph is the same on macOS, Windows, and Linux. Only build and CI commands vary by host.
