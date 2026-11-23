#!/usr/bin/env bash
# Build Companion for northwork.app/pocket/app/ and deploy the pocket-companion Worker.
# Needs CLOUDFLARE_API_TOKEN + CLOUDFLARE_ACCOUNT_ID in the environment.
set -euo pipefail
cd "$(dirname "$0")/.."
VITE_BASE=/pocket/app/ npm run build
rm -rf .northwork && mkdir -p .northwork/pocket/app
cp -R dist/. .northwork/pocket/app/
# SPA fallback: unknown /pocket/app/... paths load the app shell.
cp dist/index.html .northwork/index.html
# OTA discovery mirror for Pockets (same file the firmware-latest release carries).
curl -fsSL "https://github.com/NorthworkTech/Pocket/releases/download/firmware-latest/firmware-manifest.json" \
  -o .northwork/pocket/firmware-manifest.json || cp public/firmware-manifest.json .northwork/pocket/firmware-manifest.json
node northwork/build-worker.mjs
npx --yes wrangler@4 deploy --config northwork/wrangler.jsonc
