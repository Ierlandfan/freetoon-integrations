#!/bin/sh
# freetoon-integrations one-shot installer.
#
# Run on the Toon:
#   curl -fsSL https://raw.githubusercontent.com/Ierlandfan/freetoon-integrations/main/install.sh | sh -s -- <integration-id>
#
# Fetches the integration's tarball from the catalog, extracts to
# /mnt/data/integrations/<id>/, wires an inittab respawn row, HUPs init.

set -eu

CATALOG_URL=https://raw.githubusercontent.com/Ierlandfan/freetoon-integrations/main/catalog/index.json
INTEGRATIONS_DIR=/mnt/data/integrations
LOG=/var/volatile/tmp/integration_install.log

log() { echo "$(date '+%F %T') $*" | tee -a "$LOG"; }
die() { log "ERROR: $*"; exit 1; }

ID="${1:-}"
[ -n "$ID" ] || die "usage: $0 <integration-id>"

# Fetch the catalog and pull this integration's metadata. busybox awk
# can't parse JSON natively; rely on grep + sed for the bare-minimum
# fields we need. Brittle but avoids requiring jq on the Toon.
log "fetching catalog…"
CATALOG=$(curl -fsSL "$CATALOG_URL") || die "catalog fetch failed"

URL=$(printf '%s' "$CATALOG" | tr -d '\n' \
        | sed -n "s/.*\"id\":[ ]*\"$ID\"[^}]*\"url\":[ ]*\"\\([^\"]*\\)\".*/\\1/p")
[ -n "$URL" ] || die "integration '$ID' not in catalog"
SERVICE=$(printf '%s' "$CATALOG" | tr -d '\n' \
        | sed -n "s/.*\"id\":[ ]*\"$ID\"[^}]*\"service\":[ ]*\"\\([^\"]*\\)\".*/\\1/p")

log "installing '$ID' (service=$SERVICE) from $URL"

mkdir -p "$INTEGRATIONS_DIR/$ID"
cd "$INTEGRATIONS_DIR/$ID"

# Download + extract. The tarball is expected to expand into the current
# directory with manifest.json at top-level (no enclosing folder).
curl -fsSL "$URL" -o /tmp/integration.tgz || die "tarball fetch failed"
tar xzf /tmp/integration.tgz || die "tarball extract failed"
rm -f /tmp/integration.tgz

[ -f manifest.json ] || die "no manifest.json in tarball"
chmod +x "$INTEGRATIONS_DIR/$ID"/* 2>/dev/null || true

# Pull binary name + args from the manifest. Same grep/sed pattern.
BIN=$(sed -n 's/.*"binary":[ ]*"\([^"]*\)".*/\1/p' manifest.json)
[ -n "$BIN" ] || die "manifest missing binary field"
[ -x "$INTEGRATIONS_DIR/$ID/$BIN" ] || die "binary '$BIN' missing or not executable"

# Inittab row id is the first 4 chars of the integration id; collisions
# get a numeric suffix.
ROW_ID=$(echo "$ID" | tr -dc 'a-zA-Z0-9' | cut -c1-4)
LOG_PATH="/var/volatile/tmp/integration-$ID.log"
ROW="$ROW_ID:345:respawn:$INTEGRATIONS_DIR/$ID/$BIN >> $LOG_PATH 2>&1"

log "wiring inittab row '$ROW_ID' → $BIN"
grep -v "^${ROW_ID}:" /etc/inittab > /etc/inittab.new
echo "$ROW" >> /etc/inittab.new
mv -f /etc/inittab.new /etc/inittab

pkill -9 -f "$INTEGRATIONS_DIR/$ID/$BIN" 2>/dev/null || true
kill -HUP 1
log "installed. integration logs → $LOG_PATH"
log "toonui will pick up the new service automatically on its next BoxTalk subscribe cycle."
