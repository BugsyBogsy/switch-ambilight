#!/usr/bin/env bash
# Stage a complete SD tree and zip. Never writes directly to a user's SD card.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
for f in tier4/applet-mitm/applet-mitm.nsp tier4/donor/sft-donor.nsp \
  ambilight-overlay/switch-ambilight.ovl ambilight-manager/switch-ambilight.nro; do
  [[ -s "$f" ]] || { echo "Missing $f. First run: bash scripts/build.sh" >&2; exit 1; }
done
STAGE="$ROOT/dist/sd-root"
rm -rf "$STAGE"
mkdir -p "$STAGE/atmosphere/contents/0100000000000C20" \
         "$STAGE/atmosphere/contents/0100000000000C21/flags" \
         "$STAGE/switch/.overlays" "$STAGE/switch/switch-ambilight"
cp tier4/applet-mitm/applet-mitm.nsp "$STAGE/atmosphere/contents/0100000000000C20/exefs.nsp"
cp tier4/donor/sft-donor.nsp "$STAGE/atmosphere/contents/0100000000000C21/exefs.nsp"
: > "$STAGE/atmosphere/contents/0100000000000C21/flags/boot2.flag"
cat > "$STAGE/atmosphere/contents/0100000000000C21/toolbox.json" <<'JSON'
{"name":"Switch Ambilight","tid":"0100000000000C21","requires_reboot":true}
JSON
cp ambilight-overlay/switch-ambilight.ovl "$STAGE/switch/.overlays/switch-ambilight.ovl"
cp ambilight-manager/switch-ambilight.nro "$STAGE/switch/switch-ambilight/switch-ambilight.nro"
# The package contains a sample separately so it never overwrites existing settings.
mkdir -p "$ROOT/dist"
cp config.example.ini "$ROOT/dist/config.example.ini"
(cd "$STAGE" && zip -qr "$ROOT/dist/switch-ambilight-v0.6.0-switch.zip" atmosphere switch)
printf 'Complete SD package: %s\n' "$ROOT/dist/switch-ambilight-v0.6.0-switch.zip"
printf 'No existing config.ini is overwritten. See README before installing.\n'
