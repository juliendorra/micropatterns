# Development helpers

## hunkpick.py — split one file's changes across several commits

`git add -p` is interactive and unusable from a non-interactive shell. This does
the same job by filtering a unified diff to the hunks that match (or do not
match) keywords, emitting a patch for `git apply --cached`.

Written because a single working tree held four unrelated concerns in
`main.cpp` at once — a wipe fix, an interrupt fix, a serial console and bench
wiring — and committing them together would have made every one of them
unreviewable.

```bash
# stage only the hunks mentioning clearAllScriptData
git diff -- path/to/file.cpp | tools/dev/hunkpick.py 'clearAllScriptData' | git apply --cached -

# stage everything EXCEPT those hunks
git diff -- path/to/file.cpp | tools/dev/hunkpick.py --drop 'clearAllScriptData' | git apply --cached -
```

It prints `kept N/M hunks` to stderr, and exits non-zero if the selection is
empty — so a typo'd keyword fails loudly instead of silently staging nothing.

Recompute `git diff` between calls: staging shifts the remaining line numbers.

## `serve.mjs`

Static file server for previewing `micropatterns_emulator/` locally.

    node tools/dev/serve.mjs 8777 micropatterns_emulator

Node rather than `python3 -m http.server` because that module calls
`os.getcwd()` at import time, which this machine's sandbox refuses. It also
serves `.wasm` with the right `application/wasm` type, which the Device (WASM)
execution path needs -- `WebAssembly.instantiateStreaming` rejects
`application/octet-stream`.

`.claude/launch.json` attaches the browser preview to it (start the server
first; the preview cannot spawn it, for the same cwd reason).
