#!/usr/bin/env bash
# Deploy micropatterns_server to Deno Deploy production.
#
#   tools/server/deploy.sh
#
# Env vars must already be on the app -- see push-secrets.sh.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ORG="${DENO_ORG:-juliendorra}"
APP="${DENO_APP:-micropatterns-api}"
DEPLOY_CONF="${DEPLOY_CONF:-$HOME/Documents/GitHub/esquisse/beta.deploy.conf}"

if [[ -z "${DENO_DEPLOY_TOKEN:-}" ]]; then
  DENO_DEPLOY_TOKEN="$(grep '^DENO_DEPLOY_TOKEN=' "$DEPLOY_CONF" | cut -d= -f2- | tr -d '"')"
  export DENO_DEPLOY_TOKEN
fi

# Run from micropatterns_server so its deno.jsonc (with the import map) is the
# config that gets used. See the note in push-secrets.sh.
cd "$REPO_ROOT/micropatterns_server"

deno deploy --json --non-interactive --org "$ORG" --app "$APP" --prod

echo
echo "==> verifying"
URL="https://${APP}.${ORG}.deno.net"
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 20 "$URL/api/device/scripts/kksh2hjtkb")
echo "    GET $URL/api/device/scripts/kksh2hjtkb -> $code"
[[ "$code" == "200" ]] || { echo "    deploy did not come up healthy" >&2; exit 1; }
