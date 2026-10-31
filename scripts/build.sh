#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
# A failed or interrupted build must never leave a packageable older NSP.
STAMP="$ROOT/dist/.build-success"
mkdir -p "$ROOT/dist"
rm -f "$STAMP" "$ROOT/dist/switch-ambilight-v0.6.0-switch.zip"
rm -rf "$ROOT/dist/sd-root"
command -v docker >/dev/null || { echo 'Docker required' >&2; exit 1; }
command -v rsync >/dev/null || { echo 'rsync required (sudo apt install rsync)' >&2; exit 1; }
command -v git >/dev/null || { echo 'git required' >&2; exit 1; }
command -v python3 >/dev/null || { echo 'python3 required' >&2; exit 1; }
bash scripts/setup.sh
IMG="${DKP_IMG:-switch-ambilight-toolchain:1}"
if ! docker image inspect "$IMG" >/dev/null 2>&1; then
    echo "Building Docker toolchain $IMG"
    docker build -t "$IMG" .
fi
printf '\n== Sysmodule C20 ==\n'
DKP_IMG="$IMG" bash tier4/applet-mitm/build.sh 2>&1 | tee build-sysmodule.log
printf '\n== Launcher/donor C21, Tesla overlay and Homebrew manager ==\n'
docker run --rm --user "$(id -u):$(id -g)" \
  -e DEVKITPRO=/opt/devkitpro -e DEVKITA64=/opt/devkitpro/devkitA64 \
  -v "$ROOT":/work -w /work "$IMG" bash -ec '
  make -C tier4/donor -j2
  make -C ambilight-overlay -j2
  make -C ambilight-manager -j2
' 2>&1 | tee build-ui.log
for f in tier4/applet-mitm/applet-mitm.nsp tier4/donor/sft-donor.nsp \
         ambilight-overlay/switch-ambilight.ovl ambilight-manager/switch-ambilight.nro; do
    test -s "$f" || { echo "Missing: $f" >&2; exit 1; }
    ls -lh "$f"
done
# Save commit identity and output digests only after the entire build succeeds.
# Packaging refuses missing/stale stamps, even if older binary files still exist.
{
  git rev-parse --verify HEAD
  sha256sum tier4/applet-mitm/applet-mitm.nsp tier4/donor/sft-donor.nsp \
            ambilight-overlay/switch-ambilight.ovl ambilight-manager/switch-ambilight.nro
} > "$STAMP.tmp"
mv "$STAMP.tmp" "$STAMP"
printf '\nBuild complete. Run: bash scripts/package.sh\n'
