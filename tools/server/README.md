# micropatterns API — deployment

The API is a Deno app in `micropatterns_server/`, deployed to Deno Deploy and
backed by OVH S3.

| | |
|---|---|
| production URL | `https://micropatterns-api.juliendorra.deno.net` |
| org / app | `juliendorra` / `micropatterns-api` |
| storage | OVH S3, bucket `micropatterns`, `s3.rbx.io.cloud.ovh.net` (region `rbx`) |
| credentials | `envprod` at the repo root — **gitignored, never commit** |

```bash
tools/server/push-secrets.sh     # push envprod to the app (only affects the NEXT deploy)
tools/server/deploy.sh           # deploy to production, then health-check it
```

## Why the old URL died

`micropatterns-api.deno.dev` returned `404 DEPLOYMENT_NOT_FOUND` because **Deno
Deploy Classic was sunset on 2026-07-20**. The new platform issues
`<app>.<org>.deno.net`, so the URL changed and both clients had to be updated:
`micropatterns_emulator/simulator.js` and the M5Paper's
`network_manager.cpp` (`API_BASE_URL_DEFAULT`).

## Two traps that cost real time on 2026-08-27

**1. Run the deploy from `micropatterns_server/`, not the repo root.**
`deno deploy` writes a `deno.jsonc` holding `{org, app}` into the directory it
runs from. At the repo root that file has **no import map**, and it then shadows
`micropatterns_server/deno.jsonc`, so the server's bare specifiers (`std/http`,
`dotenv`, `s3_lite_client`) stop resolving. Both scripts here `cd` first.

**2. Use `Deno.serve()`, not `std/http`'s `serve()` with a fixed port.**
Deploy Classic wired the handler up for us, so a hardcoded `PORT = 8000` was
harmless. The current platform assigns the listener and probes it during the
**warming** step, and it does **not** set a `PORT` env var. An app binding 8000
starts perfectly, logs `Listening on http://localhost:8000/`, never answers the
probe, and the revision fails as `REVISION_FAILED` at `warming`.

That failure is nearly silent: the logs show a healthy startup and no error at
all. `deno deploy logs` also only streams **new** output, so it cannot show why a
past build failed — start the log stream first, then deploy into it.

## Storage layout

The `userID` is a secret, non-guessable string that acts as identity *and*
authentication — the same value is used by the editor (typed in, kept in
localStorage) and hardcoded in the firmware. There is no separate device ID and
no mapping table; the device list is just a different S3 key.

| request | S3 key |
|---|---|
| `GET /api/scripts/:userID` | `<userID>.json` — the full library (12) |
| `GET /api/device/scripts/:userID` | `<userID>-device.json` — the device subset (6) |
| `GET /api/scripts/:userID/:scriptID` | `scripts/<userID>/<scriptID>.json` |

A read-only mirror of the bucket taken 2026-08-27 is in `backups/2026-08-27-s3/`.
