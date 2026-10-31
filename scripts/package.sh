#!/usr/bin/env bash
# Stage a complete SD tree and zip. Never writes directly to a user's SD card.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
# Do not publish stale outputs from a failed compiler or an older commit.
OUT="$ROOT/dist/switch-ambilight-v0.6.0-switch.zip"
rm -f "$OUT"
STAMP="$ROOT/dist/.build-success"
if [[ ! -s "$STAMP" ]]; then
  echo "No successful build for this checkout. Run: bash scripts/build.sh" >&2
  exit 1
fi
read -r BUILD_COMMIT < "$STAMP"
if [[ "$BUILD_COMMIT" != "$(git rev-parse --verify HEAD)" ]]; then
  echo "Build belongs to another commit. Run: bash scripts/build.sh" >&2
  exit 1
fi
if ! tail -n +2 "$STAMP" | sha256sum --check --status; then
  echo "Build artifacts have changed since the successful build. Rebuild first." >&2
  exit 1
fi
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
if command -v zip >/dev/null 2>&1; then
    (cd "$STAGE" && zip -qr "$OUT" atmosphere switch)
else
    # Python is already required for the source/build tooling.
    python3 - "$STAGE" "$OUT" <<'PY'
from pathlib import Path
import sys
import zipfile

stage = Path(sys.argv[1])
output = Path(sys.argv[2])
with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    for item in sorted(stage.rglob("*")):
        if item.is_file():
            archive.write(item, item.relative_to(stage).as_posix())
PY
fi
printf 'Complete SD package: %s\n' "$ROOT/dist/switch-ambilight-v0.6.0-switch.zip"
printf 'No existing config.ini is overwritten. See README before installing.\n'
