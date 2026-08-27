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
