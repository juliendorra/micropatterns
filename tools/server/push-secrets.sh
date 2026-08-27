#!/usr/bin/env bash
# Push the production environment (S3 credentials) to the Deno Deploy app.
#
#   tools/server/push-secrets.sh [envfile]     # default: ./envprod
#
# Reads the Deno Deploy token from esquisse's deploy conf, since that is where
# Julien's DENO_DEPLOY_TOKEN lives. Override with DENO_DEPLOY_TOKEN in the
# environment, or DEPLOY_CONF to point at a different conf file.
#
# The secrets are stored ONLY in the gitignored envfile locally and as Deno
# Deploy secrets remotely. They are never committed.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ENV_FILE="${1:-$REPO_ROOT/envprod}"
ORG="${DENO_ORG:-juliendorra}"
APP="${DENO_APP:-micropatterns-api}"
DEPLOY_CONF="${DEPLOY_CONF:-$HOME/Documents/GitHub/esquisse/beta.deploy.conf}"

[[ -f "$ENV_FILE" ]] || { echo "no env file at $ENV_FILE" >&2; exit 1; }

if [[ -z "${DENO_DEPLOY_TOKEN:-}" ]]; then
  [[ -f "$DEPLOY_CONF" ]] || { echo "no DENO_DEPLOY_TOKEN and no conf at $DEPLOY_CONF" >&2; exit 1; }
  DENO_DEPLOY_TOKEN="$(grep '^DENO_DEPLOY_TOKEN=' "$DEPLOY_CONF" | cut -d= -f2- | tr -d '"')"
  export DENO_DEPLOY_TOKEN
fi

echo "==> pushing $(grep -cE '^[A-Z_]+=' "$ENV_FILE") variables from $ENV_FILE to $ORG/$APP"

# IMPORTANT: run from micropatterns_server, not the repo root.
#
# `deno deploy` writes a deno.jsonc holding {org, app} into the directory it is
# run from. At the repo root that file has NO import map, and it then shadows
# micropatterns_server/deno.jsonc, so the server's bare specifiers
# (std/http, dotenv, s3_lite_client) stop resolving and the build fails.
cd "$REPO_ROOT/micropatterns_server"

deno deploy env load "$ENV_FILE" --json --non-interactive --org "$ORG" --app "$APP"

echo "==> variables now on the app (values hidden for secrets):"
deno deploy env list --json --non-interactive --org "$ORG" --app "$APP" \
  | python3 -c 'import json,sys
for x in json.load(sys.stdin):
    print(f"    {x[\"key\"]:20s} {\"<secret>\" if x.get(\"isSecret\") else x.get(\"value\")}")'

echo
echo "Env changes only take effect on the NEXT deploy:"
echo "    tools/server/deploy.sh"
